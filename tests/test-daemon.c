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

#include "clawt-test-util.h"

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} Fixture;

/*
 * Writes the fleet's config.yaml.  Separate from the setup so a test can
 * rewrite it and reload, which is what an operator editing the file and
 * running `clawtilla config edit` actually does.
 */
static void
fixture_write_config(Fixture *fixture, const gchar *extra_yaml)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    /*
     * The IPC socket goes in the temporary directory rather than the real
     * runtime dir, so a test never collides with the developer's own
     * running daemon.
     */
    yaml = g_strdup_printf(
        "daemon:\n"
        /*
         * No tailnet listener.  make test is hermetic -- it opens no
         * network socket at all -- and on a machine that has a tailnet
         * this would also collide with the developer's own running
         * daemon on the same address and port.
         */
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        /*
         * And nowhere near the real fleet.  `workspace_root`
         * defaults to ~/.clawtilla/agents, so without this every
         * agent a test creates is scaffolded into the developer's
         * own agent directory -- indistinguishable afterwards from
         * one they meant to keep.  The socket and the state dir
         * were already pinned here; this is the third thing that
         * escapes a temporary directory if nobody says otherwise.
         */
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "%s",
        fixture->dir, fixture->dir, fixture->dir,
        extra_yaml != NULL ? extra_yaml : "");

    g_file_set_contents(fixture->config_path, yaml, -1, &error);
    g_assert_no_error(error);
}

static void
fixture_setup(Fixture *fixture, const gchar *extra_yaml)
{
    fixture->dir = g_dir_make_tmp("clawt-daemon-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);

    fixture_write_config(fixture, extra_yaml);

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
        clawt_test_remove_tree(fixture->dir);

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
        /*
         * No tailnet listener.  make test is hermetic -- it opens no
         * network socket at all -- and on a machine that has a tailnet
         * this would also collide with the developer's own running
         * daemon on the same address and port.
         */
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n"
        "  - id: chief\n"
        "    credentials:\n"
        "      api_key:\n"
        "        file: \"%s\"\n",
        fixture.dir, fixture.dir, fixture.dir, secret_path);

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

static void
test_a_client_can_talk_over_the_socket(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtClient) client = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    g_main_context_push_thread_default(fixture.context);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    client = clawt_client_new(socket_path);

    /*
     * No thread.  The client and the daemon share this context, and
     * connecting pumps it rather than blocking on a read -- which is
     * exactly what an in-process host needs, and what used to deadlock.
     */
    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);
    g_assert_true(clawt_client_is_connected(client));

    g_main_context_pop_thread_default(fixture.context);
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
 * A render clawtilla refuses has to reach whoever asked for the reload.
 *
 * The refusal itself is right: a `libreclaw:` passthrough that redeclares
 * `session:` would win outright and delete the agent's own persist_dir.
 * What was wrong is that nothing said so.  `control.reload` answered plain
 * success, `clawtilla config edit` printed "Reloaded.", and the agent's
 * config.yaml was left at its previous contents -- so the passthrough
 * looked ignored rather than rejected, and the only clue was a g_warning
 * in the journal nobody was told to read.
 */
static void
test_reload_reports_a_refused_render(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *rendered_path = NULL;
    g_autofree gchar *rendered = NULL;
    JsonObject *payload;
    JsonArray *refused;
    JsonObject *entry;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    rendered_path = g_build_filename(fixture.dir, "state", "agents",
                                     "chief", "config.yaml", NULL);
    g_assert_true(g_file_test(rendered_path, G_FILE_TEST_EXISTS));

    /* The operator edits the file and asks the daemon to reload it. */
    fixture_write_config(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    libreclaw:\n"
        "      session:\n"
        "        persist_dir: \"/tmp/somewhere-else\"\n");

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*redeclares*");
    reply = request(&fixture, "control.reload", NULL);
    g_test_assert_expected_messages();

    /*
     * A response, not an error: the reload itself happened, and every
     * other agent in the fleet was rendered from the new file.  What the
     * caller has to be told is which agents were left behind.
     */
    payload = payload_of(reply);
    g_assert_nonnull(payload);
    g_assert_true(json_object_has_member(payload, "refused"));

    refused = json_object_get_array_member(payload, "refused");
    g_assert_cmpuint(json_array_get_length(refused), ==, 1);

    entry = json_array_get_object_element(refused, 0);
    g_assert_cmpstr(json_object_get_string_member(entry, "agent"), ==,
                    "chief");
    g_assert_nonnull(strstr(json_object_get_string_member(entry, "message"),
                            "redeclares"));

    /* And it is telling the truth: the file on disk did not change. */
    g_assert_true(g_file_get_contents(rendered_path, &rendered, NULL, NULL));
    g_assert_null(strstr(rendered, "/tmp/somewhere-else"));

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

/*
 * A request made after subscribing must still work.
 *
 * This is the sequence every real client follows -- connect, subscribe,
 * then ask for things -- and it was broken: subscribing started an async
 * reader, and a synchronous request then read the same stream itself.
 * The two raced, one got a partial frame, and the client tore down a
 * healthy connection and reported that the daemon had gone.
 */
static void
test_requests_work_after_subscribing(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtClient) client = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autoptr(GError) error = NULL;
    gboolean resumed = FALSE;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    g_main_context_push_thread_default(fixture.context);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    client = clawt_client_new(socket_path);

    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_client_subscribe(client, 0, &resumed, &error));
    g_assert_no_error(error);
    g_assert_true(resumed);

    /* Several in a row: one lucky pass would not prove much. */
    {
        guint i;

        for (i = 0; i < 5; i++) {
            g_autoptr(JsonNode) reply = NULL;

            reply = clawt_client_request(client, "agent.list", NULL, &error);

            g_assert_no_error(error);
            g_assert_nonnull(reply);
            g_assert_cmpuint(
                json_array_get_length(
                    json_object_get_array_member(json_node_get_object(reply),
                                                 "agents")),
                ==, 1);
        }
    }

    /* And the connection is still up afterwards. */
    g_assert_true(clawt_client_is_connected(client));

    g_main_context_pop_thread_default(fixture.context);
    fixture_teardown(&fixture);
}

/*
 * A request with no payload, made while disconnected, fails cleanly.
 *
 * It used to unref the %NULL payload, which trips an assertion inside
 * json-glib -- a confusing way to be told the daemon is not there.
 */
static void
test_a_request_with_no_payload_while_disconnected(void)
{
    g_autoptr(ClawtClient) client = clawt_client_new("/nonexistent/d.sock");
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_client_request(client, "agent.list", NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED);
}

/* The model catalogue reaches a client, grouped by provider. */
static void
test_the_model_catalog_is_served(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *providers;
    guint i;
    gboolean saw_claude_code = FALSE;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "model.list", NULL);
    providers = json_object_get_array_member(payload_of(reply), "providers");

    g_assert_cmpuint(json_array_get_length(providers), >, 0);

    for (i = 0; i < json_array_get_length(providers); i++) {
        JsonObject *provider = json_array_get_object_element(providers, i);
        JsonArray *models;

        g_assert_true(json_object_has_member(provider, "label"));

        /*
         * open_ended must be reported, or a client cannot know to offer a
         * way to type a model the curated list has not heard of.
         */
        g_assert_true(json_object_has_member(provider, "open_ended"));

        models = json_object_get_array_member(provider, "models");

        if (g_strcmp0(json_object_get_string_member(provider, "id"),
                      "claude-code") == 0) {
            saw_claude_code = TRUE;
            g_assert_cmpuint(json_array_get_length(models), >, 0);
        }
    }

    g_assert_true(saw_claude_code);

    fixture_teardown(&fixture);
}

/* An agent created with a provider keeps it. */
static void
test_creating_an_agent_records_its_provider(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) created = NULL;
    ClawtAgentConfig *config;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    created = request(&fixture, "agent.create",
                      "{\"id\":\"researcher\",\"provider\":\"ollama\","
                      "\"model\":\"llama3.3\"}");

    g_assert_false(clawt_ipc_frame_is_error(created));

    config = clawt_config_get_agent(
        clawt_daemon_get_config(fixture.daemon), "researcher");

    g_assert_nonnull(config);
    g_assert_cmpstr(clawt_agent_config_get_string(config, "model.provider"),
                    ==, "ollama");
    g_assert_cmpstr(clawt_agent_config_get_string(config, "model.model"),
                    ==, "llama3.3");

    fixture_teardown(&fixture);
}

/*
 * Creating an agent must not disturb the ones already running.
 *
 * agent.create used to rebuild the whole fleet, destroying every other
 * agent's live object -- runtime, computer and link went with it -- while
 * the link server carried on holding their connections.
 */
static void
test_creating_an_agent_leaves_the_others_alone(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) created = NULL;
    ClawtAgent *chief_before;
    ClawtAgent *chief_after;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    chief_before = clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture.daemon), "chief");
    g_assert_nonnull(chief_before);

    created = request(&fixture, "agent.create",
                      "{\"id\":\"researcher\"}");
    g_assert_false(clawt_ipc_frame_is_error(created));

    chief_after = clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture.daemon), "chief");

    /* The same object, not a replacement. */
    g_assert_true(chief_before == chief_after);
    g_assert_nonnull(clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture.daemon), "researcher"));

    fixture_teardown(&fixture);
}

/*
 * A reload has to reach the fleet.
 *
 * The manager holds its own reference to the configuration, so a reload
 * that swapped only the daemon's left the fleet reading the old one for
 * ever -- an agent added to the file never appeared.
 */
static void
test_reload_reaches_the_fleet(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    g_assert_cmpuint(
        clawt_agent_manager_list(clawt_daemon_get_agents(fixture.daemon))->len,
        ==, 1);

    yaml = g_strdup_printf(
        "daemon:\n  tailscale: false\n"
        "  state_dir: \"%s/state\"\n  socket: \"%s/daemon.sock\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n  - id: chief\n  - id: researcher\n",
        fixture.dir, fixture.dir, fixture.dir);

    g_file_set_contents(fixture.config_path, yaml, -1, NULL);

    g_assert_true(clawt_daemon_reload(fixture.daemon, NULL));

    g_assert_cmpuint(
        clawt_agent_manager_list(clawt_daemon_get_agents(fixture.daemon))->len,
        ==, 2);
    g_assert_nonnull(clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture.daemon), "researcher"));

    /* And one removed from the file goes away again. */
    g_free(yaml);
    yaml = g_strdup_printf(
        "daemon:\n  tailscale: false\n"
        "  state_dir: \"%s/state\"\n  socket: \"%s/daemon.sock\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n  - id: chief\n",
        fixture.dir, fixture.dir, fixture.dir);

    g_file_set_contents(fixture.config_path, yaml, -1, NULL);
    g_assert_true(clawt_daemon_reload(fixture.daemon, NULL));

    g_assert_null(clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture.daemon), "researcher"));

    fixture_teardown(&fixture);
}

/* A mount of the state directory exposes every agent's token. */
static void
test_the_state_directory_cannot_be_mounted(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = NULL;
    ClawtAgent *agent;

    fixture_setup(&fixture, NULL);

    yaml = g_strdup_printf(
        "daemon:\n  tailscale: false\n"
        "  state_dir: \"%s/state\"\n  socket: \"%s/daemon.sock\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n"
        "  - id: sneaky\n"
        "    computer:\n"
        "      type: container\n"
        "      mounts:\n"
        "        - source: \"%s/state\"\n"
        "          target: \"/loot\"\n"
        "          mode: rw\n",
        fixture.dir, fixture.dir, fixture.dir, fixture.dir);

    g_file_set_contents(fixture.config_path, yaml, -1, NULL);

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));
    g_test_assert_expected_messages();

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "sneaky");

    /*
     * Refused at start rather than mounted.  Whether it shadows or simply
     * fails to start, what matters is that it never gets the directory.
     */
    if (agent != NULL) {
        g_autoptr(GError) error = NULL;

        g_assert_false(clawt_daemon_start_agent(fixture.daemon, "sneaky",
                                                &error));
        g_assert_nonnull(error);
    }

    fixture_teardown(&fixture);
}

typedef struct {
    ClawtClient *client;
    gboolean     nested_ok;
    gboolean     nested_ran;
    gdouble      nested_seconds;
} NestedProbe;

/*
 * A client refreshing itself when an event arrives -- which is what every
 * real client does.
 */
static void
on_event_make_a_request(ClawtClient *client, ClawtEvent *event,
                        gpointer user_data)
{
    NestedProbe *probe = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    gint64 started = g_get_monotonic_time();

    (void)client;
    (void)event;

    if (probe->nested_ran)
        return;

    probe->nested_ran = TRUE;
    reply = clawt_client_request(probe->client, "agent.list", NULL, &error);
    probe->nested_seconds = (g_get_monotonic_time() - started) / 1e6;
    probe->nested_ok = (reply != NULL);
}

/*
 * A request made from inside an event handler must not deadlock.
 *
 * The reader used to re-arm only after dispatching, so a handler that
 * issued a request waited with no read outstanding -- nothing could read
 * the reply, and both that request and the one that triggered the event
 * sat there until the two-minute timeout.  In the GTK client that made
 * sending a message appear to do nothing at all.
 */
static void
test_a_request_from_an_event_handler_completes(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtClient) client = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autoptr(GError) error = NULL;
    NestedProbe probe = { 0 };
    gint64 deadline;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    g_main_context_push_thread_default(fixture.context);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    client = clawt_client_new(socket_path);
    probe.client = client;

    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    g_signal_connect(client, "event", G_CALLBACK(on_event_make_a_request),
                     &probe);
    g_assert_true(clawt_client_subscribe(client, 0, NULL, &error));

    /* Publishing anything reaches the handler. */
    clawt_event_bus_emit(clawt_daemon_get_event_bus(fixture.daemon),
                         "test.poke", "chief");

    deadline = g_get_monotonic_time() + (10 * G_USEC_PER_SEC);

    while (!probe.nested_ran && g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, FALSE);

    g_assert_true(probe.nested_ran);
    g_assert_true(probe.nested_ok);

    /* Promptly, not after a timeout. */
    g_assert_cmpfloat(probe.nested_seconds, <, 5.0);

    g_main_context_pop_thread_default(fixture.context);
    fixture_teardown(&fixture);
}

/* A chat with an agent is a room, and asking for it by agent id works. */
static void
test_history_by_agent_id(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) sent = NULL;
    g_autoptr(JsonNode) history = NULL;
    JsonArray *messages;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    sent = request(&fixture, "msg.send",
                   "{\"target\":\"chief\",\"body\":\"hello\","
                   "\"from\":\"user\"}");
    g_assert_false(clawt_ipc_frame_is_error(sent));

    /*
     * The client knows the agent, not how a direct room is named.  Asking
     * by agent id used to answer "no such room", so every chat opened
     * empty.
     */
    history = request(&fixture, "room.history",
                      "{\"room\":\"chief\",\"as\":\"user\"}");

    g_assert_false(clawt_ipc_frame_is_error(history));

    messages = json_object_get_array_member(payload_of(history), "messages");
    g_assert_cmpuint(json_array_get_length(messages), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(messages, 0), "body"),
                    ==, "hello");

    fixture_teardown(&fixture);
}

/*
 * A send has to say whether anything is going to read it.
 *
 * The mailbox takes a message for a stopped agent on purpose -- durable
 * queues are the point -- but a client with no way to tell "queued" from
 * "delivered" shows a spinner for an agent that is never going to answer.
 * Both clients now say so, and both read it from here.
 */
static void
test_send_reports_the_target_state(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) sent = NULL;
    g_autoptr(JsonNode) to_room = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    sent = request(&fixture, "msg.send",
                   "{\"target\":\"chief\",\"body\":\"hello\","
                   "\"from\":\"user\"}");
    g_assert_false(clawt_ipc_frame_is_error(sent));
    g_assert_cmpint(json_object_get_int_member(payload_of(sent), "queued"),
                    ==, 1);
    g_assert_cmpstr(json_object_get_string_member(payload_of(sent),
                                                  "target_state"),
                    ==, "stopped");

    /*
     * A room has as many states as it has members, so there is no single
     * answer and the key is left out rather than guessed at.
     */
    to_room = request(&fixture, "msg.send",
                      "{\"target\":\"dm:chief:user\",\"body\":\"x\","
                      "\"from\":\"user\"}");
    g_assert_false(clawt_ipc_frame_is_error(to_room));
    g_assert_false(json_object_has_member(payload_of(to_room),
                                          "target_state"));

    fixture_teardown(&fixture);
}

/*
 * A flag sent as the string "true" has to mean true.
 *
 * Both bundled clients build payloads from string pairs -- their
 * build_payload(key, value, ...) helpers only ever emit strings -- and
 * the reader required a real JSON boolean, so every such flag silently
 * read as its default. That is the worst failure a boolean can have:
 * the request succeeds and does the other thing. `agent rm
 * --with-computer` removed the agent and left the container running,
 * reporting success.
 */
static void
test_string_booleans_are_understood(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *payload;

    g_assert_true(json_parser_load_from_data(
        parser,
        "{\"yes\":true,\"no\":false,\"s_yes\":\"true\","
        "\"s_no\":\"false\",\"s_upper\":\"TRUE\",\"one\":\"1\","
        "\"junk\":\"maybe\",\"number\":7}",
        -1, NULL));

    payload = json_node_get_object(json_parser_get_root(parser));

    /* Real booleans, unchanged. */
    g_assert_true(clawt_ipc_payload_boolean(payload, "yes", FALSE));
    g_assert_false(clawt_ipc_payload_boolean(payload, "no", TRUE));

    /* Strings, which is what the clients actually send. */
    g_assert_true(clawt_ipc_payload_boolean(payload, "s_yes", FALSE));
    g_assert_false(clawt_ipc_payload_boolean(payload, "s_no", TRUE));
    g_assert_true(clawt_ipc_payload_boolean(payload, "s_upper", FALSE));
    g_assert_true(clawt_ipc_payload_boolean(payload, "one", FALSE));

    /*
     * Anything else falls back rather than guessing. "maybe" is not a
     * boolean, and picking one for it would hide a client bug.
     */
    g_assert_true(clawt_ipc_payload_boolean(payload, "junk", TRUE));
    g_assert_false(clawt_ipc_payload_boolean(payload, "junk", FALSE));
    g_assert_true(clawt_ipc_payload_boolean(payload, "number", TRUE));
    g_assert_true(clawt_ipc_payload_boolean(payload, "absent", TRUE));
}

/*
 * The orchestration tools have to be reachable over IPC.
 *
 * They were served only over the agent's link, as mcp.request frames,
 * which assumed something on the agent side would relay them into its
 * AI session. Nothing did, and nothing could: an agent runs a CLI whose
 * only way of being given tools is a config naming an MCP server. This
 * verb is what clawtilla-mcp-server speaks so that server can exist --
 * and it is the agent's token, not the socket alone, that says who is
 * asking.
 */
static void
test_tool_rpc_needs_the_agents_token(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *token = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *good = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) listed = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    token_path = g_build_filename(fixture.dir, "state", "agents", "chief",
                                   "token", NULL);
    g_assert_true(g_file_get_contents(token_path, &token, NULL, NULL));
    g_strstrip(token);

    /* No token, no tools. */
    refused = request(&fixture, "tool.rpc",
                      "{\"agent\":\"chief\",\"request\":"
                      "{\"jsonrpc\":\"2.0\",\"id\":1,"
                      "\"method\":\"tools/list\"}}");
    g_assert_true(clawt_ipc_frame_is_error(refused));

    good = g_strdup_printf(
        "{\"agent\":\"chief\",\"token\":\"%s\",\"request\":"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}}",
        token);

    listed = request(&fixture, "tool.rpc", good);
    g_assert_false(clawt_ipc_frame_is_error(listed));

    {
        JsonObject *response = json_object_get_object_member(
            payload_of(listed), "response");
        JsonArray *tools = json_object_get_array_member(
            json_object_get_object_member(response, "result"), "tools");
        guint i;
        gboolean saw_ask = FALSE;

        g_assert_cmpuint(json_array_get_length(tools), >, 0);

        for (i = 0; i < json_array_get_length(tools); i++) {
            if (g_strcmp0(json_object_get_string_member(
                    json_array_get_object_element(tools, i), "name"),
                    "clawtilla_ask_agent") == 0)
                saw_ask = TRUE;
        }

        g_assert_true(saw_ask);
    }

    fixture_teardown(&fixture);
}

/*
 * And a tool call actually runs, rather than only being listed.
 */
static void
test_tool_rpc_runs_a_tool(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *token = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *call = NULL;
    g_autoptr(JsonNode) result = NULL;
    g_autoptr(JsonNode) queued = NULL;

    fixture_setup(&fixture,
                  "agents:\n  - id: chief\n  - id: worker\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    token_path = g_build_filename(fixture.dir, "state", "agents", "chief",
                                   "token", NULL);
    g_assert_true(g_file_get_contents(token_path, &token, NULL, NULL));
    g_strstrip(token);

    call = g_strdup_printf(
        "{\"agent\":\"chief\",\"token\":\"%s\",\"request\":"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"clawtilla_message_agent\","
        "\"arguments\":{\"agent_id\":\"worker\","
        "\"body\":\"through the mcp server\"}}}}",
        token);

    result = request(&fixture, "tool.rpc", call);
    g_assert_false(clawt_ipc_frame_is_error(result));

    /* The message is really in the worker's mailbox. */
    queued = request(&fixture, "mailbox.list", "{\"agent\":\"worker\"}");
    g_assert_false(clawt_ipc_frame_is_error(queued));
    g_assert_cmpuint(json_array_get_length(
        json_object_get_array_member(payload_of(queued), "items")), ==, 1);

    fixture_teardown(&fixture);
}


/* ── Who a conversation belongs to ───────────────────────────────── */

typedef struct {
    gchar *subject;
    gchar *from;
    gchar *to;
    guint  count;
} MessageWatch;

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    MessageWatch *watch = user_data;

    (void)bus;

    if (g_strcmp0(clawt_event_get_kind(event), "message") != 0)
        return;

    g_free(watch->subject);
    g_free(watch->from);
    g_free(watch->to);

    watch->subject = g_strdup(clawt_event_get_subject(event));
    watch->from = g_strdup(clawt_event_get_detail(event, "from"));
    watch->to = g_strdup(clawt_event_get_detail(event, "to"));
    watch->count++;
}

/*
 * Two agents talking to each other do not appear in the user's chat
 * with either of them.
 *
 * The report was that a reply went "to my chat instead of back to the
 * agent". The routing was right the whole time -- a message to an agent
 * goes into the direct room between sender and recipient, and the reply
 * comes back into the same one. What was wrong was the event: it named
 * the sender but not the room, so the GTK client matched on "is this
 * from the agent I am looking at", and a reply from that agent to one
 * of its peers matched.
 */
static void
test_agents_talking_stays_out_of_the_users_chat(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) sent = NULL;
    g_autoptr(JsonNode) replied = NULL;
    g_autoptr(JsonNode) between = NULL;
    g_autoptr(JsonNode) with_user = NULL;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    sent = request(&fixture, "msg.send",
                   "{\"from\":\"alpha\",\"target\":\"beta\","
                   "\"body\":\"summarise the last ten commits\"}");
    g_assert_nonnull(sent);
    g_assert_cmpint(json_object_get_int_member(payload_of(sent), "queued"),
                    ==, 1);

    replied = request(&fixture, "msg.send",
                      "{\"from\":\"beta\",\"target\":\"alpha\","
                      "\"body\":\"here they are\"}");
    g_assert_nonnull(replied);

    /* Both halves are in the room the two of them share. */
    between = request(&fixture, "room.history",
                      "{\"room\":\"beta\",\"as\":\"alpha\"}");
    g_assert_nonnull(between);
    g_assert_cmpuint(json_array_get_length(
                         json_object_get_array_member(payload_of(between),
                                                      "messages")), ==, 2);

    /*
     * And the user's own chat with beta has never had anything in it.
     * This is the assertion the bug would have failed.
     */
    with_user = request(&fixture, "room.history",
                        "{\"room\":\"beta\",\"as\":\"user\"}");
    g_assert_nonnull(with_user);
    g_assert_cmpuint(json_array_get_length(
                         json_object_get_array_member(payload_of(with_user),
                                                      "messages")), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * Every routed message publishes one event, and it names the room.
 *
 * One, because it used to be published by the link handler as well as
 * here; and the room, because that is the only thing a client can
 * honestly filter a transcript on.
 */
static void
test_a_routed_message_names_its_room(void)
{
    Fixture fixture = { 0 };
    MessageWatch watch = { 0 };
    g_autoptr(JsonNode) sent = NULL;
    g_autoptr(JsonNode) history = NULL;
    const gchar *room;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_bus_event), &watch);

    sent = request(&fixture, "msg.send",
                   "{\"from\":\"alpha\",\"target\":\"beta\","
                   "\"body\":\"ping\"}");
    g_assert_nonnull(sent);

    g_assert_cmpuint(watch.count, ==, 1);
    g_assert_cmpstr(watch.from, ==, "alpha");
    g_assert_cmpstr(watch.to, ==, "beta");

    /* The subject is the room the daemon resolved, not either agent. */
    history = request(&fixture, "room.history",
                      "{\"room\":\"beta\",\"as\":\"alpha\"}");
    room = json_object_get_string_member(payload_of(history), "room");

    g_assert_nonnull(room);
    g_assert_cmpstr(watch.subject, ==, room);
    g_assert_cmpstr(watch.subject, !=, "beta");

    g_free(watch.subject);
    g_free(watch.from);
    g_free(watch.to);
    fixture_teardown(&fixture);
}

/*
 * room.list carries enough to draw a conversation list without pulling
 * every transcript to find out which rooms have anything in them.
 */
static void
test_room_listing_shows_activity(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) sent = NULL;
    g_autoptr(JsonNode) listed = NULL;
    JsonArray *rooms;
    gboolean found = FALSE;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    sent = request(&fixture, "msg.send",
                   "{\"from\":\"alpha\",\"target\":\"beta\","
                   "\"body\":\"look into the flaky test\"}");
    g_assert_nonnull(sent);

    listed = request(&fixture, "room.list", NULL);
    rooms = json_object_get_array_member(payload_of(listed), "rooms");

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);

        if (json_object_get_int_member(room, "messages") == 0)
            continue;

        g_assert_cmpstr(json_object_get_string_member(room, "last_sender"),
                        ==, "alpha");
        g_assert_cmpstr(json_object_get_string_member(room, "last_body"),
                        ==, "look into the flaky test");
        g_assert_cmpint(json_object_get_int_member(room, "last_ts"), >, 0);
        found = TRUE;
    }

    g_assert_true(found);

    fixture_teardown(&fixture);
}

/*
 * A conversation between two agents is still there after a restart.
 *
 * Direct rooms are made on demand, so nothing in the config names them
 * and nothing re-created them at start -- every conversation the fleet
 * had ever had was missing from a listing until the same two agents
 * happened to speak again. The transcripts were on disk the whole time.
 */
static void
test_direct_rooms_come_back_after_a_restart(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) sent = NULL;
    g_autoptr(JsonNode) listed = NULL;
    JsonArray *rooms;
    gboolean found = FALSE;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    sent = request(&fixture, "msg.send",
                   "{\"from\":\"alpha\",\"target\":\"beta\","
                   "\"body\":\"the nightly build is flaky again\"}");
    g_assert_nonnull(sent);

    /* Stop, and come up again on the same state directory. */
    clawt_daemon_stop(fixture.daemon);
    g_clear_object(&fixture.daemon);

    while (g_main_context_iteration(fixture.context, FALSE))
        ;

    fixture.daemon = clawt_daemon_new(fixture.config_path, fixture.context);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    listed = request(&fixture, "room.list", NULL);
    rooms = json_object_get_array_member(payload_of(listed), "rooms");

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);

        if (json_object_get_int_member(room, "messages") == 0)
            continue;

        g_assert_cmpstr(json_object_get_string_member(room, "last_body"),
                        ==, "the nightly build is flaky again");
        found = TRUE;
    }

    g_assert_true(found);

    fixture_teardown(&fixture);
}

/*
 * Two agents replying to each other are stopped by max_hops.
 *
 * The hop counter existed, the router recorded it on every delivery,
 * and the orchestration tools read it back -- but an agent's ordinary
 * reply arrives over its link, and that path set the depth to a flat 1.
 * So every reply looked like the start of a fresh conversation and the
 * one limit built for this could never be reached: two agents traded
 * fifty messages of "Idle." and nothing stopped them.
 */
static void
test_a_reply_counts_as_a_hop(void)
{
    Fixture fixture = { 0 };
    ClawtMailboxRouter *router;
    ClawtAgentManager *agents;
    guint i;
    gint depth = 0;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 4\n"
                  "  cycle_window: 0\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    router = clawt_daemon_get_router(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);

    /*
     * Each pass is one agent answering the other, at the depth the
     * router last recorded for it -- which is what the link handler
     * does.
     */
    for (i = 0; i < 8; i++) {
        const gchar *from = (i % 2 == 0) ? "alpha" : "beta";
        const gchar *to = (i % 2 == 0) ? "beta" : "alpha";
        ClawtAgent *sender = clawt_agent_manager_get(agents, from);
        g_autoptr(GError) error = NULL;
        gint sent;

        depth = clawt_agent_get_hop_depth(sender) + 1;
        sent = clawt_mailbox_router_send_to(router, from, to, "Idle.", NULL,
                                            depth, &error);

        if (sent < 0) {
            g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
            break;
        }

        /*
         * Standing in for delivery, which is where the router records
         * how far a message had come. Nothing is running in this test,
         * so the drain has nobody to hand it to.
         */
        clawt_agent_set_hop_depth(clawt_agent_manager_get(agents, to), depth);
    }

    /* It stopped, and it stopped at the configured limit rather than
     * running to the end of the loop. */
    g_assert_cmpint(depth, ==, 4);
    g_assert_cmpuint(i, <, 8);

    fixture_teardown(&fixture);
}

/*
 * A finished turn leaves no depth behind for the next one.
 *
 * hop_depth answers "how far had the message I am handling already
 * come", which is a property of a turn and not of an agent. It was
 * stored on the agent and never cleared, so it survived the turn it
 * described: the next unrelated turn started from wherever the last
 * chain had reached, and an agent nine messages into a daemon's life
 * could no longer start a delegation at all.
 *
 * The router sets it on delivery and the turn ends at the typing
 * indicator, so that edge is where it goes back to zero.
 */
static void
test_a_finished_turn_clears_the_depth(void)
{
    Fixture fixture = { 0 };
    ClawtAgentManager *agents;
    ClawtAgent *alpha;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    agents = clawt_daemon_get_agents(fixture.daemon);
    alpha = clawt_agent_manager_get(agents, "alpha");
    g_assert_nonnull(alpha);

    /* Seven hops in, mid-turn: the depth is real and must be kept. */
    clawt_agent_set_hop_depth(alpha, 7);
    clawt_agent_set_activity(alpha, TRUE, "beta");
    g_assert_cmpint(clawt_agent_get_hop_depth(alpha), ==, 7);

    /*
     * The turn ends -- and the depth must still be here.
     *
     * libreclaw drops the typing indicator before it posts the answer,
     * so this transition happens in the window before the reply exists.
     * Clearing on it, which is how this was first written, stamped every
     * reply from zero and made max_hops unreachable on the one path it
     * exists for.
     */
    clawt_agent_set_activity(alpha, FALSE, NULL);
    g_assert_cmpint(clawt_agent_get_hop_depth(alpha), ==, 7);

    /* The reply is what carries it away: stamped one further, then gone. */
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "message", "alpha", "beta", "Done.", NULL);
    g_assert_cmpint(clawt_agent_get_hop_depth(alpha), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A turn that did not come through the router starts a fresh chain.
 *
 * Matrix, webhook, local and cmacs belong to libreclaw; the daemon never
 * sees those messages, so nothing records a depth for them. With the
 * depth left over from the last agent-to-agent delivery, an agent
 * answering a person in Matrix stamped its reply from that stale value
 * and was refused for a delegation one hop deep.
 *
 * There is only one setter, so "did not pass through the router" and
 * "hop_depth is whatever it was" are the same statement -- which is why
 * this is reproduced by ending a turn rather than by driving Matrix.
 */
static void
test_a_channel_turn_starts_from_zero(void)
{
    Fixture fixture = { 0 };
    ClawtMailboxRouter *router;
    ClawtAgentManager *agents;
    ClawtAgent *alpha;
    g_autoptr(GError) error = NULL;
    gint depth;
    gint sent;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 4\n"
                  "  cycle_window: 0\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    router = clawt_daemon_get_router(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);
    alpha = clawt_agent_manager_get(agents, "alpha");
    g_assert_nonnull(alpha);

    /* A deep chain reached alpha and alpha answered it, and the answer
     * is what takes the depth with it. */
    clawt_agent_set_hop_depth(alpha, 4);
    clawt_agent_set_activity(alpha, TRUE, "beta");
    clawt_agent_set_activity(alpha, FALSE, NULL);
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "message", "alpha", "beta", "Done.", NULL);

    /*
     * Now a person says something in Matrix. Nothing in that path
     * touches the router, so the only depth available is whatever the
     * agent still carries -- and the first hop of a new conversation
     * must be allowed.
     */
    depth = clawt_agent_get_hop_depth(alpha) + 1;
    g_assert_cmpint(depth, ==, 1);

    sent = clawt_mailbox_router_send_to(router, "alpha", "beta", "Can you "
                                        "take this?", NULL, depth, &error);
    g_assert_no_error(error);
    g_assert_cmpint(sent, >, 0);

    fixture_teardown(&fixture);
}

/*
 * Clearing the depth at the end of a turn does not soften the limit.
 *
 * This is the case the depth exists for -- two agents answering each
 * other for ever -- run the way it actually happens, with a turn
 * boundary between every hop. The chain still climbs, because its depth
 * travels in the mailbox item and the router stamps it onto whoever it
 * hands the item to; only the carry-over between unrelated turns is
 * gone.
 *
 * Without this, the previous fix could be undone by a patch that made
 * max_hops unreachable again, and every other test here would still
 * pass.
 */
static void
test_the_limit_still_fires_across_turns(void)
{
    Fixture fixture = { 0 };
    ClawtMailboxRouter *router;
    ClawtAgentManager *agents;
    guint i;
    gint depth = 0;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 4\n"
                  "  cycle_window: 0\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    router = clawt_daemon_get_router(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);

    for (i = 0; i < 8; i++) {
        const gchar *from = (i % 2 == 0) ? "alpha" : "beta";
        const gchar *to = (i % 2 == 0) ? "beta" : "alpha";
        ClawtAgent *sender = clawt_agent_manager_get(agents, from);
        ClawtAgent *receiver = clawt_agent_manager_get(agents, to);
        g_autoptr(GError) error = NULL;
        gint sent;

        depth = clawt_agent_get_hop_depth(sender) + 1;
        sent = clawt_mailbox_router_send_to(router, from, to, "Idle.", NULL,
                                            depth, &error);

        if (sent < 0) {
            g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
            break;
        }

        /*
         * Delivery, then the whole turn: the receiver is told how far
         * the item came, answers, and goes idle. The sender's own turn
         * ended when it sent, so its depth is cleared too -- and the
         * chain still climbs, because the next depth comes from the
         * item the router just delivered.
         */
        clawt_agent_set_activity(sender, FALSE, NULL);
        clawt_agent_set_hop_depth(receiver, depth);
        clawt_agent_set_activity(receiver, TRUE, from);
    }

    g_assert_cmpint(depth, ==, 4);
    g_assert_cmpuint(i, <, 8);

    fixture_teardown(&fixture);
}

/*
 * An attachment's name is rebuilt, not trusted.
 *
 * It comes from a filename somebody dragged in or a clipboard
 * suggestion, and "../../.ssh/authorized_keys" is a filename. The
 * basename is taken and the exchange resolves the rest, so a traversal
 * lands in the agent's own directory under a harmless name rather than
 * anywhere it was aimed.
 */
static void
test_an_attachment_cannot_escape_its_directory(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *payload = NULL;
    g_autofree gchar *encoded = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *host_path;

    /*
     * Its own exchange directory. Without one the daemon falls back to
     * the real ~/.local/share, and a test that writes into the
     * developer's home is a test that has already failed.
     */
    fixture_setup(&fixture,
                  /*
                   * Indented, continuing the `defaults:` the
                   * fixture opened.  A second top-level defaults
                   * would not merge -- YAML keeps the last and
                   * silently discards the first, taking
                   * workspace_root with it.
                   */
                  "  exchange_dir: \"/tmp/clawt-test-exchange\"\n"
                  "agents:\n  - id: alpha\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    encoded = g_base64_encode((const guchar *)"hello", 5);
    payload = g_strdup_printf(
        "{\"agent\":\"alpha\",\"name\":\"../../escape.txt\",\"data\":\"%s\"}",
        encoded);

    reply = request(&fixture, "attachment.put", payload);
    g_assert_nonnull(reply);

    g_assert_cmpstr(json_object_get_string_member(payload_of(reply), "name"),
                    ==, "escape.txt");

    host_path = json_object_get_string_member(payload_of(reply), "host_path");
    g_assert_nonnull(strstr(host_path, "alpha"));
    g_assert_null(strstr(host_path, ".."));

    /* And a name that is nothing but separators is refused outright. */
    {
        g_autofree gchar *bad = g_strdup_printf(
            "{\"agent\":\"alpha\",\"name\":\"../..\",\"data\":\"%s\"}", encoded);
        g_autoptr(JsonNode) refused = request(&fixture, "attachment.put", bad);

        g_assert_false(json_object_get_boolean_member(
            json_node_get_object(refused), "ok"));
    }

    clawt_test_remove_tree("/tmp/clawt-test-exchange");
    fixture_teardown(&fixture);
}

/*
 * An agent's overlay is built on its base image and records the path, so
 * deleting that image breaks the VM the next time it starts -- with an
 * error from qemu about a missing backing file, a long way from the
 * button that caused it.  The refusal names the agents so the person can
 * see what they were about to break.
 */
/*
 * A VM agent with no disk is refused at creation.
 *
 * Provisioning refuses it too, but that is a start and a daemon restart
 * away from the empty field that caused it -- far enough that what gets
 * reported is "the VM was never created", with nothing connecting the
 * two. This is the same rule, moved to where the mistake is made.
 */
static void
test_creating_a_vm_agent_without_a_disk_is_refused(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    {
        g_autoptr(JsonNode) created =
            request(&fixture, "agent.create",
                    "{\"id\":\"diskless\",\"computer\":\"vm\"}");

        g_assert_true(clawt_ipc_frame_is_error(created));
    }

    /*
     * And nothing half-made is left behind.  The check runs after the
     * fields are applied, so the agent briefly exists; a refusal that
     * kept it would leave a broken agent for somebody to find and delete.
     */
    g_assert_null(clawt_config_get_agent(
        clawt_daemon_get_config(fixture.daemon), "diskless"));

    /* A computer that needs no disk is unaffected. */
    {
        g_autoptr(JsonNode) created =
            request(&fixture, "agent.create",
                    "{\"id\":\"chatty\",\"computer\":\"none\"}");

        g_assert_false(clawt_ipc_frame_is_error(created));
    }

    fixture_teardown(&fixture);
}

static void
test_removing_an_image_in_use_is_refused(void)
{
    Fixture fixture;
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) forced = NULL;
    g_autofree gchar *image_dir = NULL;
    g_autofree gchar *image_path = NULL;
    g_autofree gchar *payload = NULL;
    ClawtAgentConfig *config;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    image_dir = g_build_filename(g_get_user_data_dir(), "clawtilla", "images",
                                 NULL);
    g_assert_true(clawt_ensure_dir(image_dir, 0700, NULL));

    image_path = g_build_filename(image_dir, "pretend.qcow2", NULL);
    g_assert_true(g_file_set_contents(image_path, "not a disk", -1, NULL));

    {
        /*
         * The disk is named at creation, because a VM agent cannot be
         * created without one -- an agent with no disk boots nothing, and
         * finding that out at start time is a long way from the dialog
         * where the field was left empty.
         */
        g_autofree gchar *create_payload =
            g_strdup_printf("{\"id\":\"vmagent\",\"computer\":\"vm\","
                            "\"vm_image\":\"%s\"}", image_path);
        g_autoptr(JsonNode) created =
            request(&fixture, "agent.create", create_payload);

        g_assert_false(clawt_ipc_frame_is_error(created));
    }

    config = clawt_config_get_agent(clawt_daemon_get_config(fixture.daemon),
                                    "vmagent");
    g_assert_nonnull(config);
    g_assert_true(clawt_daemon_reload(fixture.daemon, NULL));

    refused = request(&fixture, "image.vm_remove",
                      "{\"name\":\"pretend.qcow2\"}");
    g_assert_true(clawt_ipc_frame_is_error(refused));
    g_assert_true(g_file_test(image_path, G_FILE_TEST_EXISTS));

    /* force is the way past it, for somebody who means it. */
    payload = g_strdup("{\"name\":\"pretend.qcow2\",\"force\":true}");
    forced = request(&fixture, "image.vm_remove", payload);
    g_assert_false(clawt_ipc_frame_is_error(forced));
    g_assert_false(g_file_test(image_path, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

/*
 * An argv sent as an array survives exactly as sent.
 *
 * It used to be joined into a line by the client and split again here,
 * so every layer of quoting was consumed twice: `echo 'x\ny'` came back
 * as `xny` because the backslash the user had quoted was eaten as an
 * escape, and `sh -c 'echo a; echo b'` became four arguments and ran
 * nothing at all.
 */
static void
test_exec_argv_survives_the_wire(void)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("computer.exec", "t1");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_auto(GStrv) argv = NULL;
    JsonObject *payload;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "argv");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "sh");
    json_builder_add_string_value(builder, "-c");
    json_builder_add_string_value(builder, "echo a; echo b");
    json_builder_add_string_value(builder, "x\\ny");
    json_builder_add_string_value(builder, "one  two");
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));
    payload = clawt_ipc_frame_get_payload(frame);

    argv = clawt_ipc_payload_strv(payload, "argv");

    g_assert_nonnull(argv);
    g_assert_cmpuint(g_strv_length(argv), ==, 5);
    g_assert_cmpstr(argv[2], ==, "echo a; echo b");
    g_assert_cmpstr(argv[3], ==, "x\\ny");
    g_assert_cmpstr(argv[4], ==, "one  two");

    /*
     * And the old route really did destroy them, which is why the array
     * exists rather than more careful quoting on the way in.
     */
    {
        g_autofree gchar *joined = g_strjoinv(" ", argv);
        g_auto(GStrv) reparsed = NULL;

        g_assert_true(g_shell_parse_argv(joined, NULL, &reparsed, NULL));
        g_assert_cmpstr(reparsed[3], !=, "x\\ny");
    }
}

/* A missing or empty argv is not an argv; the command line still works. */
static void
test_exec_without_an_argv_falls_back(void)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("computer.exec", "t1");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_auto(GStrv) argv = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "command");
    json_builder_add_string_value(builder, "uname -a");
    json_builder_end_object(builder);

    clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));

    argv = clawt_ipc_payload_strv(clawt_ipc_frame_get_payload(frame), "argv");
    g_assert_null(argv);
}


/*
 * Rebuilding destroys the machine an agent is working on, so the two
 * cases that must never reach a teardown are refused before anything is
 * built: an agent with no computer, and one that is running.
 *
 * The first matters because "rebuilt" would otherwise be reported for an
 * agent that has nothing to rebuild -- a success that did nothing, which
 * is the worst answer of the three.
 */
static void
test_rebuild_refuses_what_it_must(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) no_agent = NULL;
    g_autoptr(JsonNode) no_computer = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    no_agent = request(&fixture, "computer.rebuild",
                       "{\"agent\":\"nobody\"}");
    g_assert_true(clawt_ipc_frame_is_error(no_agent));

    /*
     * chief has no computer configured, so the default is `none`.
     * Refused, and the message says so rather than reporting a rebuild.
     */
    no_computer = request(&fixture, "computer.rebuild",
                          "{\"agent\":\"chief\"}");
    g_assert_true(clawt_ipc_frame_is_error(no_computer));

    fixture_teardown(&fixture);
}



/*
 * Creating an agent starts it.
 *
 * A computer is built at *start*, never at create -- so a VM agent
 * created and left alone was a config file and no machine: no overlay,
 * no seed, no domain. `defaults.autostart` does not cover it either. It
 * is false by default and means "comes back with the daemon", which is a
 * different question from whether the thing somebody just asked for
 * exists yet.
 *
 * The CLI knew, and printed "Start it with: ..." as its third line. The
 * GTK client said "Agent created." and stopped, so a person there was
 * finished and had nothing.
 */
static void
test_creating_an_agent_starts_it(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) created = NULL;
    JsonObject *payload;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    created = request(&fixture, "agent.create", "{\"id\":\"scribe\"}");
    g_assert_false(clawt_ipc_frame_is_error(created));

    payload = clawt_ipc_frame_get_payload(created);
    g_assert_nonnull(payload);

    /*
     * The field, not the outcome: whether a libreclaw actually spawns
     * depends on a binary this suite does not ship. What matters here is
     * that the daemon tried and said so, because saying nothing is what
     * left a person believing they were done.
     */
    g_assert_true(json_object_has_member(payload, "started"));

    fixture_teardown(&fixture);
}

/* ...unless the caller says not to. */
static void
test_creating_an_agent_can_leave_it_stopped(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) created = NULL;
    JsonObject *payload;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    created = request(&fixture, "agent.create",
                      "{\"id\":\"scribe\",\"start\":false}");
    g_assert_false(clawt_ipc_frame_is_error(created));

    payload = clawt_ipc_frame_get_payload(created);
    g_assert_nonnull(payload);

    /* Nothing was attempted, so there is nothing to report. */
    g_assert_false(json_object_has_member(payload, "started"));
    g_assert_false(json_object_has_member(payload, "start_error"));

    fixture_teardown(&fixture);
}


/*
 * Changing a setting rewrites the files derived from it.
 *
 * agent.set used to save clawtilla.yaml and stop, so nothing the agent
 * reads was touched. tools.manage_fleet made that visible: the gate
 * answers from the live config and was right at once, while TOOLS.org
 * went on listing the tools as they stood at the last daemon start. Two
 * answers to "what do I have", and the file is the one that reaches the
 * agent's prompt -- so a chief-of-staff granted the tool went on saying
 * it had no such tool.
 */
static void
test_setting_a_key_rewrites_what_it_affects(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) granted = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *before = NULL;
    g_autofree gchar *after = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    chief_of_staff: true\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    path = g_build_filename(fixture.dir, "agents", "chief", "TOOLS.org",
                            NULL);
    g_assert_true(g_file_get_contents(path, &before, NULL, NULL));
    g_assert_null(strstr(before, "clawtilla_create_agent"));

    granted = request(&fixture, "agent.set",
                      "{\"agent\":\"chief\",\"key\":\"tools.manage_fleet\","
                      "\"value\":\"true\"}");
    g_assert_false(clawt_ipc_frame_is_error(granted));

    g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
    g_assert_nonnull(strstr(after, "clawtilla_create_agent"));

    fixture_teardown(&fixture);
}


/*
 * A frame's payload as a node, which is the shape a client sees.
 *
 * clawt_client_request() hands back the payload as a #JsonNode, so the
 * library reader takes one; a test holding a whole frame has the payload
 * as a #JsonObject and has to wrap it.
 */
static JsonNode *
payload_node(JsonNode *frame)
{
    JsonObject *payload = clawt_ipc_frame_get_payload(frame);
    JsonNode *node;

    if (payload == NULL)
        return NULL;

    node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, payload);

    return node;
}

static void
count_broken_warnings(const gchar *domain, GLogLevelFlags level,
                      const gchar *message, gpointer user_data)
{
    guint *seen = user_data;

    (void)domain;
    (void)level;

    if (message != NULL && strstr(message, "broken") != NULL)
        (*seen)++;
}

/*
 * Every agent says which room the operator's conversation with it is.
 *
 * A client needs it to tell a message meant for the person from the
 * fleet's own peer traffic, and it needs it for agents it has *never
 * opened* -- which is exactly the agent an unread count exists for.  It
 * is reported rather than derived because how a direct room is named is
 * the daemon's business: the GTK client already carries a comment saying
 * a client that takes "dm:a:b" apart is one that breaks when that
 * changes.
 *
 * Asserted against clawt_room_manager_direct_id() rather than against a
 * literal, so this is a test that the two agree rather than a second
 * copy of the format to fall out of step with the first.
 */
static void
test_agents_report_their_direct_room(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listed = NULL;
    g_autofree gchar *expected = NULL;
    JsonArray *agents;
    JsonObject *agent;

    fixture_setup(&fixture, "agents:\n  - id: solo\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    listed = request(&fixture, "agent.list", "{}");
    g_assert_false(clawt_ipc_frame_is_error(listed));

    agents = json_object_get_array_member(clawt_ipc_frame_get_payload(listed),
                                          "agents");
    g_assert_cmpuint(json_array_get_length(agents), ==, 1);

    agent = json_array_get_object_element(agents, 0);
    expected = clawt_room_manager_direct_id("user", "solo");

    g_assert_cmpstr(clawt_ipc_payload_string(agent, "dm_room"), ==, expected);

    /*
     * And it is the room a history request resolves to, which is the
     * whole point: a client matching an arriving message against this
     * has to be matching against the same room the daemon routes into.
     */
    {
        g_autoptr(JsonNode) history = request(&fixture, "room.history",
                                              "{\"room\":\"solo\","
                                              "\"as\":\"user\"}");

        g_assert_false(clawt_ipc_frame_is_error(history));
        g_assert_cmpstr(clawt_ipc_payload_string(
                            clawt_ipc_frame_get_payload(history), "room"),
                        ==, expected);
    }

    fixture_teardown(&fixture);
}


/*
 * A handler that re-renders the fleet says which agents it could not.
 *
 * clawtilla refuses a `libreclaw:` passthrough that redeclares a section
 * it renders itself -- YAML keeps the last of two identical top-level
 * keys, so clawtilla's own `channels:` block would be discarded.  The
 * refusal is right; what was missing is that it reached nobody.
 * `control.reload` was taught to report it and six other handlers were
 * not, so `agent.set` wrote the key to clawtilla.yaml, answered
 * `{"agent": ...}`, and left the agent running on the config.yaml it
 * already had -- which is the exact state the render exists to prevent.
 *
 * Two agents on purpose.  One refusal must not stop the other's files
 * being written, and a test with one agent cannot tell the two apart.
 */
static void
test_a_refused_render_is_reported(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) set = NULL;
    g_autoptr(JsonNode) clean = NULL;
    JsonObject *payload;
    JsonArray *refused;
    g_autofree gchar *text = NULL;
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *path = NULL;
    guint counted = 0;
    guint warned = 0;
    GLogLevelFlags was_fatal;
    guint handler;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: healthy\n"
                  "  - id: broken\n"
                  "    libreclaw:\n"
                  "      channels:\n"
                  "        webhook:\n"
                  "          enabled: true\n");

    /*
     * A counting handler rather than g_test_expect_message().  The
     * expectation queue matches the *next* message in the domain, and a
     * daemon start emits ordinary informational ones too -- so the
     * assertion becomes about the order clawtilla happens to log in,
     * which is not what this test is about and breaks the day somebody
     * adds a line.  Counting the warnings that name the refused agent
     * says the same thing and does not care when they arrive.
     */
    was_fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK);
    handler = g_log_set_handler("Clawtilla",
                                G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL |
                                G_LOG_FLAG_RECURSION,
                                count_broken_warnings, &warned);

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    /* Once for the agent, once for the summary naming the fleet. */
    g_assert_cmpuint(warned, >=, 2);

    set = request(&fixture, "agent.set",
                  "{\"agent\":\"healthy\",\"key\":\"model.model\","
                  "\"value\":\"a-particular-model\"}");

    g_assert_false(clawt_ipc_frame_is_error(set));

    payload = clawt_ipc_frame_get_payload(set);
    g_assert_nonnull(payload);

    /* The call itself succeeded -- that half was never in doubt. */
    g_assert_cmpstr(clawt_ipc_payload_string(payload, "agent"), ==,
                    "healthy");

    g_assert_true(json_object_has_member(payload, "refused"));
    refused = json_object_get_array_member(payload, "refused");
    g_assert_cmpuint(json_array_get_length(refused), ==, 1);
    g_assert_cmpstr(clawt_ipc_payload_string(
                        json_array_get_object_element(refused, 0), "agent"),
                    ==, "broken");

    /*
     * And the agent that was fine had its files written, so a refusal
     * for one is not a refusal for the fleet.
     */
    path = g_build_filename(fixture.dir, "state", "agents", "healthy",
                            "config.yaml", NULL);
    g_assert_true(g_file_get_contents(path, &rendered, NULL, NULL));
    g_assert_nonnull(strstr(rendered, "a-particular-model"));

    /* One sentence, from the library, so every client says the same. */
    {
        g_autoptr(JsonNode) node = payload_node(set);

        text = clawt_ipc_reply_refusal_text(node, &counted);
    }
    g_assert_cmpuint(counted, ==, 1);
    g_assert_nonnull(text);
    g_assert_nonnull(strstr(text, "broken"));
    g_assert_nonnull(strstr(text, "still running"));

    /*
     * A fleet with nothing wrong reports an empty array rather than no
     * array: a client has to be able to tell "nothing was refused" from
     * "this daemon does not report refusals".
     */
    clean = request(&fixture, "team.list", "{}");
    g_assert_false(clawt_ipc_frame_is_error(clean));
    {
        g_autoptr(JsonNode) node = payload_node(clean);

        g_assert_null(clawt_ipc_reply_refusal_text(node, &counted));
    }
    g_assert_cmpuint(counted, ==, 0);

    g_log_remove_handler("Clawtilla", handler);
    g_log_set_always_fatal(was_fatal);

    fixture_teardown(&fixture);
}


/*
 * Removing an agent can take everything it owns, and only when asked.
 *
 * Removing one from the fleet is reversible; deleting what it wrote is
 * not, so it is opt-in -- but a throwaway agent made to test something
 * should be throwable away, and a handful of abandoned workspaces is its
 * own kind of mess.
 */
static void
test_removing_an_agent_can_take_its_files(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) created = NULL;
    g_autoptr(JsonNode) removed = NULL;
    g_autofree gchar *workspace = NULL;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    created = request(&fixture, "agent.create",
                      "{\"id\":\"throwaway\",\"start\":false}");
    g_assert_false(clawt_ipc_frame_is_error(created));

    workspace = g_build_filename(fixture.dir, "agents", "throwaway", NULL);
    g_assert_true(g_file_test(workspace, G_FILE_TEST_IS_DIR));

    removed = request(&fixture, "agent.remove",
                      "{\"agent\":\"throwaway\",\"remove_files\":true}");
    g_assert_false(clawt_ipc_frame_is_error(removed));

    g_assert_cmpstr(clawt_ipc_payload_string(
                        clawt_ipc_frame_get_payload(removed), "files"),
                    ==, "removed");
    g_assert_false(g_file_test(workspace, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

/* ...and leaves them alone when it is not asked. */
static void
test_removing_an_agent_keeps_its_files_by_default(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) created = NULL;
    g_autoptr(JsonNode) removed = NULL;
    g_autofree gchar *workspace = NULL;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    created = request(&fixture, "agent.create",
                      "{\"id\":\"keeper\",\"start\":false}");
    g_assert_false(clawt_ipc_frame_is_error(created));

    workspace = g_build_filename(fixture.dir, "agents", "keeper", NULL);

    removed = request(&fixture, "agent.remove", "{\"agent\":\"keeper\"}");
    g_assert_false(clawt_ipc_frame_is_error(removed));

    g_assert_true(g_file_test(workspace, G_FILE_TEST_IS_DIR));

    fixture_teardown(&fixture);
}


/*
 * The guard, tested directly, because it is the thing standing between
 * "remove this agent's files" and a configured path that turned out to
 * be somebody's home directory. There is no undo on the other side.
 */
static void
test_removing_a_tree_refuses_to_leave_its_root(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-purge-XXXXXX", NULL);
    g_autofree gchar *outside = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
    g_autofree gchar *inside = g_build_filename(root, "agent", NULL);
    g_autofree gchar *file = g_build_filename(inside, "note", NULL);
    g_autofree gchar *escape = g_build_filename(root, "..", NULL);
    g_autoptr(GError) error = NULL;

    g_mkdir_with_parents(inside, 0700);
    g_file_set_contents(file, "x", -1, NULL);

    /* A path outside the root is refused, and nothing is touched. */
    g_assert_false(clawt_remove_tree(outside, root, &error));
    g_assert_nonnull(error);
    g_assert_true(g_file_test(outside, G_FILE_TEST_IS_DIR));
    g_clear_error(&error);

    /* Including one that only leaves it after canonicalisation. */
    g_assert_false(clawt_remove_tree(escape, root, &error));
    g_clear_error(&error);

    /* Inside, it does what it says. */
    g_assert_true(clawt_remove_tree(inside, root, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(inside, G_FILE_TEST_EXISTS));

    /* And a path that is already gone is success, not an error. */
    g_assert_true(clawt_remove_tree(inside, root, &error));
    g_assert_no_error(error);

    clawt_test_remove_tree(root);
    clawt_test_remove_tree(outside);
}


/*
 * Reordering the fleet, and the listing following it.
 *
 * The order is in clawtilla.yaml rather than in a client, because it is
 * about the agents rather than about reaching them -- so it is the same
 * in every client and on every machine, which is the difference between
 * this and a connection profile.
 */
static void
test_agents_can_be_reordered(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reordered = NULL;
    g_autoptr(JsonNode) listed = NULL;
    JsonArray *agents;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: alpha\n"
                  "  - id: beta\n"
                  "  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reordered = request(&fixture, "agent.reorder",
                        "{\"agents\":\"chief,alpha,beta\"}");
    g_assert_false(clawt_ipc_frame_is_error(reordered));

    listed = request(&fixture, "agent.list", "{}");
    agents = json_object_get_array_member(
        clawt_ipc_frame_get_payload(listed), "agents");

    g_assert_cmpuint(json_array_get_length(agents), ==, 3);
    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(agents, 0), "id"),
                    ==, "chief");
    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(agents, 2), "id"),
                    ==, "beta");

    fixture_teardown(&fixture);
}

/*
 * An id the client had and the daemon no longer does is skipped rather
 * than refused: the list comes from a view that may be a moment behind
 * a removal, and failing the whole reorder over that would lose the
 * arrangement somebody had just made.
 */
static void
test_reordering_survives_an_agent_that_has_gone(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reordered = NULL;
    g_autoptr(JsonNode) listed = NULL;
    JsonArray *agents;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reordered = request(&fixture, "agent.reorder",
                        "{\"agents\":\"beta,ghost,alpha\"}");
    g_assert_false(clawt_ipc_frame_is_error(reordered));

    listed = request(&fixture, "agent.list", "{}");
    agents = json_object_get_array_member(
        clawt_ipc_frame_get_payload(listed), "agents");

    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(agents, 0), "id"),
                    ==, "beta");

    fixture_teardown(&fixture);
}

/*
 * A fleet nobody has reordered comes back in the order the file has it.
 * The sort is stable and they all sit at the default, so the arrangement
 * somebody wrote by hand survives being listed.
 */
static void
test_an_unordered_fleet_keeps_its_file_order(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listed = NULL;
    JsonArray *agents;

    fixture_setup(&fixture,
                  "agents:\n  - id: zulu\n  - id: alpha\n  - id: mike\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    listed = request(&fixture, "agent.list", "{}");
    agents = json_object_get_array_member(
        clawt_ipc_frame_get_payload(listed), "agents");

    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(agents, 0), "id"),
                    ==, "zulu");
    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(agents, 2), "id"),
                    ==, "mike");

    fixture_teardown(&fixture);
}


/*
 * The listing is grouped by team, then ordered within each group.
 *
 * Grouped in the daemon so a client can put a header out whenever the
 * team changes rather than gathering the fleet itself -- two answers to
 * what order the fleet is in is one too many.
 *
 * Teamless first, because that is where the chief of staff lives:
 * putting it last would bury the agent somebody talks to most under
 * every team in the fleet.
 */
static void
test_the_listing_is_grouped_by_team(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listed = NULL;
    JsonArray *agents;
    const gchar *order[5];
    guint i;

    fixture_setup(&fixture,
                  "teams:\n"
                  "  - id: build\n    order: 20\n"
                  "  - id: research\n    order: 10\n"
                  "agents:\n"
                  "  - id: builder\n    team: build\n"
                  "  - id: reader\n    team: research\n"
                  "  - id: chief\n    chief_of_staff: true\n"
                  "  - id: writer\n    team: research\n    order: 5\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    listed = request(&fixture, "agent.list", "{}");
    agents = json_object_get_array_member(
        clawt_ipc_frame_get_payload(listed), "agents");

    g_assert_cmpuint(json_array_get_length(agents), ==, 4);

    for (i = 0; i < 4; i++)
        order[i] = json_object_get_string_member(
            json_array_get_object_element(agents, i), "id");

    /* Teamless first... */
    g_assert_cmpstr(order[0], ==, "chief");

    /* ...then research, because its order is lower than build's... */
    g_assert_cmpstr(order[1], ==, "reader");
    g_assert_cmpstr(order[2], ==, "writer");

    /* ...and build last. */
    g_assert_cmpstr(order[3], ==, "builder");

    fixture_teardown(&fixture);
}

/*
 * An agent on a team nobody declared still appears. Hiding it is how a
 * typo in agents.team survives being looked at.
 */
static void
test_an_agent_on_an_unknown_team_still_shows(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listed = NULL;
    JsonArray *agents;

    fixture_setup(&fixture,
                  "teams:\n  - id: research\n"
                  "agents:\n"
                  "  - id: one\n    team: research\n"
                  "  - id: two\n    team: reserch\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    listed = request(&fixture, "agent.list", "{}");
    agents = json_object_get_array_member(
        clawt_ipc_frame_get_payload(listed), "agents");

    g_assert_cmpuint(json_array_get_length(agents), ==, 2);

    /* Declared teams first, the mistyped one after. */
    g_assert_cmpstr(json_object_get_string_member(
                        json_array_get_object_element(agents, 1), "id"),
                    ==, "two");

    fixture_teardown(&fixture);
}

/* Each agent carries its team and standing, so a client can group and
 * an inspector can say what it may assign. */
static void
test_an_agent_reports_its_team(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listed = NULL;
    JsonObject *first;

    fixture_setup(&fixture,
                  "teams:\n  - id: research\n"
                  "agents:\n"
                  "  - id: boss\n    team: research\n    team_role: lead\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    listed = request(&fixture, "agent.list", "{}");
    first = json_array_get_object_element(
        json_object_get_array_member(clawt_ipc_frame_get_payload(listed),
                                     "agents"), 0);

    g_assert_cmpstr(json_object_get_string_member(first, "team"), ==,
                    "research");
    g_assert_cmpstr(json_object_get_string_member(first, "team_role"), ==,
                    "lead");

    fixture_teardown(&fixture);
}

/*
 * team.list counts the running agents itself. Three clients counting the
 * same thing is three chances to disagree about what "active" means.
 */
static void
test_team_list_counts_and_warns(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listed = NULL;
    JsonObject *payload;
    JsonObject *team;
    JsonArray *warnings;

    fixture_setup(&fixture,
                  "teams:\n  - id: research\n    name: Research\n"
                  "agents:\n"
                  "  - id: one\n    team: research\n    team_role: lead\n"
                  "  - id: two\n    team: research\n    team_role: lead\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    listed = request(&fixture, "team.list", "{}");
    payload = clawt_ipc_frame_get_payload(listed);
    team = json_array_get_object_element(
        json_object_get_array_member(payload, "teams"), 0);

    g_assert_cmpstr(json_object_get_string_member(team, "name"), ==,
                    "Research");
    g_assert_cmpint(json_object_get_int_member(team, "total"), ==, 2);

    /* Two leads is reported rather than picked between. */
    warnings = json_object_get_array_member(payload, "warnings");
    g_assert_cmpuint(json_array_get_length(warnings), ==, 1);
    g_assert_nonnull(strstr(json_array_get_string_element(warnings, 0),
                            "two leads"));

    fixture_teardown(&fixture);
}

/* A team's id is not editable, and removing one says who is left. */
static void
test_team_edits_are_guarded(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) renamed = NULL;
    g_autoptr(JsonNode) removed = NULL;

    fixture_setup(&fixture,
                  "teams:\n  - id: research\n"
                  "agents:\n  - id: one\n    team: research\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    renamed = request(&fixture, "team.set",
                      "{\"team\":\"research\",\"key\":\"id\","
                      "\"value\":\"other\"}");
    g_assert_true(clawt_ipc_frame_is_error(renamed));

    removed = request(&fixture, "team.remove", "{\"team\":\"research\"}");
    g_assert_false(clawt_ipc_frame_is_error(removed));
    g_assert_cmpint(json_object_get_int_member(
                        clawt_ipc_frame_get_payload(removed), "orphaned"),
                    ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A task ends when the turn ends, not when anything in its thread arrives.
 *
 * libreclaw posts more than the answer into a thread -- a progress note
 * every five minutes, a guardian refusal, a restart notice -- and each
 * of those used to complete the task the instant it arrived.  A routine
 * therefore reported `completed` seconds after starting, carrying
 * "Still working..." as its result, while the work ran for minutes
 * afterwards against a task nothing was waiting on.
 *
 * The typing indicator is what separates them: libreclaw raises it for
 * the whole turn and drops it in on_process_message_finish() before the
 * answer is posted, so anything arriving while the agent is still busy
 * is by construction not the answer.
 */
static void
test_a_progress_note_does_not_finish_a_task(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtTaskManager *tasks;
    ClawtTask *task;
    g_autoptr(GError) error = NULL;
    const gchar *task_id;

    fixture_setup(&fixture, "agents:\n  - id: worker\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    tasks = clawt_daemon_get_tasks(fixture.daemon);

    task = clawt_task_manager_create(tasks, "user", "worker",
                                     "write the morning brief", NULL, &error);
    g_assert_nonnull(task);
    task_id = clawt_task_get_id(task);
    g_assert_true(clawt_task_manager_start(tasks, task_id));

    /* The turn starts. */
    g_signal_emit_by_name(links, "typing", "worker", "dm:user:worker", TRUE);

    /*
     * Five minutes in, libreclaw says so -- in the room and in the
     * thread, which is the task id.
     */
    g_signal_emit_by_name(links, "message", "worker", "dm:user:worker",
                          "\xe2\x8f\xb3 Still working... (5m elapsed)",
                          task_id);

    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);
    g_assert_null(clawt_task_get_result(task));

    /* The turn ends, and then the answer is posted. */
    g_signal_emit_by_name(links, "typing", "worker", "dm:user:worker", FALSE);
    g_signal_emit_by_name(links, "message", "worker", "dm:user:worker",
                          "Brief written to notes/brief.org.", task_id);

    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
    g_assert_cmpstr(clawt_task_get_result(task), ==,
                    "Brief written to notes/brief.org.");

    fixture_teardown(&fixture);
}

/*
 * An agent that never raises the indicator still finishes its tasks.
 *
 * The indicator needs a room and is skipped without one, so busy stays
 * FALSE for such an agent throughout.  That has to complete as it always
 * did: a task that ends late is a delay, one that ends early is a lie,
 * and the fallback must fail towards the delay.
 */
static void
test_a_silent_agent_still_finishes_its_task(void)
{
    Fixture fixture = { 0 };
    ClawtTaskManager *tasks;
    ClawtTask *task;
    g_autoptr(GError) error = NULL;
    const gchar *task_id;

    fixture_setup(&fixture, "agents:\n  - id: worker\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    tasks = clawt_daemon_get_tasks(fixture.daemon);
    task = clawt_task_manager_create(tasks, "user", "worker", "count them",
                                     NULL, &error);
    g_assert_nonnull(task);
    task_id = clawt_task_get_id(task);
    g_assert_true(clawt_task_manager_start(tasks, task_id));

    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "message", "worker", "dm:user:worker", "Forty-two.",
                          task_id);

    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
    g_assert_cmpstr(clawt_task_get_result(task), ==, "Forty-two.");

    fixture_teardown(&fixture);
}


/*
 * A reply still counts the hops the message it answers had travelled.
 *
 * The router records how far a delivery had come on the agent, and
 * on_link_message() stamps the reply one further. libreclaw drops its
 * typing indicator *before* posting the answer, so anything that clears
 * per-turn state when the turn ends clears it in the window between the
 * two -- and the reply is then stamped as though it began a fresh
 * conversation. That is precisely the bug max_hops was unreachable
 * through once before: two agents traded fifty messages of "Idle." and
 * nothing stopped them.
 */
static void
test_a_reply_after_the_turn_ends_still_counts_hops(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgentManager *agents;
    ClawtAgent *alpha;
    ClawtAgent *beta;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 4\n"
                  "  cycle_window: 0\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);
    alpha = clawt_agent_manager_get(agents, "alpha");
    beta = clawt_agent_manager_get(agents, "beta");
    g_assert_nonnull(alpha);
    g_assert_nonnull(beta);

    /* A message reached alpha having already travelled to the limit. */
    clawt_agent_set_hop_depth(alpha, 4);

    /* Its turn runs, and ends -- the indicator drops before the answer. */
    g_signal_emit_by_name(links, "typing", "alpha", "beta", TRUE);
    g_signal_emit_by_name(links, "typing", "alpha", "beta", FALSE);

    /* Now the answer. One hop further than 4 is past max_hops. */
    g_signal_emit_by_name(links, "message", "alpha", "beta", "Idle.", NULL);

    /*
     * Refused, so nothing was queued for beta. A reply that arrives in
     * beta's mailbox here means the chain restarted at depth 1 and the
     * two of them can answer each other for ever.
     */
    g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(beta)),
                     ==, 0);

    fixture_teardown(&fixture);
}


int
main(int argc, char *argv[])
{
    g_autofree gchar *data_dir = g_dir_make_tmp("clawt-daemon-data-XXXXXX",
                                                NULL);
    gint status;

    /*
     * Before anything can ask for it: GLib caches the data directory on
     * first use, and the daemon keeps cloud images under it.  Left alone
     * a test would create and delete files in the real ~/.local/share.
     */
    g_setenv("XDG_DATA_HOME", data_dir, TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/daemon/starts", test_starts_with_an_empty_config);
    g_test_add_func("/daemon/reply-after-turn-counts-hops",
                    test_a_reply_after_the_turn_ends_still_counts_hops);
    g_test_add_func("/daemon/progress-note-is-not-an-answer",
                    test_a_progress_note_does_not_finish_a_task);
    g_test_add_func("/daemon/silent-agent-still-finishes",
                    test_a_silent_agent_still_finishes_its_task);
    g_test_add_func("/daemon/one-at-a-time", test_refuses_a_second_daemon);
    g_test_add_func("/daemon/listing-grouped-by-team",
                    test_the_listing_is_grouped_by_team);
    g_test_add_func("/daemon/unknown-team-still-shows",
                    test_an_agent_on_an_unknown_team_still_shows);
    g_test_add_func("/daemon/agent-reports-its-team",
                    test_an_agent_reports_its_team);
    g_test_add_func("/daemon/team-list-counts-and-warns",
                    test_team_list_counts_and_warns);
    g_test_add_func("/daemon/team-edits-guarded",
                    test_team_edits_are_guarded);
    g_test_add_func("/daemon/agents-can-be-reordered",
                    test_agents_can_be_reordered);
    g_test_add_func("/daemon/reorder-survives-a-missing-agent",
                    test_reordering_survives_an_agent_that_has_gone);
    g_test_add_func("/daemon/unordered-keeps-file-order",
                    test_an_unordered_fleet_keeps_its_file_order);
    g_test_add_func("/daemon/remove-tree-stays-in-its-root",
                    test_removing_a_tree_refuses_to_leave_its_root);
    g_test_add_func("/daemon/remove-can-take-the-files",
                    test_removing_an_agent_can_take_its_files);
    g_test_add_func("/daemon/remove-keeps-files-by-default",
                    test_removing_an_agent_keeps_its_files_by_default);
    g_test_add_func("/daemon/set-rewrites-derived-files",
                    test_setting_a_key_rewrites_what_it_affects);
    g_test_add_func("/daemon/refused-render-is-reported",
                    test_a_refused_render_is_reported);
    g_test_add_func("/daemon/agents-report-their-direct-room",
                    test_agents_report_their_direct_room);
    g_test_add_func("/daemon/create-starts-the-agent",
                    test_creating_an_agent_starts_it);
    g_test_add_func("/daemon/create-can-leave-it-stopped",
                    test_creating_an_agent_can_leave_it_stopped);
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

    g_test_add_func("/daemon/create-leaves-others-alone",
                    test_creating_an_agent_leaves_the_others_alone);
    g_test_add_func("/daemon/reload-reaches-the-fleet",
                    test_reload_reaches_the_fleet);
    g_test_add_func("/daemon/state-dir-cannot-be-mounted",
                    test_the_state_directory_cannot_be_mounted);
    g_test_add_func("/daemon/computer-rebuild-refusals",
                    test_rebuild_refuses_what_it_must);

    g_test_add_func("/daemon/request-from-event-handler",
                    test_a_request_from_an_event_handler_completes);
    g_test_add_func("/daemon/history-by-agent-id", test_history_by_agent_id);
    g_test_add_func("/daemon/tool-rpc-needs-a-token",
                    test_tool_rpc_needs_the_agents_token);
    g_test_add_func("/daemon/tool-rpc-runs-a-tool", test_tool_rpc_runs_a_tool);
    g_test_add_func("/daemon/string-booleans",
                    test_string_booleans_are_understood);
    g_test_add_func("/daemon/send-reports-target-state",
                    test_send_reports_the_target_state);

    g_test_add_func("/daemon/requests-after-subscribe",
                    test_requests_work_after_subscribing);
    g_test_add_func("/daemon/no-payload-while-disconnected",
                    test_a_request_with_no_payload_while_disconnected);
    g_test_add_func("/daemon/model-catalog", test_the_model_catalog_is_served);
    g_test_add_func("/daemon/create-records-provider",
                    test_creating_an_agent_records_its_provider);

    g_test_add_func("/daemon/client-vanishes-mid-read",
                    test_a_client_vanishing_mid_read_is_survivable);

    g_test_add_func("/daemon/stop-removes-sockets",
                    test_stop_removes_the_sockets);
    g_test_add_func("/daemon/broken-reload",
                    test_a_broken_reload_keeps_the_old_config);
    g_test_add_func("/daemon/reload/refused-render",
                    test_reload_reports_a_refused_render);

    g_test_add_func("/daemon/agents-talking-stays-between-them",
                    test_agents_talking_stays_out_of_the_users_chat);
    g_test_add_func("/daemon/message-event-names-its-room",
                    test_a_routed_message_names_its_room);
    g_test_add_func("/daemon/room-listing-shows-activity",
                    test_room_listing_shows_activity);
    g_test_add_func("/daemon/direct-rooms-survive-a-restart",
                    test_direct_rooms_come_back_after_a_restart);
    g_test_add_func("/daemon/a-reply-counts-as-a-hop",
                    test_a_reply_counts_as_a_hop);
    g_test_add_func("/daemon/hop-depth/limit-still-fires-across-turns",
                    test_the_limit_still_fires_across_turns);
    g_test_add_func("/daemon/hop-depth/cleared-when-a-turn-ends",
                    test_a_finished_turn_clears_the_depth);
    g_test_add_func("/daemon/hop-depth/channel-turn-starts-fresh",
                    test_a_channel_turn_starts_from_zero);
    g_test_add_func("/daemon/attachment-cannot-escape",
                    test_an_attachment_cannot_escape_its_directory);
    g_test_add_func("/daemon/vm-agent-needs-a-disk",
                    test_creating_a_vm_agent_without_a_disk_is_refused);
    g_test_add_func("/daemon/image-in-use",
                    test_removing_an_image_in_use_is_refused);
    g_test_add_func("/daemon/exec-argv",
                    test_exec_argv_survives_the_wire);
    g_test_add_func("/daemon/exec-argv-fallback",
                    test_exec_without_an_argv_falls_back);

    status = g_test_run();

    clawt_test_remove_tree(data_dir);

    return status;
}
