/*
 * test-ipc-framing.c - What the daemon buffers before it has been told who
 *                      is talking to it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The frame size limit was applied to a line that had already been read,
 * and g_data_input_stream_read_line_async() grows its buffer until it
 * finds a newline -- doubling, without bound, whatever
 * g_buffered_input_stream_set_buffer_size() was set to.  So a peer that
 * wrote bytes and no newline made the daemon allocate them all, and the
 * refusal arrived only once it had.
 *
 * `daemon.tailscale` defaults to true, so the daemon binds the machine's
 * tailnet address by default and a peer there is unauthenticated until
 * it says control.hello.  No token was needed to do this: the check runs
 * on a parsed frame, which is behind the allocation.
 *
 * Exercised over a unix socket so `make test` opens no network socket.
 * The reader is the same one either listener uses -- which is the point:
 * the rule belongs to the read, not to the listener somebody noticed.
 */

#include <clawtilla.h>

#include "clawt-test-util.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>

/*
 * A bound on the iterations rather than on the clock.  A test that can
 * hang is worse than one that fails, and the loop below is waiting for
 * something that either happens quickly or does not happen at all.
 */
#define MAX_TURNS (20000)

static JsonNode *
answer_nothing(JsonNode *request, gpointer user_data)
{
    (void)user_data;

    return clawt_ipc_response_new(request, NULL);
}

/*
 * Iterate until @predicate holds or the budget runs out.
 *
 * Returns: %TRUE if it held
 */
static gboolean
settle(ClawtIpcServer *server, gboolean want_clients)
{
    guint turn;

    for (turn = 0; turn < MAX_TURNS; turn++) {
        gboolean has = clawt_ipc_server_count_clients(server) > 0;

        if (has == want_clients)
            return TRUE;

        g_main_context_iteration(NULL, FALSE);
        g_usleep(200);
    }

    return FALSE;
}

/*
 * A frame with no newline in it is refused at the size it is refused
 * at, not after it has all been buffered.
 *
 * Asserted by the connection being dropped while the peer is still
 * holding it open: the old reader was waiting for a newline that was
 * never coming, growing as it waited, and would have sat there for as
 * long as the peer kept writing.
 */
static void
test_an_endless_frame_is_dropped_before_it_is_buffered(void)
{
    g_autoptr(ClawtIpcServer) server = NULL;
    g_autoptr(GSocketClient) client = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *chunk = NULL;
    GSocket *socket;
    gsize sent = 0;
    guint turn;

    dir = g_dir_make_tmp("clawt-framing-XXXXXX", &error);
    g_assert_no_error(error);

    socket_path = g_build_filename(dir, "d.sock", NULL);

    server = clawt_ipc_server_new(socket_path);
    clawt_ipc_server_set_handler(server, answer_nothing, NULL, NULL);

    g_assert_true(clawt_ipc_server_start(server, &error));
    g_assert_no_error(error);

    client = g_socket_client_new();
    address = g_unix_socket_address_new(socket_path);
    connection = g_socket_client_connect(client,
                                         G_SOCKET_CONNECTABLE(address),
                                         NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);

    g_assert_true(settle(server, TRUE));

    /*
     * Bytes and no newline, past the largest frame the protocol admits.
     *
     * Written non-blocking, with the loop turned between attempts: a
     * blocking write deadlocks this test outright, because the server's
     * reader runs on the very context the write is stopping us from
     * iterating.  A test that can hang is worse than one that fails.
     */
    socket = g_socket_connection_get_socket(connection);
    g_socket_set_blocking(socket, FALSE);

    chunk = g_malloc(64 * 1024);
    memset(chunk, 'a', 64 * 1024);

    for (turn = 0; turn < MAX_TURNS; turn++) {
        gssize wrote;

        if (clawt_ipc_server_count_clients(server) == 0)
            break;

        if (sent >= (gsize)CLAWT_IPC_MAX_FRAME_BYTES + (1024 * 1024))
            break;

        wrote = g_socket_send(socket, chunk, 64 * 1024, NULL, &error);

        if (wrote > 0)
            sent += (gsize)wrote;
        else if (error != NULL &&
                 !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            /* The server hung up on us, which is the pass condition. */
            break;

        g_clear_error(&error);
        g_main_context_iteration(NULL, FALSE);
    }

    g_clear_error(&error);

    /* And it is gone, without our ever having sent a newline. */
    g_assert_true(settle(server, FALSE));

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    clawt_ipc_server_stop(server);

    while (g_main_context_iteration(NULL, FALSE))
        ;

    clawt_test_remove_tree(dir);
}

/*
 * And an ordinary frame still works, at a size that spans several reads.
 *
 * The cap has to be a cap on a *frame*, not on a read: a legal frame
 * arrives in as many chunks as it takes and none of them is the whole
 * line.  A fix that refused whenever one read did not contain a newline
 * would pass the test above and break the product.
 *
 * Sized under the socket's own buffer rather than at the megabytes an
 * attachment reaches, because ClawtClient writes synchronously and this
 * server is in the same process on the same context: a write large
 * enough to fill the socket deadlocks the test, not the product, where
 * the two are separate processes.  128 KB is two of
 * CLAWT_IPC_READ_CHUNK_BYTES, which is what this is here to exercise.
 */
static void
test_a_long_but_legal_frame_still_arrives(void)
{
    g_autoptr(ClawtIpcServer) server = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *big = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    dir = g_dir_make_tmp("clawt-framing2-XXXXXX", &error);
    g_assert_no_error(error);

    socket_path = g_build_filename(dir, "d.sock", NULL);

    server = clawt_ipc_server_new(socket_path);
    clawt_ipc_server_set_handler(server, answer_nothing, NULL, NULL);

    g_assert_true(clawt_ipc_server_start(server, &error));
    g_assert_no_error(error);

    client = clawt_client_new(socket_path);
    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    big = g_malloc(128 * 1024 + 1);
    memset(big, 'x', 128 * 1024);
    big[128 * 1024] = '\0';

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "blob");
    json_builder_add_string_value(builder, big);
    json_builder_end_object(builder);

    reply = clawt_client_request(client, "control.ping",
                                 json_builder_get_root(builder), &error);

    g_assert_no_error(error);
    g_assert_nonnull(reply);

    clawt_client_disconnect(client);
    clawt_ipc_server_stop(server);

    while (g_main_context_iteration(NULL, FALSE))
        ;

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ipc-framing/endless-frame-is-dropped",
                    test_an_endless_frame_is_dropped_before_it_is_buffered);
    g_test_add_func("/ipc-framing/long-legal-frame-arrives",
                    test_a_long_but_legal_frame_still_arrives);

    return g_test_run();
}
