/*
 * main.c - clawtilla-mcp-server: the fleet's tools, as an MCP server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent runs an AI CLI, and the only way such a CLI can be given
 * tools is an --mcp-config naming a server to talk to.  clawtilla served
 * its orchestration tools over the agent's own link instead, as
 * mcp.request frames, which assumed something on the agent side would
 * relay them into the session.  Nothing did -- so an agent had a
 * mailbox, peers and a container it could not reach, and would tell you
 * so if asked.
 *
 * This is that server.  It speaks MCP over stdio to the CLI on one side
 * and clawtilla's IPC socket on the other, and is almost entirely a
 * pass-through: clawt_mcp_tools_call() already takes a JSON-RPC request
 * and returns a JSON-RPC response, so the tools have exactly one
 * implementation and this cannot drift from it.
 */

#include <clawtilla.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

#include <stdlib.h>
#include <unistd.h>

/*
 * The MCP protocol version this speaks.
 *
 * Answered from here rather than echoed back from the client: echoing
 * agrees to whatever was asked for, including a version whose semantics
 * we do not implement.
 */
#define MCP_PROTOCOL_VERSION "2024-11-05"

static gchar    *opt_socket = NULL;
static gchar    *opt_agent = NULL;
static gchar    *opt_token_file = NULL;
static gboolean  opt_version = FALSE;
static gboolean  opt_license = FALSE;

static const GOptionEntry entries[] = {
    { "socket", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket,
      "clawtilla daemon socket", "PATH" },
    { "agent", 'a', 0, G_OPTION_ARG_STRING, &opt_agent,
      "which agent these tools act as", "ID" },
    { "token-file", 't', 0, G_OPTION_ARG_FILENAME, &opt_token_file,
      "file holding that agent's link token", "PATH" },
    { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version,
      "print the version and exit", NULL },
    { "license", 0, 0, G_OPTION_ARG_NONE, &opt_license,
      "print licensing information and exit", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
};

static const gchar usage_text[] =
"clawtilla-mcp-server -- clawtilla's orchestration tools, over MCP\n"
"\n"
"Speaks MCP on stdin and stdout, and answers by asking the clawtilla\n"
"daemon.  Not meant to be run by hand: an agent's AI CLI starts it,\n"
"from the .mcp.json clawtilla writes into the agent's workspace.\n"
"\n"
"Examples:\n"
"  # What an agent's .mcp.json amounts to\n"
"  clawtilla-mcp-server --agent researcher \\\n"
"      --socket ~/.clawtilla/daemon.sock \\\n"
"      --token-file ~/.clawtilla/agents/researcher/token\n"
"\n"
"  # Check the tools an agent is offered, by hand\n"
"  echo '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}' \\\n"
"      | clawtilla-mcp-server --agent researcher\n";

/* ── Writing a reply ─────────────────────────────────────────────── */

/*
 * One JSON object per line, flushed immediately.
 *
 * MCP stdio framing is newline-delimited JSON.  The flush matters: the
 * CLI on the other end is waiting on this response before it does
 * anything else, so a buffered reply is a hang rather than a delay.
 */
static void
write_line(GOutputStream *out, JsonNode *node)
{
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autofree gchar *text = NULL;
    g_autoptr(GError) error = NULL;
    gsize written = 0;

    json_generator_set_root(generator, node);
    text = json_generator_to_data(generator, NULL);

    if (text == NULL)
        return;

    if (!g_output_stream_write_all(out, text, strlen(text), &written, NULL,
                                   &error) ||
        !g_output_stream_write_all(out, "\n", 1, &written, NULL, &error)) {
        g_printerr("clawtilla-mcp-server: could not write a reply: %s\n",
                   error != NULL ? error->message : "unknown");
        return;
    }

    g_output_stream_flush(out, NULL, NULL);
}

static JsonNode *
make_error(JsonNode *id, gint code, const gchar *message)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");

    json_builder_set_member_name(builder, "id");

    if (id != NULL)
        json_builder_add_value(builder, json_node_ref(id));
    else
        json_builder_add_null_value(builder);

    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, message);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * The handshake, answered here rather than forwarded.
 *
 * initialize is transport boilerplate: it says who both ends are and
 * what they can do.  The daemon has no opinion about it, and making it
 * grow one would be a second place for the protocol version to be
 * wrong.
 */
static JsonNode *
make_initialize_result(JsonNode *id)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");

    if (id != NULL)
        json_builder_add_value(builder, json_node_ref(id));
    else
        json_builder_add_null_value(builder);

    json_builder_set_member_name(builder, "result");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "protocolVersion");
    json_builder_add_string_value(builder, MCP_PROTOCOL_VERSION);

    json_builder_set_member_name(builder, "capabilities");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "tools");
    json_builder_begin_object(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "serverInfo");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "clawtilla");
    json_builder_set_member_name(builder, "version");
    json_builder_add_string_value(builder, CLAWT_VERSION_STRING);
    json_builder_end_object(builder);

    json_builder_end_object(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Asking the daemon ───────────────────────────────────────────── */

typedef struct {
    ClawtClient *client;
    gchar       *agent;
    gchar       *token;
} Bridge;

/*
 * Forwards one request and returns the daemon's answer.
 *
 * The request goes across whole and the response comes back whole:
 * clawt_mcp_tools_call() speaks JSON-RPC at both ends, so the tools have
 * one implementation and this cannot drift from it.
 */
static JsonNode *
forward(Bridge *bridge, JsonNode *request, JsonNode *id)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    JsonObject *result;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, bridge->agent);

    if (bridge->token != NULL) {
        json_builder_set_member_name(builder, "token");
        json_builder_add_string_value(builder, bridge->token);
    }

    json_builder_set_member_name(builder, "request");
    json_builder_add_value(builder, json_node_ref(request));
    json_builder_end_object(builder);

    /*
     * The payload is handed over, not borrowed: clawt_client_request()
     * takes it (transfer full).  Keeping a g_autoptr on it as well
     * unreferenced a node the request had already freed.
     */
    reply = clawt_client_request(bridge->client, "tool.rpc",
                                 json_builder_get_root(builder), &error);

    if (reply == NULL) {
        /*
         * Reported as a JSON-RPC error rather than by dying.  The CLI
         * has a whole conversation in flight; a daemon that was
         * restarted should cost one tool call, not the session.
         */
        return make_error(id, -32000,
                          error != NULL ? error->message
                                        : "the daemon did not answer");
    }

    if (!JSON_NODE_HOLDS_OBJECT(reply))
        return make_error(id, -32000, "the daemon sent a malformed reply");

    result = json_node_get_object(reply);

    if (!json_object_has_member(result, "response"))
        return make_error(id, -32000, "the daemon sent no response");

    return json_node_ref(json_object_get_member(result, "response"));
}

/* ── The loop ────────────────────────────────────────────────────── */

static void
handle_line(Bridge *bridge, GOutputStream *out, const gchar *line)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) response = NULL;
    JsonNode *root;
    JsonObject *request;
    JsonNode *id = NULL;
    const gchar *method;

    if (!json_parser_load_from_data(parser, line, -1, &error)) {
        write_line(out, make_error(NULL, -32700, "Parse error"));
        return;
    }

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        write_line(out, make_error(NULL, -32600, "Invalid request"));
        return;
    }

    request = json_node_get_object(root);

    if (json_object_has_member(request, "id"))
        id = json_object_get_member(request, "id");

    method = json_object_has_member(request, "method")
             ? json_object_get_string_member(request, "method") : NULL;

    if (method == NULL) {
        write_line(out, make_error(id, -32600, "Invalid request"));
        return;
    }

    /*
     * A notification has no id and takes no reply.  Answering one is a
     * protocol error, and some clients treat the stray response as the
     * answer to whatever they ask next.
     */
    if (id == NULL) {
        return;
    }

    if (g_strcmp0(method, "initialize") == 0) {
        write_line(out, make_initialize_result(id));
        return;
    }

    if (g_strcmp0(method, "ping") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "jsonrpc");
        json_builder_add_string_value(builder, "2.0");
        json_builder_set_member_name(builder, "id");
        json_builder_add_value(builder, json_node_ref(id));
        json_builder_set_member_name(builder, "result");
        json_builder_begin_object(builder);
        json_builder_end_object(builder);
        json_builder_end_object(builder);

        write_line(out, json_builder_get_root(builder));
        return;
    }

    /*
     * Everything the daemon does not implement is answered here rather
     * than forwarded: resources/list and prompts/list are asked for by
     * clients as a matter of course, and an error for them reads as a
     * broken server rather than one with no resources.
     */
    if (g_str_has_prefix(method, "resources/") ||
        g_str_has_prefix(method, "prompts/")) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "jsonrpc");
        json_builder_add_string_value(builder, "2.0");
        json_builder_set_member_name(builder, "id");
        json_builder_add_value(builder, json_node_ref(id));
        json_builder_set_member_name(builder, "result");
        json_builder_begin_object(builder);
        json_builder_set_member_name(
            builder, g_str_has_prefix(method, "resources/") ? "resources"
                                                            : "prompts");
        json_builder_begin_array(builder);
        json_builder_end_array(builder);
        json_builder_end_object(builder);
        json_builder_end_object(builder);

        write_line(out, json_builder_get_root(builder));
        return;
    }

    response = forward(bridge, root, id);

    if (response != NULL)
        write_line(out, response);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GDataInputStream) in = NULL;
    g_autoptr(GOutputStream) out = NULL;
    g_autoptr(GInputStream) raw_in = NULL;
    Bridge bridge = { 0 };
    gint status = EXIT_SUCCESS;

    context = g_option_context_new("- clawtilla's tools over MCP");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, usage_text);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("clawtilla-mcp-server: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (opt_version) {
        g_print("clawtilla-mcp-server %s\n", CLAWT_VERSION_STRING);
        return EXIT_SUCCESS;
    }

    if (opt_license) {
        g_print("clawtilla is free software under the GNU Affero General "
                "Public License,\nversion 3 or later. See the LICENSE "
                "file.\n");
        return EXIT_SUCCESS;
    }

    if (opt_agent == NULL) {
        g_printerr("clawtilla-mcp-server: --agent is required\n");
        g_printerr("%s", usage_text);
        return EXIT_FAILURE;
    }

    /*
     * The token is read once, at start.  It does not change while the
     * daemon is up -- it is written when the agent's files are first
     * rendered and left alone -- and re-reading it per call would put a
     * file read in the path of every tool the agent uses.
     */
    if (opt_token_file != NULL) {
        g_autofree gchar *expanded = clawt_expand_path(opt_token_file);

        if (!g_file_get_contents(expanded, &bridge.token, NULL, &error)) {
            g_printerr("clawtilla-mcp-server: could not read %s: %s\n",
                       expanded, error->message);
            return EXIT_FAILURE;
        }

        g_strstrip(bridge.token);
    }

    client = clawt_client_new(opt_socket);

    if (!clawt_client_connect(client, &error)) {
        g_printerr("clawtilla-mcp-server: could not reach the daemon%s%s: "
                   "%s\n",
                   opt_socket != NULL ? " at " : "",
                   opt_socket != NULL ? opt_socket : "",
                   error->message);
        return EXIT_FAILURE;
    }

    bridge.client = client;
    bridge.agent = opt_agent;

    raw_in = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    in = g_data_input_stream_new(raw_in);
    g_data_input_stream_set_newline_type(in, G_DATA_STREAM_NEWLINE_TYPE_ANY);
    out = g_unix_output_stream_new(STDOUT_FILENO, FALSE);

    for (;;) {
        g_autofree gchar *line = NULL;
        gsize length = 0;

        line = g_data_input_stream_read_line(in, &length, NULL, &error);

        if (line == NULL) {
            /* Clean end of stdin: the CLI finished. */
            if (error != NULL) {
                g_printerr("clawtilla-mcp-server: read failed: %s\n",
                           error->message);
                status = EXIT_FAILURE;
            }

            break;
        }

        if (length == 0)
            continue;

        handle_line(&bridge, out, line);
    }

    g_free(bridge.token);

    return status;
}
