/*
 * clawt-connector-relay.c - Using a credential without holding it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "clawt-http.h"
#include "integration/clawt-connector-relay.h"
#include "integration/clawt-connector-accounts.h"
#include "integration/clawt-mcp-auth.h"

#include "mcp/clawt-mcp-relay.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <libsoup/soup.h>

#define USER_AGENT "clawtilla/" CLAWT_VERSION_STRING

/*
 * Why a tool is missing, in terms that make sense for a connector.
 *
 * Named here rather than shared with the desktop's: the two features
 * restrict tools for entirely different reasons, and a message covering
 * both would be true of neither.
 */
#define CONNECTOR_REFUSAL_HINT \
    "This connector was narrowed to a smaller set of tools, so the rest " \
    "are not yours to call. Ask whoever set it up if you need one of them."

/*
 * The same refusal in front of an integration broker, which has refusals
 * of its own.  Said as clawtilla's so the two read differently: the fix
 * for this one is in clawtilla's config, the gateway's in its dashboard.
 */
#define BROKER_REFUSAL_HINT \
    "clawtilla withheld this, not the integration broker: this connector " \
    "was narrowed to a smaller set of tools, so the rest are not yours to " \
    "call. Ask whoever set it up if you need one of them."

/* A tool result carrying a file or an image is routinely megabytes. */
#define RELAY_BUFFER_BYTES (64 * 1024)

/* ── Finding the server itself ───────────────────────────────────── */

gchar *
clawt_connector_resolve_command(const gchar *command, GError **error)
{
    g_autofree gchar *exe = NULL;
    g_autofree gchar *exe_dir = NULL;
    g_autofree gchar *beside = NULL;
    g_autofree gchar *installed = NULL;
    g_autofree gchar *on_path = NULL;

    g_return_val_if_fail(command != NULL, NULL);

    /*
     * A path the operator wrote themselves -- absolute, or carrying a
     * directory separator -- is not searched for; it is trusted, and
     * checked only for existing, exactly as an explicit `command:` on an
     * integration is trusted over the catalogue's own guess.
     */
    if (g_path_is_absolute(command) || strchr(command, G_DIR_SEPARATOR) != NULL) {
        if (g_file_test(command, G_FILE_TEST_IS_EXECUTABLE))
            return g_strdup(command);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "'%s' does not exist, or is not executable", command);
        return NULL;
    }

    exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe != NULL) {
        exe_dir = g_path_get_dirname(exe);
        beside = g_build_filename(exe_dir, command, NULL);

        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);
    }

    installed = g_build_filename(CLAWT_MCP_SERVER_DIR, command, NULL);

    if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE))
        return g_steal_pointer(&installed);

    on_path = g_find_program_in_path(command);

    if (on_path != NULL)
        return g_steal_pointer(&on_path);

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                "'%s' was not found beside clawtillad, in %s, or on PATH; "
                "install it and try again", command, CLAWT_MCP_SERVER_DIR);
    return NULL;
}

/* ── Building the plan ───────────────────────────────────────────── */

static void
wipe(gchar *secret)
{
    if (secret == NULL)
        return;

    memset((void *volatile)secret, 0, strlen(secret));
}

void
clawt_connector_plan_free(ClawtConnectorPlan *self)
{
    if (self == NULL)
        return;

    /*
     * The credential is in exactly one of these, and which one depends
     * on the connector -- so both are wiped rather than the one this
     * plan happened to use.
     */
    if (self->envp != NULL) {
        gsize i;

        for (i = 0; self->envp[i] != NULL; i++)
            wipe(self->envp[i]);
    }

    wipe(self->header_value);

    g_strfreev(self->argv);
    g_strfreev(self->envp);
    g_strfreev(self->permitted);
    g_free(self->name);
    g_free(self->account_prefix);
    g_strfreev(self->account_siblings);
    g_free(self->account_conflict);
    g_free(self->url);
    g_free(self->header_name);
    g_free(self->header_value);
    clawt_mcp_results_free(self->results);
    clawt_mcp_apps_free(self->apps);
    g_free(self->gateway);

    g_free(self);
}

ClawtApplyWrites
clawt_connector_apply_writes(ClawtIntegrationBinding *binding)
{
    const gchar *nick;
    gint value = CLAWT_APPLY_WRITES_OFF;

    g_return_val_if_fail(binding != NULL, CLAWT_APPLY_WRITES_OFF);

    nick = clawt_integration_binding_get_string(binding, "apply_writes");

    /*
     * Anything not recognised is `off`: a typo must never be the thing
     * that lets an agent write unreviewed.
     */
    if (nick == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_APPLY_WRITES, nick, &value))
        return CLAWT_APPLY_WRITES_OFF;

    return (ClawtApplyWrites)value;
}

ClawtConnectorPlan *
clawt_connector_plan_new(const ClawtConnectorInfo *info,
                         ClawtIntegrationBinding  *binding,
                         const gchar              *credential,
                         GError                  **error)
{
    g_autoptr(ClawtConnectorPlan) plan = g_new0(ClawtConnectorPlan, 1);
    g_autofree gchar *formatted = NULL;
    g_auto(GStrv) tools = NULL;
    const gchar *command;
    const gchar *url;
    const gchar *name;

    g_return_val_if_fail(info != NULL, NULL);
    g_return_val_if_fail(binding != NULL, NULL);

    plan->name = g_strdup(clawt_integration_binding_get_name(binding));

    /*
     * A broker's catalogue entry means a gateway answers, not the
     * service: its refusals are named as the gateway's.
     */
    {
        ClawtBrokerType broker;

        if (clawt_broker_type_for_provider(info->id, &broker))
            plan->gateway = g_strdup(clawt_broker_type_label(broker));
    }

    /*
     * The integration wins over the catalogue.  Somebody who wrote a
     * command into their own config meant it -- most often because they
     * are running a fork, or a version the catalogue predates.
     */
    command = clawt_integration_binding_get_string(binding, "command");
    url = clawt_integration_binding_get_string(binding, "url");

    if (command == NULL && url == NULL) {
        command = info->server_command;
        url = info->server_url;
    }

    if (command == NULL && url == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "connector '%s': %s has no tool server clawtilla knows "
                    "of; set command or url on the integration",
                    clawt_integration_binding_get_name(binding), info->id);
        return NULL;
    }

    if (command != NULL) {
        g_auto(GStrv) args = NULL;
        g_autofree gchar *resolved = clawt_connector_resolve_command(command,
                                                                     NULL);
        GPtrArray *argv = g_ptr_array_new();

        /*
         * Resolved to an absolute path when it can be, and left as the
         * bare name otherwise -- silently, since a spawned child's
         * environment is the allowlist and not this process's, and its
         * PATH may differ from the one just searched.  Falling back
         * keeps today's behaviour for a server that genuinely is on the
         * child's PATH even though it was not found here.
         */
        g_ptr_array_add(argv, resolved != NULL ? g_steal_pointer(&resolved)
                                               : g_strdup(command));

        args = clawt_integration_binding_get_string_list(binding, "args");

        if (args == NULL || args[0] == NULL) {
            g_strfreev(args);
            args = g_strdupv((GStrv)info->server_args);

            /*
             * `apply_writes: all` on a VENTURE connector is the server's
             * own switch, `venturectl mcp --apply-writes`: its writes
             * land at once instead of waiting in VENTURE's confirmation
             * queue.  Only on the catalogue's arguments -- somebody who
             * wrote `args:` wrote the whole command line, and adding to
             * it would be second-guessing them.
             */
            if (g_strcmp0(info->id, "venture") == 0 &&
                clawt_connector_apply_writes(binding) ==
                    CLAWT_APPLY_WRITES_ALL) {
                GPtrArray *with = g_ptr_array_new();
                gsize i;

                for (i = 0; args != NULL && args[i] != NULL; i++)
                    g_ptr_array_add(with, g_strdup(args[i]));

                g_ptr_array_add(with, g_strdup("--apply-writes"));
                g_ptr_array_add(with, NULL);
                g_strfreev(args);
                args = (GStrv)g_ptr_array_free(with, FALSE);
            }
        }

        if (args != NULL) {
            gsize i;

            for (i = 0; args[i] != NULL; i++)
                g_ptr_array_add(argv, g_strdup(args[i]));
        }

        g_ptr_array_add(argv, NULL);
        plan->argv = (GStrv)g_ptr_array_free(argv, FALSE);
    } else {
        plan->url = clawt_connector_resolve_url(
            info, url,
            clawt_integration_binding_get_string(binding, "instance"));
    }

    /*
     * The credential's name is overridable and its format is not.  A
     * service that wants its key in SOME_OTHER_VAR is one line of
     * config; a service that wants it spelled differently is a catalogue
     * entry, because getting `Bearer ` wrong produces a 401 that names
     * neither.
     */
    name = clawt_integration_binding_get_string(binding, "credential_name");

    if (name == NULL)
        name = info->credential_name;

    if (name == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "connector '%s': nothing says what to call the "
                    "credential; set credential_name",
                    clawt_integration_binding_get_name(binding));
        return NULL;
    }

    /*
     * No credential is only ever a remote server nobody has signed in to
     * yet.  A stdio server started without one would fail somewhere
     * inside itself, naming neither clawtilla nor the sign-in.
     */
    if (credential == NULL && plan->url == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "connector '%s' is not connected yet; run `clawtilla "
                    "connector connect %s`",
                    clawt_integration_binding_get_name(binding),
                    clawt_integration_binding_get_name(binding));
        return NULL;
    }

    if (credential == NULL) {
        plan->header_name = g_strdup(name);
    } else if (info->placement == CLAWT_CREDENTIAL_PLACEMENT_HEADER) {
        formatted = clawt_connector_format_credential(info, credential);
        plan->header_name = g_strdup(name);
        plan->header_value = g_steal_pointer(&formatted);
    } else {
        GPtrArray *envp = g_ptr_array_new();

        formatted = clawt_connector_format_credential(info, credential);

        g_ptr_array_add(envp, g_strdup_printf("%s=%s", name, formatted));

        /*
         * And which instance, for a stdio server that has to be told.
         *
         * An HTTP server needs nothing here -- the resolved instance is
         * the URL the relay dials.  A stdio server is a separate program
         * with its own idea of where the service lives, and every one of
         * them takes it from the environment; without this a connector
         * pointed at a second instance starts a server that quietly
         * talks to the first, which is the failure `default_instance`
         * exists to prevent one layer up.
         *
         * Resolved through the same function every URL field goes
         * through, so the instance an agent's tools reach and the
         * instance its credential was minted against cannot disagree.
         */
        if (info->instance_var != NULL) {
            g_autofree gchar *base = clawt_connector_resolve_url(
                info, "/",
                clawt_integration_binding_get_string(binding, "instance"));

            if (base != NULL) {
                gsize length = strlen(base);

                /* "/" made it a URL; the servers want the origin. */
                if (length > 0 && base[length - 1] == '/')
                    base[length - 1] = '\0';

                g_ptr_array_add(envp, g_strdup_printf("%s=%s",
                                                      info->instance_var,
                                                      base));
            }
        }

        g_ptr_array_add(envp, NULL);
        plan->envp = (GStrv)g_ptr_array_free(envp, FALSE);
    }

    /*
     * An empty list is not the same as no list, and the difference is
     * the whole tool surface: no list means every tool, and an empty
     * one would mean none.  A `tools:` key somebody left blank is
     * treated as absent rather than as a lockout they did not ask for.
     */
    tools = clawt_integration_binding_get_string_list(binding, "tools");

    if (tools != NULL && tools[0] != NULL) {
        /*
         * A pack that declares what its server offers lets a typo in
         * `tools:` be caught here instead of narrowing an agent down to
         * nothing and leaving nobody able to say why.  A pack that
         * declares nothing -- the ordinary case, since most entries have
         * no server anybody here has looked inside of -- disables the
         * check rather than treating every name as unrecognised.
         */
        if (info->known_tools != NULL) {
            gsize i;

            for (i = 0; tools[i] != NULL; i++) {
                gsize j;
                gboolean known = FALSE;

                for (j = 0; info->known_tools[j] != NULL; j++) {
                    if (g_strcmp0(tools[i], info->known_tools[j]) == 0) {
                        known = TRUE;
                        break;
                    }
                }

                if (!known)
                    g_warning("connector '%s': integration '%s' names tool "
                             "'%s', which %s is not known to offer",
                             info->id,
                             clawt_integration_binding_get_name(binding),
                             tools[i], info->name);
            }
        }

        plan->permitted = g_steal_pointer(&tools);
    }

    return g_steal_pointer(&plan);
}

/* ── What comes back, and what is said when nothing useful does ──── */

gchar *
clawt_connector_relay_body_text(GBytes *body)
{
    gconstpointer data;
    gsize length = 0;

    if (body == NULL)
        return g_strdup("");

    /*
     * Two statements, never `g_strndup(g_bytes_get_data(body, &length),
     * length)`: C leaves the order of a call's arguments to the
     * compiler, and gcc on x86-64 read `length` first -- still 0 -- so
     * every successful reply from every remote connector became an empty
     * string and was dropped without a word.  Refusals came another way
     * and worked, which is why it looked like the servers' fault.
     */
    data = g_bytes_get_data(body, &length);

    return g_strndup(data, length);
}

gchar *
clawt_connector_relay_failure_reply(const gchar *line,
                                    const gchar *text)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    JsonObject *request;
    JsonObject *reply;
    const gchar *method;

    g_return_val_if_fail(text != NULL, NULL);

    if (line == NULL || !json_parser_load_from_data(parser, line, -1, NULL))
        return NULL;

    if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        return NULL;

    request = json_node_get_object(json_parser_get_root(parser));

    /* A notification has nothing to answer into. */
    if (!json_object_has_member(request, "id"))
        return NULL;

    method = json_object_get_string_member_with_default(request, "method",
                                                        NULL);

    reply = json_object_new();
    json_object_set_string_member(reply, "jsonrpc", "2.0");
    json_object_set_member(reply, "id",
                           json_node_copy(json_object_get_member(request,
                                                                 "id")));

    /*
     * A tool call gets a failed tool *result*, because that is what a
     * model reads; anything else a JSON-RPC error with the same words.
     * Either way the request is answered: left unanswered, the client
     * waits on an id nothing will ever reply to, which an agent sees as
     * a tool that hangs.
     */
    if (g_strcmp0(method, "tools/call") == 0) {
        JsonObject *result = json_object_new();
        JsonArray *content = json_array_new();
        JsonObject *item = json_object_new();

        json_object_set_string_member(item, "type", "text");
        json_object_set_string_member(item, "text", text);
        json_array_add_object_element(content, item);
        json_object_set_array_member(result, "content", content);
        json_object_set_boolean_member(result, "isError", TRUE);
        json_object_set_object_member(reply, "result", result);
    } else {
        JsonObject *error = json_object_new();

        json_object_set_int_member(error, "code", -32603);
        json_object_set_string_member(error, "message", text);
        json_object_set_object_member(reply, "error", error);
    }

    root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, reply);
    generator = json_generator_new();
    json_generator_set_root(generator, root);

    return json_generator_to_data(generator, NULL);
}

/*
 * Whether @body is a JSON-RPC message of the server's own, which is
 * passed through whatever the status: plenty of servers send a JSON-RPC
 * error with a 400 or a 500, and it says more than anything said here.
 */
static gboolean
is_jsonrpc(const gchar *content_type, const gchar *body)
{
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;

    if (body == NULL || *body == '\0')
        return FALSE;

    if (content_type != NULL && strstr(content_type, "text/event-stream"))
        return strstr(body, "data:") != NULL;

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, body, -1, NULL))
        return FALSE;

    root = json_parser_get_root(parser);

    return root != NULL && JSON_NODE_HOLDS_OBJECT(root) &&
           json_object_has_member(json_node_get_object(root), "jsonrpc");
}

/* ── An HTTP server, spoken to over stdio ────────────────────────── */

/*
 * The agent's CLI speaks MCP over stdin and stdout; some services offer
 * it only over HTTP.  Rather than let the CLI dial those directly -- which
 * would mean putting the credential in a `headers` block of the agent's
 * own .mcp.json, the exact leak this file exists to prevent -- each
 * message is forwarded here, with the header added on the way past.
 */
typedef struct {
    SoupSession               *session;
    ClawtConnectorPlan        *plan;
    const ClawtMcpRelayPolicy *policy;
    gchar                     *session_id;
    GOutputStream             *out;

    /*
     * Whether every request goes out through the egress gate, so a
     * refusal can say which setting would let it through.
     */
    gboolean                   gated;

    /*
     * How deep forward_one() has recursed for pages the directory asked
     * for itself.  The directory stops after its own page limit; this
     * is the backstop that keeps a misbehaving server from turning that
     * into a stack overflow.
     */
    guint                      depth;

    /* Messages read off responses so far, so a broken body is noticed. */
    guint                      delivered;
} HttpBridge;

static void
write_line(GOutputStream *out, const gchar *line)
{
    g_output_stream_write_all(out, line, strlen(line), NULL, NULL, NULL);
    g_output_stream_write_all(out, "\n", 1, NULL, NULL, NULL);
    g_output_stream_flush(out, NULL, NULL);
}

/*
 * A streamable-HTTP server may answer either with one JSON object or
 * with an SSE stream carrying several.  Both are handled, because which
 * one arrives is the server's choice per request and not a property of
 * the server -- a client that understands only JSON works until the
 * first tool call that streams.
 */
static void forward_one(HttpBridge *bridge, const gchar *line);

/*
 * One message from the server, through the same decision the stdio
 * relay makes: filtered by the permitted list, then seen by the
 * directory, which may answer the client with a gathered list or ask the
 * server for the next page of one.
 */
static void
deliver_one(HttpBridge *bridge, const gchar *data)
{
    g_autofree gchar *to_client = NULL;
    g_autofree gchar *to_server = NULL;

    clawt_mcp_relay_policy_inbound(bridge->policy, data, &to_client,
                                   &to_server);

    /*
     * An error the gateway sent is named as the gateway's, so the agent
     * and whoever reads its transcript can tell "AnythingMCP refused it"
     * from a refusal clawtilla made on the way.
     */
    if (to_client != NULL && bridge->plan->gateway != NULL) {
        gchar *named = clawt_broker_annotate_reply(to_client,
                                                   bridge->plan->gateway);

        if (named != NULL) {
            g_free(to_client);
            to_client = named;
        }
    }

    if (to_client != NULL)
        write_line(bridge->out, to_client);

    if (to_server != NULL && bridge->depth < 128) {
        bridge->depth++;
        forward_one(bridge, to_server);
        bridge->depth--;
    }
}

/*
 * One message read off a response, whole or described.  A message too
 * big to hold becomes the budget's reply, which then goes through the
 * policy like any other.
 */
static void
on_response_message(const gchar             *message,
                    gsize                    length,
                    const ClawtMcpOversized *oversized,
                    gpointer                 user_data)
{
    HttpBridge *bridge = user_data;
    g_autofree gchar *line = NULL;

    (void)length;

    if (message != NULL) {
        if (*message != '\0') {
            bridge->delivered++;
            deliver_one(bridge, message);
        }
        return;
    }

    line = clawt_mcp_relay_oversized_line(bridge->policy, oversized);
    bridge->delivered++;

    if (line != NULL)
        deliver_one(bridge, line);
}

/*
 * A streamable-HTTP server may answer either with one JSON object or
 * with an SSE stream carrying several.  Both are handled, because which
 * one arrives is the server's choice per request and not a property of
 * the server -- a client that understands only JSON works until the
 * first tool call that streams.
 *
 * Read a chunk at a time through a bounded reader.  The bridge used to
 * read the whole body and look at it afterwards, so a server answering
 * with a gigabyte had it held in memory before anything here could say
 * it was too big -- a limit applied after the read.
 */
static void
deliver_stream(HttpBridge *bridge, const gchar *line,
               const gchar *content_type, const gchar *head,
               gsize head_length, GInputStream *body)
{
    g_autoptr(ClawtMcpReader) reader = NULL;
    g_autofree gchar *chunk = g_malloc(RELAY_BUFFER_BYTES);
    ClawtMcpFraming framing = CLAWT_MCP_FRAMING_WHOLE;
    guint before = bridge->delivered;
    gboolean failed = FALSE;

    if (content_type != NULL && strstr(content_type, "text/event-stream"))
        framing = CLAWT_MCP_FRAMING_SSE;

    reader = clawt_mcp_reader_new(
        framing, CLAWT_MCP_RESULT_HOLD_MAX,
        (bridge->policy->results != NULL)
            ? clawt_mcp_results_get_spill_dir(bridge->policy->results)
            : NULL,
        on_response_message, bridge);

    /* What was already read to judge the answer goes first. */
    if (head != NULL && head_length > 0)
        clawt_mcp_reader_feed(reader, head, head_length);

    for (;;) {
        g_autoptr(GError) error = NULL;
        gssize got = g_input_stream_read(body, chunk, RELAY_BUFFER_BYTES,
                                         NULL, &error);

        if (got < 0) {
            g_printerr("clawtilla: reading the tool server's answer "
                       "failed: %s\n", error->message);
            failed = TRUE;
            break;
        }

        if (got == 0)
            break;

        clawt_mcp_reader_feed(reader, chunk, (gsize)got);
    }

    clawt_mcp_reader_finish(reader);

    /*
     * Every request is answered.  A body that broke off before any
     * message in it was whole leaves the client waiting on an id
     * nothing will answer -- an agent sees a tool that hangs -- so it
     * gets the same tool error a request that never arrived does.
     */
    if (failed && bridge->delivered == before) {
        g_autofree gchar *said = g_strdup_printf(
            "The connector '%s' tool server's answer broke off before it "
            "was complete, so nothing it said can be trusted to have run "
            "or not.",
            bridge->plan->name != NULL ? bridge->plan->name : "?");
        g_autofree gchar *reply =
            clawt_connector_relay_failure_reply(line, said);

        if (reply != NULL)
            write_line(bridge->out, reply);
    }
}

static void
forward_one(HttpBridge *bridge, const gchar *line)
{
    g_autoptr(SoupMessage) message = NULL;
    g_autoptr(GInputStream) response = NULL;
    g_autoptr(GBytes) request = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *head = NULL;
    SoupMessageHeaders *headers;
    const gchar *content_type;
    const gchar *fresh_session;
    gsize head_length = 0;

    message = clawt_http_message_new("POST", bridge->plan->url,
                                     CLAWT_HTTP_CARRIES_CREDENTIALS);

    if (message == NULL) {
        g_printerr("clawtilla: '%s' is not a URL that can be dialled\n",
                   bridge->plan->url);
        return;
    }

    request = g_bytes_new(line, strlen(line));
    soup_message_set_request_body_from_bytes(message, "application/json",
                                             request);

    headers = soup_message_get_request_headers(message);

    soup_message_headers_replace(headers, "Accept",
                                 "application/json, text/event-stream");

    if (bridge->plan->header_value != NULL)
        soup_message_headers_replace(headers, bridge->plan->header_name,
                                     bridge->plan->header_value);

    /*
     * A streamable-HTTP server hands out a session id on initialize and
     * refuses every later request that does not carry it back.  Without
     * this the relay works exactly once per process, which reads as the
     * server being broken after the handshake.
     */
    if (bridge->session_id != NULL)
        soup_message_headers_replace(headers, "Mcp-Session-Id",
                                     bridge->session_id);

    /*
     * The body as a stream, so it is bounded while it is read; the
     * redirect refusal clawt_http_send_and_read() makes is made here
     * too, on the headers, before a byte of the body is taken.
     */
    response = soup_session_send(bridge->session, message, NULL, &error);

    if (response != NULL && !clawt_http_check_redirect(message, &error))
        g_clear_object(&response);

    if (response == NULL) {
        /*
         * A gateway that is not answering gets a reply naming it; any
         * other server one naming the connector.  Without one the client
         * waits on a request id nothing will ever answer, which an agent
         * sees as a tool that hangs.
         */
        if (bridge->plan->gateway != NULL) {
            g_autofree gchar *reply = clawt_broker_refusal_reply(
                line, bridge->plan->gateway,
                bridge->plan->name != NULL ? bridge->plan->name : "?", 0,
                error->message);

            if (reply != NULL)
                write_line(bridge->out, reply);
        } else {
            g_autofree gchar *said = g_strdup_printf(
                "The connector '%s' could not reach its tool server at "
                "%s: %s.%s",
                bridge->plan->name != NULL ? bridge->plan->name : "?",
                bridge->plan->url, error->message,
                bridge->gated
                    ? " This agent's network is gated, so the request went "
                      "through clawtilla's egress gate; a server on this "
                      "machine or the local network is reachable only when "
                      "the person adds its address to egress.allow."
                    : "");
            g_autofree gchar *reply =
                clawt_connector_relay_failure_reply(line, said);

            if (reply != NULL)
                write_line(bridge->out, reply);
        }

        g_printerr("clawtilla: the request to the tool server failed: %s\n",
                   error->message);
        return;
    }

    /*
     * Anything a gateway refuses, a 401 included: there is no sign-in to
     * offer for a broker's connector, only a key to store, and the
     * refusal says which `clawtilla broker key` stores it.
     */
    if (bridge->plan->gateway != NULL &&
        soup_message_get_status(message) >= 400) {
        g_autofree gchar *body = g_malloc0(RELAY_BUFFER_BYTES + 1);
        g_autofree gchar *reason = NULL;
        g_autofree gchar *reply = NULL;
        gsize got = 0;

        /*
         * A refusal's reason is a sentence: read at most a buffer of it,
         * the body is a stream now and a hostile gateway's "error" can be
         * as long as it likes.
         */
        (void)g_input_stream_read_all(response, body, RELAY_BUFFER_BYTES,
                                      &got, NULL, NULL);
        body[got] = '\0';
        reason = clawt_broker_reason_from_body(body);
        reply = clawt_broker_refusal_reply(
            line, bridge->plan->gateway,
            bridge->plan->name != NULL ? bridge->plan->name : "?",
            soup_message_get_status(message), reason);

        if (reply != NULL)
            write_line(bridge->out, reply);

        g_printerr("clawtilla: %s answered %u for connector '%s'\n",
                   bridge->plan->gateway, soup_message_get_status(message),
                   bridge->plan->name != NULL ? bridge->plan->name : "?");
        return;
    }

    /*
     * A 401 is answered here rather than passed through.  Passed through,
     * it starts the agent CLI's own sign-in, which cannot work -- the
     * relay holds the credential, not the CLI -- and the CLI reports only
     * that the server failed.  A server that lists tools to anybody and
     * refuses only tool calls made this invisible until the first call;
     * so a refused tools/call becomes a tool error naming the connector
     * and the sign-in, and the daemon is told so a client can offer one.
     */
    if (soup_message_get_status(message) == SOUP_STATUS_UNAUTHORIZED) {
        const gchar *challenge = soup_message_headers_get_one(
            soup_message_get_response_headers(message), "WWW-Authenticate");
        g_autofree gchar *reply = clawt_mcp_auth_unauthorized_reply(
            line, bridge->plan->name != NULL ? bridge->plan->name : "?",
            bridge->plan->header_value != NULL);

        if (reply != NULL)
            write_line(bridge->out, reply);

        if (bridge->plan->unauthorized != NULL)
            bridge->plan->unauthorized(bridge->plan->name, challenge,
                                       bridge->plan->header_value != NULL,
                                       bridge->plan->unauthorized_data);

        g_printerr("clawtilla: %s answered 401; the connector '%s' needs "
                   "a sign-in\n", bridge->plan->url,
                   bridge->plan->name != NULL ? bridge->plan->name : "?");
        return;
    }

    content_type = soup_message_headers_get_content_type(
        soup_message_get_response_headers(message), NULL);

    /*
     * A failure status is judged on its first buffer, read bounded: a
     * JSON-RPC error is a sentence, and an HTML error page or a gate's
     * refusal is answered without reading the rest of it.  A success is
     * never read here -- it goes straight to the bounded reader.
     */
    if (soup_message_get_status(message) >= 400) {
        gsize got = 0;

        head = g_malloc0(RELAY_BUFFER_BYTES + 1);
        (void)g_input_stream_read_all(response, head, RELAY_BUFFER_BYTES,
                                      &got, NULL, NULL);
        head[got] = '\0';
        head_length = got;
        text = g_strdup(head);
    }

    /*
     * A refusal that is not the server's own JSON-RPC -- the egress
     * gate's 403, a proxy's 502, an HTML error page -- is answered here.
     * Passed through, it is not a message the client can parse, and the
     * request it belonged to was never answered at all.
     */
    if (soup_message_get_status(message) >= 400 &&
        !is_jsonrpc(content_type, text)) {
        const gchar *gate = soup_message_headers_get_one(
            soup_message_get_response_headers(message),
            "X-Clawtilla-Egress");
        g_autofree gchar *said = NULL;
        g_autofree gchar *reply = NULL;

        /*
         * The gate's own answer carries its reason in a header.  Only a
         * 403 is a decision egress.allow can change; a 502 is the gate
         * failing to reach a server it was allowed to.
         */
        if (gate != NULL && soup_message_get_status(message) == 403)
            said = g_strdup_printf(
                "clawtilla's egress gate refused the connector '%s' "
                "(%s): %s. This agent's network is gated; if %s is "
                "meant to be reachable, the person adds its address to "
                "egress.allow.",
                bridge->plan->name != NULL ? bridge->plan->name : "?",
                bridge->plan->url, gate, bridge->plan->url);
        else if (gate != NULL)
            said = g_strdup_printf(
                "clawtilla's egress gate could not reach the connector "
                "'%s' tool server (%s): %s.",
                bridge->plan->name != NULL ? bridge->plan->name : "?",
                bridge->plan->url, gate);
        else
            said = g_strdup_printf(
                "The connector '%s' tool server answered HTTP %u, so "
                "nothing ran.",
                bridge->plan->name != NULL ? bridge->plan->name : "?",
                soup_message_get_status(message));

        reply = clawt_connector_relay_failure_reply(line, said);

        if (reply != NULL)
            write_line(bridge->out, reply);

        g_printerr("clawtilla: %s answered %u for connector '%s'%s%s\n",
                   bridge->plan->url, soup_message_get_status(message),
                   bridge->plan->name != NULL ? bridge->plan->name : "?",
                   gate != NULL ? ": " : "", gate != NULL ? gate : "");
        return;
    }

    fresh_session = soup_message_headers_get_one(
        soup_message_get_response_headers(message), "Mcp-Session-Id");

    if (fresh_session != NULL) {
        g_free(bridge->session_id);
        bridge->session_id = g_strdup(fresh_session);
    }

    /*
     * A notification has no id and gets 202 with nothing in it.  The
     * reader delivers nothing for an empty body, so no empty line goes
     * on the wire for a message the client never asked for.
     */
    deliver_stream(bridge, line, content_type, head, head_length, response);
}

/*
 * The bridge's loop, over whatever streams and session it is handed:
 * stdin, stdout and a session of its own in the relay process, a
 * session pointed at a unix socket and memory streams in a test.
 */
static gint
run_http_on(ClawtConnectorPlan        *plan,
            const ClawtMcpRelayPolicy *policy,
            SoupSession               *session,
            gboolean                   gated,
            GInputStream              *in,
            GOutputStream             *out)
{
    HttpBridge bridge = { 0 };
    g_autoptr(GDataInputStream) reader = NULL;

    reader = g_data_input_stream_new(in);

    g_buffered_input_stream_set_buffer_size(
        G_BUFFERED_INPUT_STREAM(reader), RELAY_BUFFER_BYTES);

    bridge.session = session;
    bridge.plan = plan;
    bridge.policy = policy;
    bridge.out = out;
    bridge.gated = gated;

    for (;;) {
        g_autofree gchar *line = NULL;
        g_autofree gchar *to_server = NULL;
        g_autofree gchar *to_client = NULL;

        line = g_data_input_stream_read_line(reader, NULL, NULL, NULL);

        if (line == NULL)
            break;

        /*
         * The same decision the stdio relay makes, gate included.  This
         * loop used to apply the permitted list and nothing else, so an
         * action rule on a connector reached over HTTP was never asked.
         */
        clawt_mcp_relay_policy_outbound(policy, line, &to_server,
                                        &to_client);

        if (to_client != NULL)
            write_line(out, to_client);

        if (to_server != NULL)
            forward_one(&bridge, to_server);
    }

    g_free(bridge.session_id);

    return EXIT_SUCCESS;
}

static gint
run_http(ClawtConnectorPlan *plan, const ClawtMcpRelayPolicy *policy)
{
    g_autoptr(GInputStream) stdin_stream = NULL;
    g_autoptr(GOutputStream) stdout_stream = NULL;
    g_autoptr(SoupSession) session = NULL;
    gboolean gated = FALSE;

    /*
     * Synchronous throughout, which is right here and would not be in
     * the daemon: this process exists to serve one MCP client, so there
     * is nothing else for it to be doing while a request is out, and a
     * main loop would be machinery in the way of reading the code.
     */
    session = soup_session_new_with_options("user-agent", USER_AGENT,
                                            "timeout", 120, NULL);

    /*
     * A gated box has no route but the egress gate, and the relay is in
     * the box.  Every request goes through it, set here rather than left
     * to HTTPS_PROXY: whether libsoup reads that depends on which GIO
     * proxy module the machine has, and NO_PROXY names 127.0.0.1 -- which
     * in this namespace is the box's own loopback, not the host's the
     * connector's URL means.  The gate then decides, as it does for
     * everything else the agent reaches.
     */
    {
        const gchar *gate = g_getenv(CLAWT_EGRESS_PROXY_ENV);

        if (gate != NULL && *gate != '\0') {
            g_autoptr(GProxyResolver) resolver =
                g_simple_proxy_resolver_new(gate, NULL);

            soup_session_set_proxy_resolver(session, resolver);
            gated = TRUE;
        }
    }

    stdin_stream = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    stdout_stream = g_unix_output_stream_new(STDOUT_FILENO, FALSE);

    return run_http_on(plan, policy, session, gated, stdin_stream,
                       stdout_stream);
}

gint
clawt_connector_relay_run(ClawtConnectorPlan *plan)
{
    return clawt_connector_relay_run_full(plan, NULL, NULL, NULL);
}

gint
clawt_connector_relay_run_gated(ClawtConnectorPlan *plan,
                                ClawtMcpRelayGate   gate,
                                gpointer            gate_data)
{
    return clawt_connector_relay_run_full(plan, gate, gate_data, NULL);
}

/*
 * The one policy both transports apply, from the plan.  Its own function
 * so the stream-driven entry point below cannot build a different one.
 */
static void
policy_from_plan(ClawtConnectorPlan  *plan,
                 ClawtMcpRelayGate    gate,
                 gpointer             gate_data,
                 ClawtMcpDirectory   *directory,
                 ClawtMcpRelayPolicy *policy)
{
    memset(policy, 0, sizeof(*policy));

    /*
     * No tool list means no restriction, and it has to be said as a
     * separate flag: the filtering relay reads a NULL list as "allow
     * nothing", which is the right default for a grant being enforced
     * and exactly wrong for a connector that simply was not narrowed.
     */
    policy->permitted = plan->permitted;
    policy->filter = (plan->permitted != NULL);
    policy->hint = (plan->gateway != NULL) ? BROKER_REFUSAL_HINT
                                           : CONNECTOR_REFUSAL_HINT;
    policy->gate = gate;
    policy->gate_data = gate_data;
    policy->directory = directory;

    /*
     * Several accounts on one service: the prefix is part of the one
     * policy both transports apply, outermost on both sides, so the
     * directory, the tool list and the gate all see the server's names.
     */
    policy->accounts = plan->accounts;
    policy->account_prefix = plan->account_prefix;
    policy->account_siblings = plan->account_siblings;
    policy->account_conflict = plan->account_conflict;

    /* The budget and apps, which the caller set up for this agent. */
    policy->results = plan->results;
    policy->apps = plan->apps;
}

gint
clawt_connector_relay_run_full(ClawtConnectorPlan *plan,
                               ClawtMcpRelayGate   gate,
                               gpointer            gate_data,
                               ClawtMcpDirectory  *directory)
{
    ClawtMcpRelayPolicy policy;

    g_return_val_if_fail(plan != NULL, EXIT_FAILURE);

    policy_from_plan(plan, gate, gate_data, directory, &policy);

    if (plan->url != NULL)
        return run_http(plan, &policy);

    return clawt_mcp_relay_run_policy(plan->argv, plan->envp, &policy);
}

gint
clawt_connector_relay_run_http_on(ClawtConnectorPlan *plan,
                                  ClawtMcpRelayGate   gate,
                                  gpointer            gate_data,
                                  ClawtMcpDirectory  *directory,
                                  SoupSession        *session,
                                  GInputStream       *in,
                                  GOutputStream      *out)
{
    ClawtMcpRelayPolicy policy;

    g_return_val_if_fail(plan != NULL && plan->url != NULL, EXIT_FAILURE);
    g_return_val_if_fail(SOUP_IS_SESSION(session), EXIT_FAILURE);
    g_return_val_if_fail(G_IS_INPUT_STREAM(in), EXIT_FAILURE);
    g_return_val_if_fail(G_IS_OUTPUT_STREAM(out), EXIT_FAILURE);

    policy_from_plan(plan, gate, gate_data, directory, &policy);

    return run_http_on(plan, &policy, session, FALSE, in, out);
}

ClawtMcpDirectory *
clawt_connector_directory_new(ClawtConfig             *config,
                              ClawtIntegrationBinding *binding,
                              const gchar             *agent_id)
{
    ClawtToolDirectoryMode mode = CLAWT_TOOL_DIRECTORY_AUTO;
    ClawtAgentConfig *agent = NULL;
    const gchar *nick;
    const gchar *provider = NULL;
    gint64 min_tools;
    gint64 min_chars;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
    g_return_val_if_fail(binding != NULL, NULL);

    nick = clawt_integration_binding_get_string(binding, "directory");

    /*
     * A spelling no mode has is said out loud and read as `auto`, the
     * documented default -- not as whichever member is zero, and not as
     * a reason to take the connector's tools away.
     */
    if (!clawt_tool_directory_mode_from_nick(nick, &mode))
        g_printerr("clawtilla: connector '%s': directory '%s' is not auto, "
                   "always or never; using auto\n",
                   clawt_integration_binding_get_name(binding), nick);

    if (agent_id != NULL)
        agent = clawt_config_get_agent(config, agent_id);

    if (agent != NULL)
        provider = clawt_agent_config_get_string(agent, "model.provider");

    min_tools = clawt_config_get_int(config, "connectors.directory_min_tools");
    min_chars = clawt_config_get_int(config, "connectors.directory_min_chars");

    return clawt_mcp_directory_new(
        clawt_integration_binding_get_name(binding), mode,
        clawt_mcp_directory_cli_searches_tools(provider),
        (guint64)MAX(min_tools, 0), (guint64)MAX(min_chars, 0));
}
