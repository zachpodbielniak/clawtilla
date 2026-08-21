/*
 * test-daemon.c - The fleet, assembled
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * These run a real daemon against a real socket with a real client, because
 * the interesting failures here are between the pieces rather than inside
 * any one of them.
 */

#include <clawtilla.h>

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} Fixture;

static void
remove_tree(const gchar *path)
{
    g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
    const gchar *name;

    if (dir == NULL)
        return;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *child = g_build_filename(path, name, NULL);

        if (g_file_test(child, G_FILE_TEST_IS_DIR))
            remove_tree(child);
        else
            g_unlink(child);
    }

    g_rmdir(path);
}

static void
fixture_setup(Fixture *fixture, const gchar *extra_yaml)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-daemon-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);

    /*
     * The IPC socket goes in the temporary directory rather than the real
     * runtime dir, so a test never collides with the developer's own
     * running daemon.
     */
    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "%s",
        fixture->dir, fixture->dir,
        extra_yaml != NULL ? extra_yaml : "");

    g_file_set_contents(fixture->config_path, yaml, -1, &error);
    g_assert_no_error(error);

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    /*
     * Iterate once after stopping.  Closing a GSocketListener finishes
     * its outstanding accept on the *next* loop iteration, and until that
     * runs the listener, its sources and its sockets are all still
     * referenced.  A real daemon iterates anyway; a test that skipped it
     * would report the whole socket stack as leaked and bury the leaks
     * that are actually ours.
     */
    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    g_clear_pointer(&fixture->context, g_main_context_unref);

    if (fixture->dir != NULL)
        remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->config_path, g_free);
}

static JsonNode *
request(Fixture *fixture, const gchar *kind, const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

    if (payload_json != NULL) {
        g_autoptr(JsonParser) parser = json_parser_new();

        g_assert_true(json_parser_load_from_data(parser, payload_json, -1,
                                                 NULL));
        clawt_ipc_frame_set_payload(
            frame, json_node_copy(json_parser_get_root(parser)));
    }

    return clawt_daemon_handle_request(fixture->daemon, frame);
}

/* ── Starting up ─────────────────────────────────────────────────── */

static void
test_starts_with_an_empty_config(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    /* Both sockets exist and are the daemon's. */
    g_assert_nonnull(clawt_daemon_get_link_server(fixture.daemon));
    g_assert_nonnull(clawt_daemon_get_ipc_server(fixture.daemon));

    fixture_teardown(&fixture);
}

/*
 * A second daemon on the same socket is refused rather than quietly
 * taking over.  Two daemons on one fleet would each hold half the agents
 * and neither would know.
 */
static void
test_refuses_a_second_daemon(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtDaemon) second = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    second = clawt_daemon_new(fixture.config_path, fixture.context);

    g_assert_false(clawt_daemon_start(second, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);

    fixture_teardown(&fixture);
}

/*
 * An agent clawtilla cannot understand becomes a shadow; the fleet still
 * starts.  This is what lets a config written by a newer build load in an
 * older one.
 */
static void
test_a_bad_agent_does_not_stop_the_fleet(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *agents;
    guint i;
    gboolean saw_shadow = FALSE;
    gboolean saw_good = FALSE;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: good\n"
        "  - id: strange\n"
        "    computer:\n"
        "      type: teleporter\n");

    /*
     * The warning is the point, not an accident: an agent that quietly
     * disabled itself would be far worse than one that says why.  GTest
     * makes g_warning fatal, so it is expected rather than silenced.
     */
    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*unknown computer type*");

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    g_test_assert_expected_messages();

    reply = request(&fixture, "agent.list", NULL);
    agents = json_object_get_array_member(
        json_object_get_object_member(json_node_get_object(reply),
                                      "payload"),
        "agents");

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *state = json_object_get_string_member(agent, "state");

        if (g_strcmp0(json_object_get_string_member(agent, "id"),
                      "strange") == 0 && g_strcmp0(state, "shadow") == 0)
            saw_shadow = TRUE;

        if (g_strcmp0(json_object_get_string_member(agent, "id"),
                      "good") == 0)
            saw_good = TRUE;
    }

    g_assert_true(saw_shadow);
    g_assert_true(saw_good);

    fixture_teardown(&fixture);
}

/* ── The client surface ──────────────────────────────────────────── */

static JsonObject *
payload_of(JsonNode *reply)
{
    return json_object_get_object_member(json_node_get_object(reply),
                                         "payload");
}

static void
test_status_reports_the_fleet(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *status;

    fixture_setup(&fixture, "agents:\n  - id: chief\n  - id: researcher\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "control.status", NULL);
    status = payload_of(reply);

    g_assert_cmpint(json_object_get_int_member(status, "agents"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(status, "version"), ==,
                    CLAWT_VERSION_STRING);

    fixture_teardown(&fixture);
}

/*
 * A request this build does not have names itself in the reply, so a
 * newer client learns what is missing rather than only that something
 * failed.
 */
static void
test_unknown_request_names_itself(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    const gchar *message;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "agent.teleport", NULL);

    g_assert_true(clawt_ipc_frame_is_error(reply));
    message = json_object_get_string_member(json_node_get_object(reply),
                                            "error");
    g_assert_nonnull(strstr(message, "agent.teleport"));

    fixture_teardown(&fixture);
}

/* A credential is reported by reference, never by value. */
static void
test_credentials_are_never_sent_in_full(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *secret_path = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *yaml = NULL;
    JsonObject *agent;
    JsonObject *credentials;

    fixture_setup(&fixture, NULL);

    /* Rewrite the config with a real secret file to resolve. */
    secret_path = g_build_filename(fixture.dir, "api.key", NULL);
    g_file_set_contents(secret_path, "sk-super-secret-value", -1, NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "agents:\n"
        "  - id: chief\n"
        "    credentials:\n"
        "      api_key:\n"
        "        file: \"%s\"\n",
        fixture.dir, fixture.dir, secret_path);

    g_file_set_contents(fixture.config_path, yaml, -1, NULL);

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "agent.show", "{\"agent\":\"chief\"}");
    agent = json_object_get_object_member(payload_of(reply), "agent");
    credentials = json_object_get_object_member(agent, "credentials");

    g_assert_true(json_object_has_member(credentials, "api_key"));
    g_assert_null(strstr(json_object_get_string_member(credentials,
                                                       "api_key"),
                         "sk-super-secret-value"));

    fixture_teardown(&fixture);
}

/* ── Rendering ───────────────────────────────────────────────────── */

/*
 * The rendered file is what the agent actually runs, so the clawtilla
 * channel block has to be in it: without it the agent starts and never
 * dials home, which looks like a network problem rather than a config
 * one.
 */
static void
test_rendered_config_joins_the_fleet(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    const gchar *rendered;

    fixture_setup(&fixture, "agents:\n  - id: chief\n    name: \"Chief\"\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "config.render", "{\"agent\":\"chief\"}");
    rendered = json_object_get_string_member(payload_of(reply), "yaml");

    g_assert_nonnull(strstr(rendered, "clawtilla:"));
    g_assert_nonnull(strstr(rendered, "agent_id: \"chief\""));
    g_assert_nonnull(strstr(rendered, "token_file:"));
    g_assert_nonnull(strstr(rendered, "Do not edit"));

    fixture_teardown(&fixture);
}

/* Same input, same bytes: a rewrite on every start must not churn. */
static void
test_rendering_is_deterministic(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    {
        g_autoptr(JsonNode) reply =
            request(&fixture, "config.render", "{\"agent\":\"chief\"}");

        first = g_strdup(json_object_get_string_member(payload_of(reply),
                                                       "yaml"));
    }

    {
        g_autoptr(JsonNode) reply =
            request(&fixture, "config.render", "{\"agent\":\"chief\"}");

        second = g_strdup(json_object_get_string_member(payload_of(reply),
                                                        "yaml"));
    }

    g_assert_cmpstr(first, ==, second);

    fixture_teardown(&fixture);
}

/*
 * A libreclaw setting clawtilla has never heard of still reaches the
 * agent.  Without passthrough, every new libreclaw option would need a
 * clawtilla release before anyone could use it.
 */
static void
test_passthrough_reaches_the_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    const gchar *rendered;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    libreclaw:\n"
        "      some_future_thing:\n"
        "        enabled: true\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "config.render", "{\"agent\":\"chief\"}");
    rendered = json_object_get_string_member(payload_of(reply), "yaml");

    g_assert_nonnull(strstr(rendered, "some_future_thing"));

    fixture_teardown(&fixture);
}

/* The token is created once and then left alone: regenerating it would
 * lock out an agent that is already connected. */
static void
test_token_is_stable_across_renders(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    token_path = g_build_filename(fixture.dir, "state", "agents", "chief",
                                  "token", NULL);

    g_assert_true(g_file_get_contents(token_path, &first, NULL, NULL));
    g_assert_cmpuint(strlen(first), >=, 32);

    g_assert_true(clawt_daemon_reload(fixture.daemon, NULL));
    g_assert_true(g_file_get_contents(token_path, &second, NULL, NULL));

    g_assert_cmpstr(first, ==, second);

    fixture_teardown(&fixture);
}

/* ── Messaging ───────────────────────────────────────────────────── */

/*
 * Messaging a stopped agent queues rather than failing.  This is the
 * whole reason the mailbox is durable.
 */
static void
test_message_to_a_stopped_agent_queues(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) send = NULL;
    g_autoptr(JsonNode) listing = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n  - id: researcher\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    send = request(&fixture, "msg.send",
                   "{\"target\":\"researcher\",\"body\":\"have a look\"}");

    g_assert_false(clawt_ipc_frame_is_error(send));
    g_assert_cmpint(json_object_get_int_member(payload_of(send), "queued"),
                    ==, 1);

    listing = request(&fixture, "mailbox.list",
                      "{\"agent\":\"researcher\"}");

    g_assert_cmpint(json_object_get_int_member(payload_of(listing), "depth"),
                    ==, 1);

    fixture_teardown(&fixture);
}

/* Messaging somebody who does not exist says so rather than vanishing. */
static void
test_message_to_nobody_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "msg.send",
                    "{\"target\":\"ghost\",\"body\":\"hello?\"}");

    g_assert_true(clawt_ipc_frame_is_error(reply));

    fixture_teardown(&fixture);
}

/* A room fans out to every member except the sender. */
static void
test_room_post_reaches_every_member(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "  - id: researcher\n"
        "  - id: writer\n"
        "rooms:\n"
        "  - id: standup\n"
        "    members: [chief, researcher, writer]\n"
        "    require_mention: false\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "msg.send",
                    "{\"target\":\"standup\",\"from\":\"chief\","
                    "\"body\":\"morning\"}");

    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "queued"),
                    ==, 2);

    fixture_teardown(&fixture);
}

/* ── Live client over the socket ─────────────────────────────────── */

/*
 * A wrapper rather than casting clawt_client_connect to GThreadFunc:
 * the signatures genuinely differ, and the cast is undefined behaviour
 * that happens to work until it does not.
 */
typedef struct {
    ClawtClient *client;
    gint         done;      /* atomic */
    gboolean     ok;
} ConnectJob;

static gpointer
connect_in_thread(gpointer user_data)
{
    ConnectJob *job = user_data;
    g_autoptr(GError) error = NULL;

    job->ok = clawt_client_connect(job->client, &error);

    if (!job->ok)
        g_message("connect failed: %s", error->message);

    g_atomic_int_set(&job->done, 1);

    return NULL;
}

static void
test_a_client_can_talk_over_the_socket(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtClient) client = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    g_main_context_push_thread_default(fixture.context);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    client = clawt_client_new(socket_path);

    /*
     * The connect handshake is synchronous on the client side and served
     * from the daemon's main context, so the loop has to be pumped while
     * it happens.  Doing it in a thread keeps the test honest -- this is
     * exactly the arrangement a real client is in.
     */
    {
        g_autoptr(GThread) thread = NULL;
        ConnectJob job = { client, 0, FALSE };
        gint64 deadline = g_get_monotonic_time() + (10 * G_USEC_PER_SEC);

        thread = g_thread_new("connect", connect_in_thread, &job);

        /*
         * Pumped until the connect thread says it is finished, not for a
         * fixed number of iterations.  A fixed count passes on a fast
         * machine and hangs in g_thread_join() on a loaded one, which is
         * the worst possible failure for a test suite.
         */
        while (!g_atomic_int_get(&job.done) &&
               g_get_monotonic_time() < deadline)
            g_main_context_iteration(fixture.context, FALSE);

        g_assert_true(g_atomic_int_get(&job.done));
        g_thread_join(g_steal_pointer(&thread));
        g_assert_true(job.ok);
    }

    g_assert_true(clawt_client_is_connected(client));

    fixture_teardown(&fixture);
}

/* ── Shutdown ────────────────────────────────────────────────────── */

/* Stopping removes the sockets, so the next start does not have to clear
 * up after this one. */
static void
test_stop_removes_the_sockets(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *socket_path = NULL;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    g_assert_true(g_file_test(socket_path, G_FILE_TEST_EXISTS));

    clawt_daemon_stop(fixture.daemon);

    g_assert_false(g_file_test(socket_path, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

/*
 * A reload with a broken config keeps the old one.  Swapping in a config
 * and then discovering it is malformed would leave the daemon running on
 * half of each.
 */
static void
test_a_broken_reload_keeps_the_old_config(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    g_file_set_contents(fixture.config_path,
                        "daemon:\n  state_dir: \"[unclosed\n\t- tab\n",
                        -1, NULL);

    g_assert_false(clawt_daemon_reload(fixture.daemon, &error));
    g_assert_nonnull(error);

    /* Still serving the fleet it had. */
    reply = request(&fixture, "control.status", NULL);
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "agents"),
                    ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A client that disconnects while a read is in flight must not take the
 * daemon with it.
 *
 * The read completes after the client has been dropped from the server's
 * list, and before the client was reference counted it did so holding a
 * pointer to freed memory.  ASAN catches this; without the test it went
 * unnoticed because the suite never iterated the loop after teardown.
 */
static void
test_a_client_vanishing_mid_read_is_survivable(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *socket_path = NULL;
    g_autoptr(GSocketClient) raw = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    gint64 deadline;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    g_main_context_push_thread_default(fixture.context);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    address = g_unix_socket_address_new(socket_path);
    raw = g_socket_client_new();

    connection = g_socket_client_connect(raw, G_SOCKET_CONNECTABLE(address),
                                         NULL, &error);
    g_assert_no_error(error);

    /* Let the server accept it and start reading. */
    deadline = g_get_monotonic_time() + (2 * G_USEC_PER_SEC);

    while (clawt_ipc_server_count_clients(
               clawt_daemon_get_ipc_server(fixture.daemon)) == 0 &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, FALSE);

    g_assert_cmpuint(clawt_ipc_server_count_clients(
                         clawt_daemon_get_ipc_server(fixture.daemon)),
                     ==, 1);

    /* Vanish without saying goodbye. */
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_clear_object(&connection);

    deadline = g_get_monotonic_time() + (2 * G_USEC_PER_SEC);

    while (clawt_ipc_server_count_clients(
               clawt_daemon_get_ipc_server(fixture.daemon)) > 0 &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, FALSE);

    /* The daemon noticed, dropped it, and is still answering. */
    g_assert_cmpuint(clawt_ipc_server_count_clients(
                         clawt_daemon_get_ipc_server(fixture.daemon)),
                     ==, 0);

    {
        g_autoptr(JsonNode) reply = request(&fixture, "control.status", NULL);

        g_assert_false(clawt_ipc_frame_is_error(reply));
    }

    g_main_context_pop_thread_default(fixture.context);
    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/daemon/starts", test_starts_with_an_empty_config);
    g_test_add_func("/daemon/one-at-a-time", test_refuses_a_second_daemon);
    g_test_add_func("/daemon/shadow-agent",
                    test_a_bad_agent_does_not_stop_the_fleet);

    g_test_add_func("/daemon/status", test_status_reports_the_fleet);
    g_test_add_func("/daemon/unknown-request",
                    test_unknown_request_names_itself);
    g_test_add_func("/daemon/credentials-are-references",
                    test_credentials_are_never_sent_in_full);

    g_test_add_func("/daemon/render-joins-fleet",
                    test_rendered_config_joins_the_fleet);
    g_test_add_func("/daemon/render-deterministic",
                    test_rendering_is_deterministic);
    g_test_add_func("/daemon/passthrough", test_passthrough_reaches_the_agent);
    g_test_add_func("/daemon/token-stable",
                    test_token_is_stable_across_renders);

    g_test_add_func("/daemon/queue-for-stopped-agent",
                    test_message_to_a_stopped_agent_queues);
    g_test_add_func("/daemon/message-to-nobody",
                    test_message_to_nobody_is_refused);
    g_test_add_func("/daemon/room-fanout",
                    test_room_post_reaches_every_member);

    g_test_add_func("/daemon/client-over-socket",
                    test_a_client_can_talk_over_the_socket);

    g_test_add_func("/daemon/client-vanishes-mid-read",
                    test_a_client_vanishing_mid_read_is_survivable);

    g_test_add_func("/daemon/stop-removes-sockets",
                    test_stop_removes_the_sockets);
    g_test_add_func("/daemon/broken-reload",
                    test_a_broken_reload_keeps_the_old_config);

    return g_test_run();
}
