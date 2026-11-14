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
    g_free(self->url);
    g_free(self->header_name);
    g_free(self->header_value);

    g_free(self);
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
    g_return_val_if_fail(credential != NULL, NULL);

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

    formatted = clawt_connector_format_credential(info, credential);

    if (info->placement == CLAWT_CREDENTIAL_PLACEMENT_HEADER) {
        plan->header_name = g_strdup(name);
        plan->header_value = g_steal_pointer(&formatted);
    } else {
        GPtrArray *envp = g_ptr_array_new();

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
     * How deep forward_one() has recursed for pages the directory asked
     * for itself.  The directory stops after its own page limit; this
     * is the backstop that keeps a misbehaving server from turning that
     * into a stack overflow.
     */
    guint                      depth;
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

    if (to_client != NULL)
        write_line(bridge->out, to_client);

    if (to_server != NULL && bridge->depth < 128) {
        bridge->depth++;
        forward_one(bridge, to_server);
        bridge->depth--;
    }
}

/*
 * A streamable-HTTP server may answer either with one JSON object or
 * with an SSE stream carrying several.  Both are handled, because which
 * one arrives is the server's choice per request and not a property of
 * the server -- a client that understands only JSON works until the
 * first tool call that streams.
 */
static void
deliver_response(HttpBridge *bridge, const gchar *content_type,
                 const gchar *body)
{
    if (body == NULL || *body == '\0')
        return;

    if (content_type != NULL && strstr(content_type, "text/event-stream")) {
        g_auto(GStrv) lines = g_strsplit(body, "\n", -1);
        gsize i;

        for (i = 0; lines[i] != NULL; i++) {
            const gchar *data;

            if (!g_str_has_prefix(lines[i], "data:"))
                continue;

            data = lines[i] + 5;

            while (*data == ' ')
                data++;

            if (*data == '\0')
                continue;

            deliver_one(bridge, data);
        }

        return;
    }

    deliver_one(bridge, body);
}

static void
forward_one(HttpBridge *bridge, const gchar *line)
{
    g_autoptr(SoupMessage) message = NULL;
    g_autoptr(GBytes) response = NULL;
    g_autoptr(GBytes) request = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;
    SoupMessageHeaders *headers;
    const gchar *content_type;
    const gchar *fresh_session;
    gsize length = 0;

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

    response = clawt_http_send_and_read(bridge->session, message, NULL,
                                        &error);

    if (response == NULL) {
        g_printerr("clawtilla: the request to the tool server failed: %s\n",
                   error->message);
        return;
    }

    fresh_session = soup_message_headers_get_one(
        soup_message_get_response_headers(message), "Mcp-Session-Id");

    if (fresh_session != NULL) {
        g_free(bridge->session_id);
        bridge->session_id = g_strdup(fresh_session);
    }

    /*
     * A notification has no id and gets 202 with nothing in it.  Writing
     * an empty line for one would put a message on the wire the client
     * never asked for.
     */
    if (g_bytes_get_size(response) == 0)
        return;

    content_type = soup_message_headers_get_content_type(
        soup_message_get_response_headers(message), NULL);

    text = g_strndup(g_bytes_get_data(response, &length), length);

    deliver_response(bridge, content_type, text);
}

static gint
run_http(ClawtConnectorPlan *plan, const ClawtMcpRelayPolicy *policy)
{
    HttpBridge bridge = { 0 };
    g_autoptr(GInputStream) stdin_stream = NULL;
    g_autoptr(GOutputStream) stdout_stream = NULL;
    g_autoptr(GDataInputStream) reader = NULL;
    g_autoptr(SoupSession) session = NULL;

    /*
     * Synchronous throughout, which is right here and would not be in
     * the daemon: this process exists to serve one MCP client, so there
     * is nothing else for it to be doing while a request is out, and a
     * main loop would be machinery in the way of reading the code.
     */
    session = soup_session_new_with_options("user-agent", USER_AGENT,
                                            "timeout", 120, NULL);

    stdin_stream = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    stdout_stream = g_unix_output_stream_new(STDOUT_FILENO, FALSE);
    reader = g_data_input_stream_new(stdin_stream);

    g_buffered_input_stream_set_buffer_size(
        G_BUFFERED_INPUT_STREAM(reader), RELAY_BUFFER_BYTES);

    bridge.session = session;
    bridge.plan = plan;
    bridge.policy = policy;
    bridge.out = stdout_stream;

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
            write_line(stdout_stream, to_client);

        if (to_server != NULL)
            forward_one(&bridge, to_server);
    }

    g_free(bridge.session_id);

    return EXIT_SUCCESS;
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

gint
clawt_connector_relay_run_full(ClawtConnectorPlan *plan,
                               ClawtMcpRelayGate   gate,
                               gpointer            gate_data,
                               ClawtMcpDirectory  *directory)
{
    ClawtMcpRelayPolicy policy = { 0 };

    g_return_val_if_fail(plan != NULL, EXIT_FAILURE);

    /*
     * No tool list means no restriction, and it has to be said as a
     * separate flag: the filtering relay reads a NULL list as "allow
     * nothing", which is the right default for a grant being enforced
     * and exactly wrong for a connector that simply was not narrowed.
     */
    policy.permitted = plan->permitted;
    policy.filter = (plan->permitted != NULL);
    policy.hint = CONNECTOR_REFUSAL_HINT;
    policy.gate = gate;
    policy.gate_data = gate_data;
    policy.directory = directory;

    if (plan->url != NULL)
        return run_http(plan, &policy);

    return clawt_mcp_relay_run_policy(plan->argv, plan->envp, &policy);
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
