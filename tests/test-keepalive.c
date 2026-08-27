/*
 * test-keepalive.c - Noticing that a network client has gone
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A unix client that goes away closes its socket and the reader is told
 * at once.  A client on another machine does no such thing: its route
 * can disappear -- a laptop suspended, a tailnet that came back on a
 * different path -- and both ends go on holding a connection that no
 * packet will ever cross again.  Nothing fails, so nothing reconnects,
 * so the window stays "connected" and shows no new message for the rest
 * of the day.
 *
 * The cadence is what makes that visible while somebody is still
 * sitting there, so the cadence is what is asserted on.  Arming a
 * socket needs no address and no peer, which is why the first two tests
 * are hermetic; proving that the two places that open a connection
 * actually call it needs a real loopback socket, so that one sits
 * behind CLAWT_TEST_INTEGRATION.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

static gboolean
integration_enabled(void)
{
    return g_getenv("CLAWT_TEST_INTEGRATION") != NULL;
}

/*
 * One socket option, read back off the descriptor rather than off
 * anything we remember setting.
 */
static gint
socket_option(gint fd, gint level, gint option)
{
    gint value = -1;
    socklen_t length = sizeof(value);

    if (getsockopt(fd, level, option, &value, &length) != 0)
        return -1;

    return value;
}

/* ── The policy ──────────────────────────────────────────────────── */

static void
test_a_tcp_socket_is_armed(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = NULL;
    gint fd;

    /*
     * Unbound and unconnected on purpose.  Nothing here needs an
     * interface, which is what keeps `make test` hermetic -- the suite
     * runs under `unshare -rn`, where loopback is down and there is no
     * address to bind to.
     */
    socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                          G_SOCKET_PROTOCOL_TCP, &error);

    g_assert_no_error(error);
    g_assert_nonnull(socket);

    fd = g_socket_get_fd(socket);

    /* The kernel's default, and the whole problem. */
    g_assert_cmpint(socket_option(fd, SOL_SOCKET, SO_KEEPALIVE), ==, 0);

    g_assert_true(clawt_ipc_socket_keepalive(socket, &error));
    g_assert_no_error(error);

    g_assert_cmpint(socket_option(fd, SOL_SOCKET, SO_KEEPALIVE), !=, 0);
    g_assert_cmpint(socket_option(fd, IPPROTO_TCP, TCP_KEEPIDLE), ==,
                    CLAWT_IPC_KEEPALIVE_IDLE_SECONDS);
    g_assert_cmpint(socket_option(fd, IPPROTO_TCP, TCP_KEEPINTVL), ==,
                    CLAWT_IPC_KEEPALIVE_INTERVAL_SECONDS);
    g_assert_cmpint(socket_option(fd, IPPROTO_TCP, TCP_KEEPCNT), ==,
                    CLAWT_IPC_KEEPALIVE_COUNT);
}

/*
 * SO_KEEPALIVE on its own is not the fix.
 *
 * The kernel's default idle is two hours, so a connection armed and left
 * at the default is reported broken long after the person watching has
 * concluded the fleet is dead -- and, worse, long after the message they
 * typed into it timed out and was lost.  A dead route has to be noticed
 * *before* a request gives up, or the reconnect arrives too late to be
 * the thing that saves the message.
 */
static void
test_a_dead_route_is_noticed_before_a_request_gives_up(void)
{
    gint budget = CLAWT_IPC_KEEPALIVE_IDLE_SECONDS +
                  (CLAWT_IPC_KEEPALIVE_INTERVAL_SECONDS *
                   CLAWT_IPC_KEEPALIVE_COUNT);

    /* clawt_client_request()'s own timeout, which it does not export. */
    g_assert_cmpint(budget, <, 120);

    /*
     * And not so eager that an ordinary pause between messages looks
     * like a failure.  Probing costs one packet; giving up costs the
     * conversation.
     */
    g_assert_cmpint(CLAWT_IPC_KEEPALIVE_IDLE_SECONDS, >=, 15);
}

static void
test_a_unix_socket_is_left_alone(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = NULL;

    socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                          G_SOCKET_PROTOCOL_DEFAULT, &error);

    g_assert_no_error(error);
    g_assert_nonnull(socket);

    /*
     * A no-op rather than a refusal.  Both callers hand over whatever
     * socket the connection arrived on, and a unix one is the ordinary
     * case -- reporting a failure for it would put a warning on the
     * console of every local client for something that is right.
     */
    g_assert_true(clawt_ipc_socket_keepalive(socket, &error));
    g_assert_no_error(error);

    g_assert_cmpint(socket_option(g_socket_get_fd(socket), SOL_SOCKET,
                                  SO_KEEPALIVE), ==, 0);
}

/* ── The wiring ──────────────────────────────────────────────────── */

/*
 * Every socket this process holds that is connected to @port, and
 * whether keepalive is armed on it.
 *
 * Read out of /proc/self/fd rather than out of anything the client or
 * the server hands back, because neither of them exposes its socket --
 * and a test that asked them to would be asserting on an accessor that
 * exists for the test.  A listener is skipped: SO_ACCEPTCONN is what
 * tells one apart, and arming a listening socket would do nothing.
 */
static void
count_connected_sockets(guint16 port, guint *out_total, guint *out_armed)
{
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    *out_total = 0;
    *out_armed = 0;

    dir = g_dir_open("/proc/self/fd", 0, NULL);

    if (dir == NULL)
        return;

    while ((name = g_dir_read_name(dir)) != NULL) {
        struct sockaddr_in local;
        struct sockaddr_in peer;
        socklen_t length;
        gint fd = (gint)g_ascii_strtoll(name, NULL, 10);
        gint type = 0;

        length = sizeof(type);

        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0)
            continue;

        if (socket_option(fd, SOL_SOCKET, SO_ACCEPTCONN) > 0)
            continue;

        length = sizeof(local);

        if (getsockname(fd, (struct sockaddr *)&local, &length) != 0 ||
            local.sin_family != AF_INET)
            continue;

        length = sizeof(peer);

        if (getpeername(fd, (struct sockaddr *)&peer, &length) != 0)
            continue;

        if (g_ntohs(local.sin_port) != port &&
            g_ntohs(peer.sin_port) != port)
            continue;

        (*out_total)++;

        if (socket_option(fd, SOL_SOCKET, SO_KEEPALIVE) > 0 &&
            socket_option(fd, IPPROTO_TCP, TCP_KEEPIDLE) ==
                CLAWT_IPC_KEEPALIVE_IDLE_SECONDS)
            (*out_armed)++;
    }
}

static JsonNode *
answer_nothing(JsonNode *request, gpointer user_data)
{
    return clawt_ipc_response_new(request, NULL);
}

/*
 * The test that would have caught this.
 *
 * The helper above can be perfect and reach nobody: a socket is armed
 * where a connection is opened, and there are two such places -- the
 * client dialling out and the server accepting.  Both ends of one real
 * TCP connection are in this process, so both are checkable at once,
 * which is the only version of this that notices if either call site is
 * dropped.
 */
static void
test_both_ends_of_a_tcp_connection_are_armed(void)
{
    g_autoptr(ClawtIpcServer) server = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *dir = NULL;
    guint total = 0;
    guint armed = 0;
    GLogLevelFlags fatal;
    const guint16 port = 18793;

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }

    dir = g_dir_make_tmp("clawt-keepalive-XXXXXX", &error);
    g_assert_no_error(error);

    socket_path = g_build_filename(dir, "d.sock", NULL);

    server = clawt_ipc_server_new(socket_path);
    clawt_ipc_server_set_handler(server, answer_nothing, NULL, NULL);
    clawt_ipc_server_set_token(server, "a-token");
    clawt_ipc_server_add_listener(server, "127.0.0.1", port, FALSE);

    /*
     * A TCP listener with no certificate says so, and GTest makes a
     * warning fatal.  The warning is correct and is not what this test
     * is about, so the fatal mask is relaxed for as long as the server
     * is starting -- and only the mask, so the warning is still printed
     * rather than hidden.
     */
    fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);

    if (!clawt_ipc_server_start(server, &error)) {
        g_log_set_always_fatal(fatal);
        g_test_skip(error->message);
        clawt_test_remove_tree(dir);
        return;
    }

    g_log_set_always_fatal(fatal);

    client = clawt_client_new_tcp("127.0.0.1", port, "a-token");

    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    /* The accept happens on the loop, not in connect(). */
    while (clawt_ipc_server_count_clients(server) == 0)
        g_main_context_iteration(NULL, TRUE);

    count_connected_sockets(port, &total, &armed);

    g_assert_cmpuint(total, ==, 2);
    g_assert_cmpuint(armed, ==, 2);

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

    g_test_add_func("/keepalive/tcp-socket-is-armed",
                    test_a_tcp_socket_is_armed);
    g_test_add_func("/keepalive/noticed-before-a-request-gives-up",
                    test_a_dead_route_is_noticed_before_a_request_gives_up);
    g_test_add_func("/keepalive/unix-socket-is-left-alone",
                    test_a_unix_socket_is_left_alone);
    g_test_add_func("/keepalive/both-ends-are-armed",
                    test_both_ends_of_a_tcp_connection_are_armed);

    return g_test_run();
}
