/*
 * test-client-retry.c - A daemon whose loop is behind is not a daemon gone
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The control socket is accepted on the daemon's main loop.  While that
 * loop is held, connections queue in the kernel's listen backlog, and
 * once the backlog is full a connect fails at once with EAGAIN --
 * "Could not connect: Resource temporarily unavailable" -- which is
 * exactly what every policy hook printed while a daemon sat in fsync().
 * It is not a flood: the backlog GLib gives a GSocketService is ten, so
 * the eleventh-or-so client of a loop that is a second late is refused.
 *
 * The test holds the loop by not iterating it, fills the backlog the
 * way the hooks did, shows the one-shot connect failing as they failed,
 * and then lets the loop go after a moment: a connect that retries gets
 * through, and one that does not cannot.
 */

#include <clawtilla.h>

#include "clawt-test-util.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>

/* Far more than any backlog; the loop stops at the first refusal. */
#define MAX_FILLERS 4096

/* How long the "daemon" stays behind before its loop runs again. */
#define HELD_MS 300

static JsonNode *
answer_nothing(JsonNode *request, gpointer user_data)
{
    (void)user_data;

    return clawt_ipc_response_new(request, NULL);
}

typedef struct {
    gint stop;
} Loop;

/*
 * The daemon's loop, let go after HELD_MS.  It runs on this thread so
 * the test's own thread can sit in the client's connect meanwhile; the
 * client works on a context of its own, so the two never contend.
 */
static gpointer
run_loop_later(gpointer user_data)
{
    Loop *loop = user_data;

    g_usleep(HELD_MS * 1000);

    while (!g_atomic_int_get(&loop->stop)) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(200);
    }

    return NULL;
}

typedef struct {
    gchar          *dir;
    gchar          *socket_path;
    ClawtIpcServer *server;
    GPtrArray      *fillers;
    GMainContext   *client_context;
} Fixture;

/*
 * A server whose loop nobody is turning, with its backlog full.
 *
 * Returns: how many connections the backlog took
 */
static guint
fixture_setup(Fixture *fixture)
{
    g_autoptr(GSocketClient) raw = g_socket_client_new();
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    fixture->dir = g_dir_make_tmp("clawt-retry-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->socket_path = g_build_filename(fixture->dir, "d.sock", NULL);

    fixture->server = clawt_ipc_server_new(fixture->socket_path);
    clawt_ipc_server_set_handler(fixture->server, answer_nothing, NULL, NULL);
    g_assert_true(clawt_ipc_server_start(fixture->server, &error));
    g_assert_no_error(error);

    fixture->fillers = g_ptr_array_new_with_free_func(g_object_unref);
    address = g_unix_socket_address_new(fixture->socket_path);

    for (i = 0; i < MAX_FILLERS; i++) {
        GSocketConnection *connection =
            g_socket_client_connect(raw, G_SOCKET_CONNECTABLE(address), NULL,
                                    &error);

        if (connection == NULL)
            break;

        g_ptr_array_add(fixture->fillers, connection);
    }

    /* The refusal the hooks saw: the daemon is there, its backlog is not. */
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
    g_assert_nonnull(strstr(error->message, "Resource temporarily unavailable"));
    g_assert_cmpuint(fixture->fillers->len, <, MAX_FILLERS);

    fixture->client_context = g_main_context_new();
    g_main_context_push_thread_default(fixture->client_context);

    return fixture->fillers->len;
}

static void
fixture_teardown(Fixture *fixture)
{
    g_main_context_pop_thread_default(fixture->client_context);
    while (g_main_context_iteration(fixture->client_context, FALSE))
        ;
    g_main_context_unref(fixture->client_context);

    g_ptr_array_unref(fixture->fillers);
    clawt_ipc_server_stop(fixture->server);
    while (g_main_context_iteration(NULL, FALSE))
        ;
    g_object_unref(fixture->server);

    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->socket_path);
    g_free(fixture->dir);
}

/* The hook as it was: one try, refused while the loop is behind. */
static void
test_one_try_is_refused_while_the_loop_is_held(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    guint queued = fixture_setup(&fixture);

    g_test_message("the listen backlog took %u connections", queued);

    client = clawt_client_new(fixture.socket_path);
    g_assert_false(clawt_client_connect(client, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
    g_assert_nonnull(strstr(error->message, "could not reach the daemon"));

    /* A budget of nothing is the same one try. */
    g_clear_error(&error);
    g_assert_false(clawt_client_connect_with_retry(client, 0, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);

    g_clear_object(&client);
    fixture_teardown(&fixture);
}

/* A loop that is a moment late costs a moment, not a refused call. */
static void
test_a_retry_gets_through_once_the_loop_runs(void)
{
    Fixture fixture = { 0 };
    Loop loop = { 0 };
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    GThread *thread;
    gint64 started;
    gint64 waited_ms;

    fixture_setup(&fixture);

    client = clawt_client_new(fixture.socket_path);
    thread = g_thread_new("held-loop", run_loop_later, &loop);

    started = g_get_monotonic_time();
    g_assert_true(clawt_client_connect_with_retry(client, 5000, &error));
    g_assert_no_error(error);
    waited_ms = (g_get_monotonic_time() - started) / 1000;

    /* It really did wait for the loop, and not for its whole budget. */
    g_assert_cmpint(waited_ms, >=, HELD_MS - 50);
    g_assert_cmpint(waited_ms, <, 5000);

    clawt_client_disconnect(client);
    g_clear_object(&client);

    g_atomic_int_set(&loop.stop, 1);
    g_thread_join(thread);

    fixture_teardown(&fixture);
}

/* A socket with nobody on it is retried too, and given up on in time. */
static void
test_no_daemon_gives_up_within_the_budget(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("clawt-retry-none-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "absent.sock", NULL);
    g_autoptr(ClawtClient) client = clawt_client_new(path);
    gint64 started = g_get_monotonic_time();
    gint64 waited_ms;

    g_assert_false(clawt_client_connect_with_retry(client, 400, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    waited_ms = (g_get_monotonic_time() - started) / 1000;

    g_assert_cmpint(waited_ms, >=, 350);
    g_assert_cmpint(waited_ms, <, 2000);

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/client-retry/one-try-refused-while-held",
                    test_one_try_is_refused_while_the_loop_is_held);
    g_test_add_func("/client-retry/retry-gets-through",
                    test_a_retry_gets_through_once_the_loop_runs);
    g_test_add_func("/client-retry/no-daemon-gives-up",
                    test_no_daemon_gives_up_within_the_budget);

    return g_test_run();
}
