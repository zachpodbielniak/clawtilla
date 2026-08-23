/*
 * clawt-desktop-relay.c - stdio MCP, from the agent's CLI into its VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-desktop-relay.h"

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
#define JSONRPC_METHOD_NOT_FOUND (-32601)

typedef struct {
    GMainLoop      *loop;
    GStrv           permitted;

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
 * Parses one line, or %NULL when it is not an object.
 *
 * Anything unparseable is passed through rather than dropped.  This sits
 * in the middle of somebody else's protocol, and a message shape neither
 * end has told us about is far more likely to be a version of MCP newer
 * than this file than an attack -- and swallowing it would surface as the
 * client hanging on a reply that never comes.
 */
static JsonNode *
parse_object(const gchar *line, JsonObject **object)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;

    *object = NULL;

    if (!json_parser_load_from_data(parser, line, -1, NULL))
        return NULL;

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;

    *object = json_object_ref(json_node_get_object(root));

    return NULL;
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
build_refusal(JsonObject *request, const gchar *tool)
{
    g_autoptr(JsonObject) reply = json_object_new();
    JsonObject *error = json_object_new();
    g_autofree gchar *message = NULL;

    json_object_set_string_member(reply, "jsonrpc", "2.0");
    json_object_set_member(reply, "id",
                           json_node_copy(json_object_get_member(request,
                                                                 "id")));

    message = g_strdup_printf(
        "clawtilla does not permit '%s' for this agent. Seeing the screen "
        "and acting on it are separate grants: turn on "
        "computer.desktop.allow_input (or allow_spawn) if this agent "
        "should have it.", tool);

    json_object_set_int_member(error, "code", JSONRPC_METHOD_NOT_FOUND);
    json_object_set_string_member(error, "message", message);
    json_object_set_object_member(reply, "error", error);

    return render(reply);
}

gboolean
clawt_desktop_relay_filter_outbound(const gchar  *line,
                                    GStrv         permitted,
                                    gchar       **refusal)
{
    g_autoptr(JsonObject) object = NULL;
    JsonObject *params;
    const gchar *tool;

    if (refusal != NULL)
        *refusal = NULL;

    parse_object(line, &object);

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
        *refusal = build_refusal(object, tool != NULL ? tool : "that tool");

    return FALSE;
}

gchar *
clawt_desktop_relay_filter_inbound(const gchar *line, GStrv permitted)
{
    g_autoptr(JsonObject) object = NULL;
    JsonObject *result;
    JsonArray *tools;
    JsonArray *kept;
    guint i;
    guint length;
    gboolean removed_any = FALSE;

    parse_object(line, &object);

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
            g_printerr("clawtilla: desktop relay: %s\n", error->message);

        direction_closed(relay);
        return;
    }

    if (stream == relay->from_client) {
        g_autofree gchar *refusal = NULL;

        if (clawt_desktop_relay_filter_outbound(line, relay->permitted,
                                                &refusal)) {
            if (!write_line(relay->to_guest, line)) {
                direction_closed(relay);
                return;
            }
        } else if (refusal != NULL) {
            /*
             * Answered here rather than forwarded: the guest would run
             * it, and the point is that it must not.
             */
            if (!write_line(relay->to_client, refusal)) {
                direction_closed(relay);
                return;
            }
        }
    } else {
        g_autofree gchar *filtered =
            clawt_desktop_relay_filter_inbound(line, relay->permitted);

        if (!write_line(relay->to_client, filtered)) {
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
clawt_desktop_relay_run(GStrv argv, GStrv permitted)
{
    Relay relay = { 0 };
    g_autoptr(GSubprocess) child = NULL;
    g_autoptr(GInputStream) stdin_stream = NULL;
    g_autoptr(GOutputStream) stdout_stream = NULL;
    g_autoptr(GDataInputStream) from_client = NULL;
    g_autoptr(GDataInputStream) from_guest = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(GError) error = NULL;

    g_return_val_if_fail(argv != NULL && argv[0] != NULL, EXIT_FAILURE);

    /*
     * The child's stderr is inherited rather than piped.  It carries ssh's
     * own diagnostics -- a refused connection, an unreachable host, a
     * changed host key -- and those are the messages somebody debugging
     * this needs to see, in the place they are already looking.
     */
    child = g_subprocess_newv((const gchar * const *)argv,
                              G_SUBPROCESS_FLAGS_STDIN_PIPE |
                              G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                              &error);

    if (child == NULL) {
        g_printerr("clawtilla: could not reach the guest's desktop: %s\n",
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
    relay.permitted = permitted;
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
     * client closes our stdin the ssh on the other side has no reason to
     * notice, and waiting on it would hang the relay for as long as the
     * VM stays up.
     */
    g_subprocess_force_exit(child);
    g_subprocess_wait(child, NULL, NULL);

    return EXIT_SUCCESS;
}
