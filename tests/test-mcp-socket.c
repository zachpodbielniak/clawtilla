/*
 * test-mcp-socket.c - one MCP tool call over a unix socket
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawt_mcp_socket_call() is how every picture of a gowl screen is
 * taken and every key sent to one.  These run it against a stand-in
 * server on a thread, answering the way real servers do.
 */

#include <clawtilla.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar    *path;
    gboolean  string_ids;   /* answer "id":"N", as gowl 0.5.0 does */
    gboolean  chatter;      /* a notification and a stray reply first */
} Server;

/* The id of a request line, as a number. */
static gint64
request_id(JsonObject *request)
{
    return json_object_has_member(request, "id")
        ? json_object_get_int_member(request, "id") : -1;
}

static void
answer(GOutputStream *out, Server *server, gint64 id, const gchar *result)
{
    g_autofree gchar *line = server->string_ids
        ? g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":\"%" G_GINT64_FORMAT
                          "\",\"result\":%s}\n", id, result)
        : g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT
                          ",\"result\":%s}\n", id, result);

    g_output_stream_write_all(out, line, strlen(line), NULL, NULL, NULL);
}

/* Serves one connection: initialize, then the one tool call. */
static gpointer
serve(gpointer data)
{
    Server *server = data;
    g_autoptr(GSocketListener) listener = g_socket_listener_new();
    g_autoptr(GSocketAddress) address =
        g_unix_socket_address_new(server->path);
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GDataInputStream) in = NULL;
    GOutputStream *out;
    gchar *line;

    g_assert_true(g_socket_listener_add_address(
        listener, address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
        NULL, NULL, NULL));
    connection = g_socket_listener_accept(listener, NULL, NULL, NULL);
    g_assert_nonnull(connection);
    in = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

    while ((line = g_data_input_stream_read_line(in, NULL, NULL, NULL)) !=
           NULL) {
        g_autoptr(JsonParser) parser = json_parser_new();
        JsonObject *request;
        const gchar *method;

        g_assert_true(json_parser_load_from_data(parser, line, -1, NULL));
        g_free(line);
        request = json_node_get_object(json_parser_get_root(parser));
        method = json_object_get_string_member(request, "method");

        if (g_strcmp0(method, "initialize") == 0) {
            answer(out, server, request_id(request),
                   "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{}}");
        } else if (g_strcmp0(method, "tools/call") == 0) {
            if (server->chatter) {
                const gchar *noise =
                    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/"
                    "message\",\"params\":{\"level\":\"info\"}}\n"
                    "{\"jsonrpc\":\"2.0\",\"id\":\"999\",\"result\":{}}\n"
                    "not json at all\n";

                g_output_stream_write_all(out, noise, strlen(noise), NULL,
                                          NULL, NULL);
            }
            answer(out, server, request_id(request),
                   "{\"content\":[{\"type\":\"text\",\"text\":\"pong\"}]}");
            break;
        }
    }
    return NULL;
}

static void
call_against(gboolean string_ids, gboolean chatter)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-mcpsock-XXXXXX", NULL);
    Server server = { 0 };
    GThread *thread;
    g_autoptr(JsonNode) result = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;
    guint waited;

    server.path = g_build_filename(dir, "mcp.sock", NULL);
    server.string_ids = string_ids;
    server.chatter = chatter;
    thread = g_thread_new("mcp-stand-in", serve, &server);
    for (waited = 0; waited < 2000 &&
         !g_file_test(server.path, G_FILE_TEST_EXISTS); waited += 10)
        g_usleep(10 * 1000);

    result = clawt_mcp_socket_call(server.path, "ping", NULL, 5, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    text = clawt_mcp_socket_result_text(result);
    g_assert_cmpstr(text, ==, "pong");

    g_thread_join(thread);
    g_free(server.path);
    clawt_test_remove_tree(dir);
}

/* The ordinary case: ids answered as the numbers they were sent as. */
static void
test_numeric_ids(void)
{
    call_against(FALSE, FALSE);
}

/*
 * gowl 0.5.0 answers `"id":"1"` to `"id":1`.  JSON-RPC allows it; read
 * as an integer only, it never matched, and every screenshot of a gowl
 * display waited out its whole timeout against a server that had
 * already answered.
 */
static void
test_string_ids(void)
{
    call_against(TRUE, FALSE);
}

/*
 * A notification, somebody else's reply and a line that is not JSON
 * before the answer are all passed over: the first line back is not
 * necessarily the reply.
 */
static void
test_chatter_before_the_answer(void)
{
    call_against(TRUE, TRUE);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mcp-socket/numeric-ids", test_numeric_ids);
    g_test_add_func("/mcp-socket/string-ids", test_string_ids);
    g_test_add_func("/mcp-socket/chatter-before-the-answer",
                    test_chatter_before_the_answer);

    return g_test_run();
}
