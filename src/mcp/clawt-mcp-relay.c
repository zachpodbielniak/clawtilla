/*
 * clawt-mcp-relay.c - stdio MCP, with a tool filter in the middle
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mcp/clawt-mcp-relay.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <json-glib/json-glib.h>

/*
 * A screenshot comes back as base64 inside one JSON-RPC message, so a
 * single line here is routinely megabytes.  The default 4 KB buffer would
 * read one of those a fragment at a time.
 */
#define RELAY_BUFFER_BYTES (64 * 1024)

/*
 * JSON-RPC's code for a method the peer refused.  Chosen over a custom
 * code so a client that already knows the protocol reports something
 * sensible rather than an unknown number.
 */
#define JSONRPC_INVALID_REQUEST  (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)

typedef struct {
    GMainLoop      *loop;

    /*
     * Every decision about a message: the permitted list, whether it
     * applies at all, the per-call gate and the directory.  Kept as one
     * struct because the HTTP bridge makes the same decisions through
     * the same functions -- see ClawtMcpRelayPolicy.
     */
    const ClawtMcpRelayPolicy *policy;

    GDataInputStream  *from_client;
    GDataInputStream  *from_guest;
    GOutputStream     *to_client;
    GOutputStream     *to_guest;

    GSubprocess    *child;

    /*
     * Both directions have to finish before the loop stops.  Quitting on
     * the first EOF would cut off a reply that was still being written.
     */
    gint            open_directions;
    gint            status;
} Relay;

static gboolean
tool_is_permitted(GStrv permitted, const gchar *name)
{
    gsize i;

    if (permitted == NULL || name == NULL)
        return FALSE;

    for (i = 0; permitted[i] != NULL; i++) {
        if (g_strcmp0(permitted[i], name) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Parses one line into its top-level object, and says separately whether
 * the line was a JSON-RPC batch.
 *
 * Anything unparseable is passed through rather than dropped.  This sits
 * in the middle of somebody else's protocol, and a message shape neither
 * end has told us about is far more likely to be a version of MCP newer
 * than this file than an attack -- and swallowing it would surface as the
 * client hanging on a reply that never comes.
 *
 * A batch is not that shape, and the difference is the whole point.  An
 * array is valid JSON holding ordinary requests, so "we could not find an
 * object" read it as an unknown dialect and let it through untouched --
 * and this filter is the only place allow_input is enforced for a guest
 * desktop and the only place a connector's `tools:` grant is enforced at
 * all.  Two characters around a refused `tools/call` therefore permitted
 * it.  The comment above said the fail-open was deliberate, and it was:
 * it was written about forward compatibility, by somebody not thinking
 * about a security boundary, which is exactly why it survived.
 *
 * Batching is reported rather than filtered element by element.  Neither
 * the guest desktop nor any connector needs it, and a filter that has to
 * be right about nested shapes is a filter that will be wrong about the
 * next one.
 */
static void
parse_object(const gchar *line, JsonObject **object, gboolean *out_batch)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;

    *object = NULL;

    if (out_batch != NULL)
        *out_batch = FALSE;

    if (!json_parser_load_from_data(parser, line, -1, NULL))
        return;

    root = json_parser_get_root(parser);

    if (root == NULL)
        return;

    if (JSON_NODE_HOLDS_ARRAY(root)) {
        if (out_batch != NULL)
            *out_batch = TRUE;

        return;
    }

    if (!JSON_NODE_HOLDS_OBJECT(root))
        return;

    *object = json_object_ref(json_node_get_object(root));
}

static gchar *
render(JsonObject *object)
{
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

    json_node_set_object(node, object);
    json_generator_set_root(generator, node);

    /*
     * One line, because that is the framing: a pretty-printed message
     * would be read as several truncated ones.
     */
    json_generator_set_pretty(generator, FALSE);

    return json_generator_to_data(generator, NULL);
}

/*
 * A JSON-RPC error carrying the id the caller used, so the client matches
 * it to its own request rather than waiting for a reply for ever.
 */
static gchar *
build_refusal(JsonObject *request, const gchar *tool, const gchar *hint)
{
    g_autoptr(JsonObject) reply = json_object_new();
    JsonObject *error = json_object_new();
    g_autofree gchar *message = NULL;

    json_object_set_string_member(reply, "jsonrpc", "2.0");
    json_object_set_member(reply, "id",
                           json_node_copy(json_object_get_member(request,
                                                                 "id")));

    /*
     * The reason belongs to whoever imposed the restriction.
     *
     * This message used to name computer.desktop.allow_input, because
     * the relay began life serving guest desktops -- and when connectors
     * started using it, an agent refused a repository tool was told to
     * turn on a setting about seeing the screen.  Confidently wrong, and
     * about a completely different feature: the sort of thing that sends
     * somebody reading desktop documentation to fix a GitHub grant.
     */
    message = (hint != NULL)
        ? g_strdup_printf("clawtilla does not permit '%s' for this agent. %s",
                          tool, hint)
        : g_strdup_printf("clawtilla does not permit '%s' for this agent.",
                          tool);

    json_object_set_int_member(error, "code", JSONRPC_METHOD_NOT_FOUND);
    json_object_set_string_member(error, "message", message);
    json_object_set_object_member(reply, "error", error);

    return render(reply);
}

/*
 * A refusal for a batch, which has no single id to answer.
 *
 * JSON-RPC says a request whose id could not be determined is answered
 * with a null id, and a client waiting on the ids inside the batch is
 * better off seeing an error than seeing nothing.
 */
static gchar *
build_batch_refusal(void)
{
    g_autoptr(JsonObject) reply = json_object_new();
    JsonObject *error = json_object_new();

    json_object_set_string_member(reply, "jsonrpc", "2.0");
    json_object_set_null_member(reply, "id");

    json_object_set_int_member(error, "code", JSONRPC_INVALID_REQUEST);
    json_object_set_string_member(
        error, "message",
        "clawtilla does not relay batched requests: send one request per "
        "line, so each can be checked against what this agent is permitted "
        "to call.");
    json_object_set_object_member(reply, "error", error);

    return render(reply);
}

gchar *
clawt_mcp_relay_call_name(const gchar *line)
{
    g_autoptr(JsonObject) object = NULL;
    JsonObject *params;
    const gchar *tool;

    parse_object(line, &object, NULL);

    if (object == NULL)
        return NULL;

    if (g_strcmp0(json_object_get_string_member_with_default(object, "method",
                                                             ""),
                  "tools/call") != 0)
        return NULL;

    if (!json_object_has_member(object, "params") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "params")))
        return NULL;

    params = json_object_get_object_member(object, "params");
    tool = json_object_get_string_member_with_default(params, "name", NULL);

    return g_strdup(tool);
}

gchar *
clawt_mcp_relay_build_refusal(const gchar *line, const gchar *message)
{
    g_autoptr(JsonObject) object = NULL;
    g_autoptr(JsonObject) reply = NULL;
    JsonObject *error;

    parse_object(line, &object, NULL);

    /*
     * A notification -- no id -- expects no answer, so there is nothing
     * to refuse it with and dropping it is the whole of the response.
     */
    if (object == NULL || !json_object_has_member(object, "id"))
        return NULL;

    reply = json_object_new();
    error = json_object_new();

    json_object_set_string_member(reply, "jsonrpc", "2.0");
    json_object_set_member(reply, "id",
                           json_node_copy(json_object_get_member(object,
                                                                 "id")));
    json_object_set_int_member(error, "code", JSONRPC_METHOD_NOT_FOUND);
    json_object_set_string_member(error, "message", message);
    json_object_set_object_member(reply, "error", error);

    return render(reply);
}

gboolean
clawt_mcp_relay_filter_outbound(const gchar  *line,
                                GStrv         permitted,
                                const gchar  *hint,
                                gchar       **refusal)
{
    g_autoptr(JsonObject) object = NULL;
    JsonObject *params;
    const gchar *tool;
    gboolean batch = FALSE;

    if (refusal != NULL)
        *refusal = NULL;

    parse_object(line, &object, &batch);

    if (batch) {
        if (refusal != NULL)
            *refusal = build_batch_refusal();

        return FALSE;
    }

    if (object == NULL)
        return TRUE;

    if (g_strcmp0(json_object_get_string_member_with_default(object, "method",
                                                             ""),
                  "tools/call") != 0)
        return TRUE;

    if (!json_object_has_member(object, "params") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "params")))
        return TRUE;

    params = json_object_get_object_member(object, "params");
    tool = json_object_get_string_member_with_default(params, "name", NULL);

    if (tool_is_permitted(permitted, tool))
        return TRUE;

    /*
     * A notification -- no id -- expects no answer, so there is nothing to
     * refuse it with.  Dropping it is the whole of the response.
     */
    if (refusal != NULL && json_object_has_member(object, "id"))
        *refusal = build_refusal(object, tool != NULL ? tool : "that tool",
                                 hint);

    return FALSE;
}

gchar *
clawt_mcp_relay_filter_inbound(const gchar *line, GStrv permitted)
{
    g_autoptr(JsonObject) object = NULL;
    JsonObject *result;
    JsonArray *tools;
    JsonArray *kept;
    guint i;
    guint length;
    gboolean removed_any = FALSE;

    /*
     * A batch is passed through here, unlike outbound.  It is refused on
     * the way out, so a batched reply can only re-advertise a tool the
     * agent will still be refused when it calls it -- untidy, not a way
     * past the gate.
     */
    parse_object(line, &object, NULL);

    if (object == NULL)
        return g_strdup(line);

    if (!json_object_has_member(object, "result") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "result")))
        return g_strdup(line);

    result = json_object_get_object_member(object, "result");

    if (!json_object_has_member(result, "tools") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "tools")))
        return g_strdup(line);

    tools = json_object_get_array_member(result, "tools");
    length = json_array_get_length(tools);
    kept = json_array_new();

    for (i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(tools, i);
        JsonObject *tool;
        const gchar *name;

        if (!JSON_NODE_HOLDS_OBJECT(element)) {
            json_array_add_element(kept, json_node_copy(element));
            continue;
        }

        tool = json_node_get_object(element);
        name = json_object_get_string_member_with_default(tool, "name", NULL);

        if (tool_is_permitted(permitted, name)) {
            json_array_add_element(kept, json_node_copy(element));
            continue;
        }

        removed_any = TRUE;
    }

    if (!removed_any) {
        json_array_unref(kept);
        return g_strdup(line);
    }

    json_object_set_array_member(result, "tools", kept);

    return render(object);
}

void
clawt_mcp_relay_policy_outbound(const ClawtMcpRelayPolicy  *policy,
                                const gchar                *line,
                                gchar                     **to_server,
                                gchar                     **to_client)
{
    g_autofree gchar *rewritten = NULL;
    g_autofree gchar *tool = NULL;
    g_autofree gchar *refusal = NULL;
    const gchar *effective = line;

    *to_server = NULL;
    *to_client = NULL;

    g_return_if_fail(policy != NULL);

    /*
     * A batch is refused whenever anything here would look inside it,
     * not only when the permitted list applies.  The gate and the
     * directory read one call per line; a batch would carry its calls
     * past both, which for the gate is an approval nobody was asked for.
     */
    if (!policy->filter &&
        (policy->gate != NULL || policy->directory != NULL)) {
        g_autoptr(JsonObject) object = NULL;
        gboolean batch = FALSE;

        parse_object(line, &object, &batch);

        if (batch) {
            *to_client = build_batch_refusal();
            return;
        }
    }

    /*
     * The directory first, so everything after it sees the tool that
     * would actually run.  A search or a describe is answered here and
     * goes no further -- it runs nothing on the server.
     */
    if (policy->directory != NULL &&
        clawt_mcp_directory_rewrite_outbound(policy->directory, line,
                                             &rewritten, to_client)) {
        if (rewritten == NULL)
            return;

        effective = rewritten;
    }

    /*
     * The permitted list, again, on the inner call.  The directory was
     * built from the filtered list and only calls what it holds, so this
     * refuses nothing it would not -- and that is the point: the wall
     * and the door are checked from one list, not trusted to agree.
     */
    if (policy->filter &&
        !clawt_mcp_relay_filter_outbound(effective, policy->permitted,
                                         policy->hint, &refusal)) {
        *to_client = g_steal_pointer(&refusal);
        return;
    }

    /*
     * The permitted list said yes; the gate gets the question that
     * could not be answered when the relay started.
     *
     * Only for a real tools/call: everything else -- the handshake,
     * tools/list, a notification -- goes straight through, so a gate
     * that has to reach the daemon costs nothing on the messages that
     * make up most of the traffic.
     */
    if (policy->gate != NULL)
        tool = clawt_mcp_relay_call_name(effective);

    if (tool != NULL) {
        g_autofree gchar *denied = NULL;

        if (!policy->gate(tool, &denied, policy->gate_data)) {
            *to_client = clawt_mcp_relay_build_refusal(
                effective,
                (denied != NULL) ? denied
                                 : "clawtilla is not allowing that right "
                                   "now.");
            return;
        }
    }

    *to_server = g_strdup(effective);
}

void
clawt_mcp_relay_policy_inbound(const ClawtMcpRelayPolicy  *policy,
                               const gchar                *line,
                               gchar                     **to_client,
                               gchar                     **to_server)
{
    g_autofree gchar *filtered = NULL;

    *to_client = NULL;
    *to_server = NULL;

    g_return_if_fail(policy != NULL);

    filtered = policy->filter
        ? clawt_mcp_relay_filter_inbound(line, policy->permitted)
        : g_strdup(line);

    /*
     * After the filter, never before: the directory is built from what
     * reaches it, and what reaches it is what this agent may use.
     */
    if (policy->directory != NULL &&
        clawt_mcp_directory_rewrite_inbound(policy->directory, filtered,
                                            to_client, to_server))
        return;

    *to_client = g_steal_pointer(&filtered);
}

/*
 * Writes one framed message.
 *
 * Synchronous, and safe to be: the peer on each side is an MCP
 * implementation that reads continuously, and a message is one line.
 */
static gboolean
write_line(GOutputStream *stream, const gchar *line)
{
    g_autofree gchar *framed = g_strconcat(line, "\n", NULL);

    return g_output_stream_write_all(stream, framed, strlen(framed), NULL,
                                     NULL, NULL);
}

static void relay_read_next(Relay             *relay,
                            GDataInputStream  *source);

static void
direction_closed(Relay *relay)
{
    relay->open_directions--;

    if (relay->open_directions <= 0)
        g_main_loop_quit(relay->loop);
}

static void
on_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GDataInputStream *stream = G_DATA_INPUT_STREAM(source);
    Relay *relay = user_data;
    g_autofree gchar *line = NULL;
    g_autoptr(GError) error = NULL;
    gsize length = 0;

    line = g_data_input_stream_read_line_finish(stream, result, &length,
                                                &error);

    if (line == NULL) {
        if (error != NULL)
            g_printerr("clawtilla: relay: %s\n", error->message);

        /*
         * The client going away has to take the server with it.
         *
         * A stdio MCP server exits when its stdin closes, and nothing
         * else tells it to -- so without this the relay sat waiting for
         * a stdout that would never close, and the pair stayed alive for
         * ever after the agent that started them had gone.  For a
         * connector that means an abandoned process holding a live
         * credential; measured before the fix as a relay still running
         * two minutes after its client exited.
         */
        if (stream == relay->from_client && relay->to_guest != NULL)
            g_output_stream_close(relay->to_guest, NULL, NULL);

        direction_closed(relay);
        return;
    }

    {
        g_autofree gchar *to_client = NULL;
        g_autofree gchar *to_server = NULL;

        if (stream == relay->from_client)
            clawt_mcp_relay_policy_outbound(relay->policy, line, &to_server,
                                            &to_client);
        else
            clawt_mcp_relay_policy_inbound(relay->policy, line, &to_client,
                                           &to_server);

        /*
         * Answered here rather than forwarded when it is a refusal: the
         * guest would run it, and the point is that it must not.
         */
        if (to_client != NULL && !write_line(relay->to_client, to_client)) {
            direction_closed(relay);
            return;
        }

        if (to_server != NULL && !write_line(relay->to_guest, to_server)) {
            direction_closed(relay);
            return;
        }
    }

    relay_read_next(relay, stream);
}

static void
relay_read_next(Relay *relay, GDataInputStream *source)
{
    g_data_input_stream_read_line_async(source, G_PRIORITY_DEFAULT, NULL,
                                        on_line, relay);
}

gint
clawt_mcp_relay_run_policy(GStrv                      argv,
                           GStrv                      envp,
                           const ClawtMcpRelayPolicy *policy)
{
    Relay relay = { 0 };
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) child = NULL;
    g_autoptr(GInputStream) stdin_stream = NULL;
    g_autoptr(GOutputStream) stdout_stream = NULL;
    g_autoptr(GDataInputStream) from_client = NULL;
    g_autoptr(GDataInputStream) from_guest = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(GError) error = NULL;

    g_return_val_if_fail(argv != NULL && argv[0] != NULL, EXIT_FAILURE);
    g_return_val_if_fail(policy != NULL, EXIT_FAILURE);

    /*
     * The child's stderr is inherited rather than piped.  It carries ssh's
     * own diagnostics -- a refused connection, an unreachable host, a
     * changed host key -- and those are the messages somebody debugging
     * this needs to see, in the place they are already looking.
     */
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE);

    /*
     * Added to the inherited environment rather than replacing it.  The
     * server still needs PATH, HOME and the rest to run at all, and the
     * one variable being added here is the whole point of the relay.
     *
     * It is set on the child alone.  Putting it in the agent's .mcp.json
     * would leave it in a file the agent reads; putting it on this
     * process's own environment would leave it in /proc for anything
     * that thought to look.
     */
    if (envp != NULL) {
        gsize i;

        for (i = 0; envp[i] != NULL; i++) {
            g_auto(GStrv) pair = g_strsplit(envp[i], "=", 2);

            if (pair[0] != NULL && pair[1] != NULL)
                g_subprocess_launcher_setenv(launcher, pair[0], pair[1],
                                             TRUE);
        }
    }

    child = g_subprocess_launcher_spawnv(launcher,
                                         (const gchar * const *)argv,
                                         &error);

    if (child == NULL) {
        g_printerr("clawtilla: could not start the tool server: %s\n",
                   error->message);
        return EXIT_FAILURE;
    }

    stdin_stream = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    stdout_stream = g_unix_output_stream_new(STDOUT_FILENO, FALSE);

    from_client = g_data_input_stream_new(stdin_stream);
    from_guest =
        g_data_input_stream_new(g_subprocess_get_stdout_pipe(child));

    g_buffered_input_stream_set_buffer_size(
        G_BUFFERED_INPUT_STREAM(from_client), RELAY_BUFFER_BYTES);
    g_buffered_input_stream_set_buffer_size(
        G_BUFFERED_INPUT_STREAM(from_guest), RELAY_BUFFER_BYTES);

    loop = g_main_loop_new(NULL, FALSE);

    relay.loop = loop;
    relay.policy = policy;
    relay.from_client = from_client;
    relay.from_guest = from_guest;
    relay.to_client = stdout_stream;
    relay.to_guest = g_subprocess_get_stdin_pipe(child);
    relay.child = child;
    relay.open_directions = 2;

    relay_read_next(&relay, from_client);
    relay_read_next(&relay, from_guest);

    g_main_loop_run(loop);

    /*
     * The child is asked to go before it is waited for.  When the MCP
     * client closes our stdin the server on the other side has no reason
     * to notice -- an ssh into a VM certainly does not -- and waiting on
     * it would hang the relay for as long as that server stays up.
     */
    g_subprocess_force_exit(child);
    g_subprocess_wait(child, NULL, NULL);

    return EXIT_SUCCESS;
}

static gint
relay_run(GStrv argv, GStrv envp, GStrv permitted, gboolean filter,
          const gchar *hint, ClawtMcpRelayGate gate, gpointer gate_data)
{
    ClawtMcpRelayPolicy policy = { 0 };

    policy.permitted = permitted;
    policy.filter = filter;
    policy.hint = hint;
    policy.gate = gate;
    policy.gate_data = gate_data;

    return clawt_mcp_relay_run_policy(argv, envp, &policy);
}

gint
clawt_mcp_relay_run(GStrv argv, GStrv envp, GStrv permitted,
                    const gchar *hint)
{
    return relay_run(argv, envp, permitted, TRUE, hint, NULL, NULL);
}

gint
clawt_mcp_relay_run_gated(GStrv              argv,
                          GStrv              envp,
                          GStrv              permitted,
                          const gchar       *hint,
                          ClawtMcpRelayGate  gate,
                          gpointer           gate_data)
{
    return relay_run(argv, envp, permitted, TRUE, hint, gate, gate_data);
}

gint
clawt_mcp_relay_run_unfiltered_gated(GStrv              argv,
                                     GStrv              envp,
                                     ClawtMcpRelayGate  gate,
                                     gpointer           gate_data)
{
    return relay_run(argv, envp, NULL, FALSE, NULL, gate, gate_data);
}

gint
clawt_mcp_relay_run_unfiltered(GStrv argv, GStrv envp)
{
    return relay_run(argv, envp, NULL, FALSE, NULL, NULL, NULL);
}
