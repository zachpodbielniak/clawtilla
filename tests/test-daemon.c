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
#include <signal.h>
#include <utime.h>

#include "clawt-test-util.h"

/*
 * For clawt_daemon_turn_settle() and the daemon's own turn watch:
 * core/clawt-daemon-private.h *is* the interface of daemon-turn.c,
 * and the typing edge is exactly the wire between the two.
 */
#include "core/clawt-daemon-private.h"

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
         * And the pods, which are the fourth.  `automation_dir`
         * defaults to ~/.clawtilla/pods, so on a machine that runs a
         * real fleet every daemon fixture here loaded that person's own
         * automation and let it *act* on the test's agents -- which is
         * worse than the workspace case below, because a pod does
         * things rather than leaving files behind.
         */
        "  automation_dir: \"%s/pods\"\n"
        /*
         * And nowhere near the real fleet.  `workspace_root`
         * defaults to ~/.clawtilla/agents, so without this every
         * agent a test creates is scaffolded into the developer's
         * own agent directory -- indistinguishable afterwards from
         * one they meant to keep.  The socket and the state dir
         * were already pinned here; this is the third thing that
         * escapes a temporary directory if nobody says otherwise.
         */
        /*
         * And the skills, which are the fifth.  `skills.dir` defaults
         * to ~/.clawtilla/skills, so without this every daemon fixture
         * here scanned the developer's own library, watched it, and
         * linked whatever was in it into the agents these tests create
         * -- a green run whose result depends on what somebody happens
         * to have written on that machine.
         *
         * Before `defaults:` rather than after, because `defaults:` has
         * to stay the last block: several tests pass @extra_yaml that
         * continues it with another indented key, and a top-level
         * section wedged in between turns those into an unknown key
         * under `skills`.
         */
        "skills:\n  dir: \"%s/skills\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "%s",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->dir, extra_yaml != NULL ? extra_yaml : "");

    g_file_set_contents(fixture->config_path, yaml, -1, &error);
    g_assert_no_error(error);
}

/*
 * Drops a warning on the floor.
 *
 * Used where a test provokes one on purpose and cares only that the
 * operation still succeeded.  g_test_expect_message() matches the *next*
 * message in the domain, so it turns the assertion into one about the
 * order clawtilla happens to log in -- which is not what any of these
 * tests are about and breaks the day somebody adds a line.
 */
static void
swallow_warnings(const gchar *domain, GLogLevelFlags level,
                 const gchar *message, gpointer user_data)
{
    (void)domain;
    (void)level;
    (void)message;
    (void)user_data;
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
 * A second daemon on a *different socket* but the same state directory is
 * refused too.
 *
 * This is the case that cost a real conversation.  The socket was guarded
 * by a connect probe, which only ever answers "is anything listening
 * there" -- so two daemons with different socket paths and one state
 * directory both started happily.  They then kept a room manager each,
 * and save_room() rewrites the whole transcript from memory on every
 * message, so the last to flush won and four delivered messages were
 * deleted.
 *
 * The test above shares the whole config and so is caught by the socket
 * guard as well; it cannot tell whether the state lock works.  This one
 * changes the socket and nothing else, so only the lock can refuse it.
 */
static void
test_refuses_a_second_daemon_on_the_same_state_dir(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtDaemon) second = NULL;
    g_autofree gchar *second_yaml = NULL;
    g_autofree gchar *second_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GError) write_error = NULL;

    fixture_setup(&fixture, NULL);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    /*
     * Same state_dir, different socket -- and different ports, so that
     * nothing but the state directory can be the thing in common.
     */
    second_path = g_build_filename(fixture.dir, "second.yaml", NULL);
    second_yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/second.sock\"\n"
        "  automation_dir: \"%s/pods2\"\n"
        "defaults:\n  workspace_root: \"%s/agents2\"\n",
        fixture.dir, fixture.dir, fixture.dir, fixture.dir);

    g_assert_true(g_file_set_contents(second_path, second_yaml, -1,
                                      &write_error));
    g_assert_no_error(write_error);

    second = clawt_daemon_new(second_path, fixture.context);

    g_assert_false(clawt_daemon_start(second, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);

    /* The refusal names the directory, which is what somebody must change. */
    g_assert_nonnull(strstr(error->message, "state directory"));

    fixture_teardown(&fixture);
}

/*
 * Stopping releases the lock, so the same process can start again.
 *
 * An embedded host -- cmacs -- stops and starts the daemon in one
 * process, and a lock held past stop would make the second start refuse
 * against nobody but itself.
 */
static void
test_stopping_releases_the_state_lock(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));
    clawt_daemon_stop(fixture.daemon);

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

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

/*
 * A `channels:` passthrough is merged, not refused.
 *
 * libreclaw's webhook routing lives at `channels.webhook.endpoints` -- a
 * list of objects with nested targets, which has no sensible spelling in
 * a flat schema.  clawtilla refused any passthrough redeclaring
 * `channels:`, so the listener could be configured, scoped and
 * health-checked and could never receive anything.  The collision hazard
 * is per key, so that is where the check now is.
 */
static void
test_passthrough_channels_are_merged(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    const gchar *rendered;
    const gchar *webhook;
    const gchar *clawtilla;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    integrations:\n"
        "      webhook:\n"
        "        enabled: true\n"
        "        port: 9101\n"
        "    libreclaw:\n"
        "      channels:\n"
        "        webhook:\n"
        "          endpoints:\n"
        "            - name: deploy\n"
        "              path: /deploy\n"
        "              mode: prompt\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "config.render", "{\"agent\":\"chief\"}");
    rendered = json_object_get_string_member(payload_of(reply), "yaml");

    /* The routing arrived... */
    g_assert_nonnull(strstr(rendered, "endpoints:"));
    g_assert_nonnull(strstr(rendered, "deploy"));

    /* ...inside the webhook block clawtilla rendered, not beside it. */
    webhook = strstr(rendered, "  webhook:\n");
    g_assert_nonnull(webhook);
    g_assert_true(strstr(webhook, "endpoints:") != NULL);
    g_assert_nonnull(strstr(webhook, "listen_port:"));

    /*
     * And clawtilla's own channel is still there.  A second top-level
     * `channels:` would win outright and take this with it, which is the
     * collision the whole-section refusal existed to prevent -- so the
     * merged text must not also be copied through verbatim.
     */
    clawtilla = strstr(rendered, "  clawtilla:\n");
    g_assert_nonnull(clawtilla);

    {
        /* Exactly one top-level channels: in the whole document. */
        const gchar *scan = rendered;
        guint seen = 0;

        while ((scan = strstr(scan, "\nchannels:\n")) != NULL) {
            seen++;
            scan += 2;
        }

        g_assert_cmpuint(seen, ==, 1);
    }

    fixture_teardown(&fixture);
}

/*
 * ...and a key clawtilla writes itself is still refused, by name.
 *
 * That is the whole reason the section was refused wholesale: a
 * passthrough setting `listen_port` would silently take the one the
 * integration configured.  Refusing the key rather than the section is
 * the same protection at the resolution the hazard actually has.
 */
static void
test_a_colliding_channel_key_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    integrations:\n"
        "      webhook:\n"
        "        enabled: true\n"
        "        port: 9101\n"
        "    libreclaw:\n"
        "      channels:\n"
        "        webhook:\n"
        "          listen_port: 1234\n");

    /*
     * Start renders every agent and warns about the one it refused.  The
     * fatal mask is lowered rather than the messages expected, for the
     * reason /daemon/refused-render-is-reported gives: the expectation
     * queue matches the *next* message in the domain and a start emits
     * ordinary informational ones too.
     */
    {
        GLogLevelFlags was_fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK);
        guint handler = g_log_set_handler("Clawtilla",
                                          G_LOG_LEVEL_WARNING |
                                          G_LOG_FLAG_FATAL |
                                          G_LOG_FLAG_RECURSION,
                                          swallow_warnings, NULL);

        g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

        g_log_remove_handler("Clawtilla", handler);
        g_log_set_always_fatal(was_fatal);
    }

    reply = request(&fixture, "config.render", "{\"agent\":\"chief\"}");
    g_assert_true(clawt_ipc_frame_is_error(reply));

    fixture_teardown(&fixture);
}

/*
 * An isolated routine gets a room of its own, and therefore a session.
 *
 * A run is sent from `user` to the agent by default, so it lands in the
 * operator's room from the operator's sender -- and libreclaw keys a
 * session on channel, room and sender together.  One session, one queue:
 * Monday's run is in Tuesday's context and a 09:00 brief starts whenever
 * the agent next goes idle.  docs/routines.org promised the opposite for
 * a long time and it was never true.
 *
 * Both halves are asserted, because either alone is not isolation: a
 * distinct room reached from `user` would put every routine on that
 * agent in one session, and a distinct sender in the operator's room
 * would still be in their transcript.
 */
static void
test_an_isolated_routine_gets_its_own_room(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) ran = NULL;
    g_autoptr(JsonNode) rooms = NULL;
    JsonArray *listed;
    gboolean found = FALSE;
    guint i;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: worker\n"
        "routines:\n"
        "  - id: morning\n"
        "    agent: worker\n"
        "    instructions: \"say good morning\"\n"
        "    schedule: manual\n"
        "    isolate: true\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    ran = request(&fixture, "routine.run", "{\"id\":\"morning\"}");
    g_assert_false(clawt_ipc_frame_is_error(ran));

    rooms = request(&fixture, "room.list", "{}");
    listed = json_object_get_array_member(clawt_ipc_frame_get_payload(rooms),
                                          "rooms");

    for (i = 0; i < json_array_get_length(listed); i++) {
        JsonObject *room = json_array_get_object_element(listed, i);

        if (g_strcmp0(clawt_ipc_payload_string(room, "id"),
                      "routine:morning") != 0)
            continue;

        found = TRUE;

        /*
         * Its members are the agent and `routine`, not the operator --
         * the sender is half of what makes the session key differ.
         */
        {
            JsonArray *members = json_object_get_array_member(room,
                                                              "members");
            gboolean has_routine = FALSE;
            gboolean has_user = FALSE;
            guint m;

            for (m = 0; m < json_array_get_length(members); m++) {
                const gchar *who = json_array_get_string_element(members, m);

                if (g_strcmp0(who, "routine") == 0)
                    has_routine = TRUE;
                if (g_strcmp0(who, "user") == 0)
                    has_user = TRUE;
            }

            g_assert_true(has_routine);
            g_assert_false(has_user);
        }
    }

    g_assert_true(found);

    fixture_teardown(&fixture);
}

/* ...and a routine that did not ask still shares the operator's room. */
static void
test_an_ordinary_routine_stays_in_the_conversation(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) ran = NULL;
    g_autoptr(JsonNode) rooms = NULL;
    JsonArray *listed;
    guint i;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: worker\n"
        "routines:\n"
        "  - id: morning\n"
        "    agent: worker\n"
        "    instructions: \"say good morning\"\n"
        "    schedule: manual\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    ran = request(&fixture, "routine.run", "{\"id\":\"morning\"}");
    g_assert_false(clawt_ipc_frame_is_error(ran));

    rooms = request(&fixture, "room.list", "{}");
    listed = json_object_get_array_member(clawt_ipc_frame_get_payload(rooms),
                                          "rooms");

    for (i = 0; i < json_array_get_length(listed); i++) {
        JsonObject *room = json_array_get_object_element(listed, i);

        g_assert_cmpstr(clawt_ipc_payload_string(room, "id"), !=,
                        "routine:morning");
    }

    fixture_teardown(&fixture);
}

/*
 * The event log is readable, which it had never been.
 *
 * ClawtEventLog has written every published event to NDJSON since the
 * daemon was first built and swept on daemon.event_log_days, and nothing
 * read it back -- so diagnosing a message loop meant running sqlite3 and
 * grep on the host.  The alerts surface pages into it through this.
 */
static void
test_the_event_log_can_be_read_back(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) sent = NULL;
    g_autoptr(JsonNode) all = NULL;
    g_autoptr(JsonNode) scoped = NULL;
    JsonArray *events;
    gboolean saw_message = FALSE;
    guint i;

    fixture_setup(&fixture,
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    sent = request(&fixture, "msg.send",
                   "{\"target\":\"alpha\",\"body\":\"something\"}");
    g_assert_false(clawt_ipc_frame_is_error(sent));

    all = request(&fixture, "event.list", "{\"limit\":50}");
    g_assert_false(clawt_ipc_frame_is_error(all));

    events = json_object_get_array_member(clawt_ipc_frame_get_payload(all),
                                          "events");
    g_assert_cmpuint(json_array_get_length(events), >, 0);

    for (i = 0; i < json_array_get_length(events); i++) {
        JsonObject *event = json_array_get_object_element(events, i);

        /*
         * The same shape a subscriber receives, not a second spelling of
         * what an event is: kind, subject, ts and detail.
         */
        g_assert_nonnull(clawt_ipc_payload_string(event, "kind"));
        g_assert_true(json_object_has_member(event, "ts"));

        if (g_strcmp0(clawt_ipc_payload_string(event, "kind"),
                      "message") == 0)
            saw_message = TRUE;
    }

    g_assert_true(saw_message);

    /*
     * And a subject narrows it.  Fleet-wide is the default because the
     * case that sends somebody to the shell is watching several agents
     * at once, but "only this one" has to be one request away.
     */
    scoped = request(&fixture, "event.list",
                     "{\"subject\":\"beta\",\"limit\":50}");
    g_assert_false(clawt_ipc_frame_is_error(scoped));

    events = json_object_get_array_member(clawt_ipc_frame_get_payload(scoped),
                                          "events");

    for (i = 0; i < json_array_get_length(events); i++) {
        JsonObject *event = json_array_get_object_element(events, i);

        g_assert_cmpstr(clawt_ipc_payload_string(event, "subject"), ==,
                        "beta");
    }

    fixture_teardown(&fixture);
}

/*
 * A file an agent sent reaches a client as bytes, and an id from the
 * wire cannot name a file the daemon was never asked to serve.
 *
 * The path would work for a client on this host and show nothing for a
 * remote one, which reads as a broken image rather than an unsupported
 * setup -- so the bytes travel.  The id becomes a filename, which is the
 * one thing here worth being paranoid about.
 */
static void
test_an_attachment_is_served_as_bytes(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *dir = NULL;
    g_autofree gchar *stored = NULL;
    g_autoptr(JsonNode) got = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) missing = NULL;
    g_autofree gchar *decoded = NULL;
    JsonObject *payload;
    gsize length = 0;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    /*
     * Written where the daemon keeps them, with an embedded NUL: a byte
     * count is the thing that separates a copy from a string, and the
     * PNG signature has one at index 2.
     */
    dir = g_build_filename(fixture.dir, "state", "attachments", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);

    stored = g_build_filename(dir, "0abcdef-shot.png", NULL);
    g_assert_true(g_file_set_contents(stored, "\211PNG\0\r\n\032\n", 9,
                                      NULL));

    got = request(&fixture, "attachment.get",
                  "{\"id\":\"0abcdef-shot.png\"}");
    g_assert_false(clawt_ipc_frame_is_error(got));

    payload = clawt_ipc_frame_get_payload(got);
    g_assert_cmpstr(clawt_ipc_payload_string(payload, "name"), ==,
                    "shot.png");
    g_assert_cmpint(clawt_ipc_payload_int(payload, "bytes", 0), ==, 9);

    decoded = (gchar *)g_base64_decode(
        clawt_ipc_payload_string(payload, "base64"), &length);
    g_assert_cmpuint(length, ==, 9);
    g_assert_cmpint(memcmp(decoded, "\211PNG\0\r\n\032\n", 9), ==, 0);

    /* An id of somebody's choosing reaches nothing. */
    refused = request(&fixture, "attachment.get",
                      "{\"id\":\"../../config.yaml\"}");
    g_assert_true(clawt_ipc_frame_is_error(refused));

    missing = request(&fixture, "attachment.get", "{}");
    g_assert_true(clawt_ipc_frame_is_error(missing));

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

/*
 * The cycle window reaches the guard from the config, and expires.
 *
 * tests/test-orchestration.c covers ClawtLoopGuard on its own.  This is
 * the wire: orchestration.cycle_seconds -> clawt_config_get_int() ->
 * configure_limits() -> the guard the router actually consults, and the
 * message.refused the operator sees.  A limit that works in isolation
 * and is never handed its configured value is a shape this codebase has
 * shipped more than once -- max_hops was stamped flat, task_budget_usd
 * was never incremented -- and neither was visible from a unit test of
 * the thing being limited.
 *
 * One second, so the wait is one second.  Both sides are asserted: a
 * build with the cycle check deleted passes the "delivered after" half
 * on its own.
 */
static void
test_the_cycle_window_reaches_the_guard(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) first = NULL;
    g_autoptr(JsonNode) repeat = NULL;
    g_autoptr(JsonNode) after = NULL;
    g_autoptr(JsonNode) events = NULL;
    JsonArray *list;
    gboolean saw_refusal = FALSE;
    guint i;

    fixture_setup(&fixture,
                  "orchestration:\n  cycle_seconds: 1\n"
                  "agents:\n  - id: fai\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    first = request(&fixture, "msg.send",
                    "{\"target\":\"fai\",\"body\":\"Error: spawn failed\"}");
    g_assert_false(clawt_ipc_frame_is_error(first));

    /* Inside the window. This is the control. */
    repeat = request(&fixture, "msg.send",
                     "{\"target\":\"fai\",\"body\":\"Error: spawn failed\"}");
    g_assert_true(clawt_ipc_frame_is_error(repeat));

    /*
     * And the refusal names the configured number rather than the
     * built-in default, which is what says the config reached the guard
     * rather than the guard having a window of its own.
     */
    {
        g_autoptr(GError) refusal = clawt_ipc_frame_to_error(repeat);

        g_assert_nonnull(refusal);
        g_assert_nonnull(strstr(refusal->message,
                                "within the last 1 seconds"));
    }

    /*
     * Published against the *room*, because that is where the refusal
     * happened.  Worth asserting: an operator's first instinct is to
     * ask about the agent, and that listing does not contain it.
     */
    events = request(&fixture, "event.list", "{\"limit\":50}");
    list = json_object_get_array_member(payload_of(events), "events");

    for (i = 0; i < json_array_get_length(list); i++) {
        JsonObject *event = json_array_get_object_element(list, i);

        if (g_strcmp0(json_object_get_string_member(event, "kind"),
                      "message.refused") != 0)
            continue;

        saw_refusal = TRUE;
        g_assert_cmpstr(json_object_get_string_member(event, "subject"),
                        ==, "dm:fai:user");
    }

    g_assert_true(saw_refusal);

    /*
     * Past the window the same body is delivered.  A real wait rather
     * than a seam: what is being tested is what the guard does as time
     * passes, and a test that set the timestamps by hand would be on the
     * wrong side of the window it is about.
     */
    g_usleep(1200 * 1000);

    after = request(&fixture, "msg.send",
                    "{\"target\":\"fai\",\"body\":\"Error: spawn failed\"}");
    g_assert_false(clawt_ipc_frame_is_error(after));

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

    /*
     * `ollama` is not a CLI libreclaw can drive, so it warns and runs
     * Claude Code -- which is the behaviour this test is about keeping,
     * and which the skills provisioner now also asks about when it
     * works out which harness's directories to link into.  Swallowed on
     * the domain libreclaw logs in, because the warning is true and the
     * assertion below is about the value being recorded, not about how
     * many things noticed it.
     */
    {
        GLogLevelFlags was_fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK);
        guint handler = g_log_set_handler("LibreClaw",
                                          G_LOG_LEVEL_WARNING |
                                          G_LOG_FLAG_FATAL |
                                          G_LOG_FLAG_RECURSION,
                                          swallow_warnings, NULL);

        created = request(&fixture, "agent.create",
                          "{\"id\":\"researcher\",\"provider\":\"ollama\","
                          "\"model\":\"llama3.3\"}");

        g_log_remove_handler("LibreClaw", handler);
        g_log_set_always_fatal(was_fatal);
    }

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
 * A room's own hop limit is what its messages are counted against.
 *
 * `rooms.max_hops` was parsed, stored on the #ClawtRoom, and read by
 * nothing at all -- clawt_room_get_max_hops() had no caller, so every
 * hop in every room was counted against orchestration.max_hops whatever
 * a room declared.
 *
 * The room here *raises* the fleet limit, which is the direction the
 * key exists for and a deliberate policy decision rather than an
 * accident: three agents in a room each reply one hop deeper, so an
 * ordinary standup reaches the fleet ceiling on its own, and the only
 * other remedy is to raise orchestration.max_hops -- which loosens the
 * limit for every delegation chain in the fleet in order to fix one
 * room.
 */
static void
test_a_rooms_hop_limit_overrides_the_fleets(void)
{
    Fixture fixture = { 0 };
    ClawtMailboxRouter *router;
    g_autoptr(GError) allowed = NULL;
    g_autoptr(GError) refused = NULL;
    g_autoptr(GError) direct = NULL;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 2\n"
                  "  cycle_window: 0\n"
                  "rooms:\n"
                  "  - id: standup\n"
                  "    members: [alpha, beta]\n"
                  "    require_mention: false\n"
                  "    max_hops: 6\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    router = clawt_daemon_get_router(fixture.daemon);

    /* Four hops in: past the fleet's 2, inside the room's 6. */
    g_assert_cmpint(clawt_mailbox_router_send_to(
                        router, "alpha", "standup", "still going", NULL, 4,
                        &allowed), >=, 0);
    g_assert_no_error(allowed);

    /* And the room's own limit still stops it. */
    g_assert_cmpint(clawt_mailbox_router_send_to(
                        router, "alpha", "standup", "and going", NULL, 6,
                        &refused), <, 0);
    g_assert_error(refused, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);

    /*
     * The same depth straight to an agent is refused, because a direct
     * room declared no limit of its own.  Without this the test would
     * pass just as well against a build that had simply raised the
     * fleet limit.
     */
    g_assert_cmpint(clawt_mailbox_router_send_to(
                        router, "alpha", "beta", "still going", NULL, 4,
                        &direct), <, 0);
    g_assert_error(direct, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);

    fixture_teardown(&fixture);
}

/*
 * The periodic sweep applies the exchange's size cap.
 *
 * clawt_exchange_prepare() enforces it when an agent starts and before
 * a file put -- which is every way the *daemon* can make the exchange
 * grow, and not the way the exchange exists for.  A file written
 * through the mount from inside an agent's own computer is invisible to
 * all of those, so a fleet doing that went past
 * defaults.exchange_max_bytes and stayed there:
 * clawt_exchange_sweep() had no periodic caller at all.
 *
 * The files are written straight into the exchange directory after the
 * daemon has started, which is exactly what a computer's mount does and
 * exactly what nothing tells the daemon about.
 */
static void
test_the_sweep_applies_the_exchange_cap(void)
{
    Fixture fixture = { 0 };
    ClawtExchange *exchange;
    g_autofree gchar *shared = NULL;
    g_autofree gchar *older = NULL;
    g_autofree gchar *newer = NULL;
    g_autofree gchar *filler = g_strnfill(1000, 'x');
    struct utimbuf times;

    /*
     * Indented, because the fixture has just written `defaults:` and its
     * workspace_root: this continues that mapping rather than opening a
     * second top-level `defaults:`, which YAML resolves by discarding
     * the first -- taking workspace_root with it and scaffolding into
     * the developer's real fleet.
     */
    fixture_setup(&fixture, "  exchange_max_bytes: 1500\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    exchange = clawt_daemon_get_exchange(fixture.daemon);
    g_assert_nonnull(exchange);

    shared = g_build_filename(clawt_exchange_get_root(exchange), "shared",
                              NULL);
    g_assert_cmpint(g_mkdir_with_parents(shared, 0700), ==, 0);

    older = g_build_filename(shared, "older.bin", NULL);
    newer = g_build_filename(shared, "newer.bin", NULL);

    g_assert_true(g_file_set_contents(older, filler, 1000, NULL));
    g_assert_true(g_file_set_contents(newer, filler, 1000, NULL));

    /*
     * Dated rather than waited for.  The sweep removes oldest first, and
     * two files written in the same second would leave the sort deciding
     * which one goes -- an assertion that passes or fails on the order
     * readdir happened to return.
     */
    times.actime = time(NULL) - 3600;
    times.modtime = times.actime;
    g_assert_cmpint(g_utime(older, &times), ==, 0);

    /* Over the cap, and nothing has noticed. */
    g_assert_cmpint(clawt_exchange_get_size(exchange), ==, 2000);

    clawt_daemon_sweep(fixture.daemon);

    g_assert_false(g_file_test(older, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(newer, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

/*
 * The daemon hands its event bus to the orchestration tools.
 *
 * The emitting is tested in test-mcp-tools.c, where the bus is set by
 * the fixture -- which is exactly the shape that hides a missing
 * caller: a mechanism correct in isolation and reaching nobody in the
 * product. This is the wire, and nothing else here tests it.
 *
 * Read back off the daemon's own bus, and asserted on the *subject*
 * being the agent, because that is what makes the trail filterable and
 * what the client path already publishes.
 */
static void
test_the_daemon_gives_the_tools_its_event_bus(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(GPtrArray) events = NULL;
    ClawtAgent *agent;
    gboolean found = FALSE;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "chief");
    g_assert_nonnull(agent);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    g_assert_true(json_parser_load_from_data(
        parser,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"clawtilla_computer_exec\","
        "\"arguments\":{\"command\":\"echo audited\","
        "\"timeout\":10}}}", -1, NULL));

    response = clawt_mcp_tools_call(clawt_daemon_get_mcp_tools(fixture.daemon),
                                    "chief", NULL,
                                    json_parser_get_root(parser));
    g_assert_nonnull(response);

    events = clawt_event_bus_replay(clawt_daemon_get_event_bus(fixture.daemon),
                                    0, NULL);

    for (i = 0; events != NULL && i < events->len; i++) {
        ClawtEvent *event = g_ptr_array_index(events, i);

        if (g_strcmp0(clawt_event_get_kind(event), "computer.exec") != 0)
            continue;

        g_assert_cmpstr(clawt_event_get_subject(event), ==, "chief");
        g_assert_cmpstr(clawt_event_get_detail(event, "command"), ==,
                        "echo audited");
        found = TRUE;
    }

    g_assert_true(found);

    fixture_teardown(&fixture);
}

/*
 * An urgent message is leased before an ordinary one that was sent
 * first.
 *
 * The whole point of the priority bands, and until now they had no
 * writer at all outside a test: clawt_mailbox_item_set_priority() was
 * called by nothing in the product, so every item the router had ever
 * queued sat at the constructor's NORMAL.  The mailbox leased by band,
 * `drop-oldest` shed the lowest band first, docs/mailboxes.org described
 * URGENT overtaking, and clawtilla_message_agent told agents that urgent
 * jumps the queue.  Four bands, one of them ever used.
 *
 * The assertion is on the *order*, not on the field.  Checking that the
 * band round-trips onto the item would have passed the day a band
 * started being stored and never delivered anything sooner, which is a
 * test of a getter rather than of the feature; this one fails unless
 * the second message genuinely overtakes the first.
 */
static void
test_urgent_overtakes_an_earlier_ordinary_message(void)
{
    Fixture fixture = { 0 };
    ClawtMailboxRouter *router;
    ClawtAgentManager *agents;
    ClawtMailbox *mailbox;
    g_autoptr(ClawtMailboxItem) first = NULL;
    g_autoptr(ClawtMailboxItem) second = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    router = clawt_daemon_get_router(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);

    /*
     * The ordinary one first, so nothing but the band can decide the
     * order: the mailbox breaks a tie by created_at ascending, which
     * without the band would hand this one back first every time.
     *
     * Nothing is connected in this test, so the router's immediate
     * drain has no open link to hand anything to and both stay queued.
     */
    g_assert_cmpint(clawt_mailbox_router_send_to(
                        router, "alpha", "beta", "whenever you get a moment",
                        NULL, 0, &error), >, 0);
    g_assert_no_error(error);

    g_assert_cmpint(clawt_mailbox_router_send_to_full(
                        router, "alpha", "beta", "the build is broken",
                        NULL, 0, CLAWT_PRIORITY_URGENT, &error), >, 0);
    g_assert_no_error(error);

    mailbox = clawt_agent_get_mailbox(clawt_agent_manager_get(agents,
                                                              "beta"));
    g_assert_nonnull(mailbox);

    first = clawt_mailbox_lease(mailbox, 60);
    g_assert_nonnull(first);
    g_assert_cmpstr(clawt_mailbox_item_get_body(first), ==,
                    "the build is broken");
    g_assert_cmpint(clawt_mailbox_item_get_priority(first), ==,
                    CLAWT_PRIORITY_URGENT);

    /* And the ordinary one is still there, behind it rather than lost. */
    second = clawt_mailbox_lease(mailbox, 60);
    g_assert_nonnull(second);
    g_assert_cmpstr(clawt_mailbox_item_get_body(second), ==,
                    "whenever you get a moment");
    g_assert_cmpint(clawt_mailbox_item_get_priority(second), ==,
                    CLAWT_PRIORITY_NORMAL);

    fixture_teardown(&fixture);
}

/*
 * And a pod's own `message_agent` carries the band it names.
 *
 * `priority` has been in the pod module's declared parameters for as
 * long as the module has existed, and the daemon's action handler never
 * read it -- so a pod that set one was accepted, answered `ok`, and
 * queued at NORMAL like everything else.  Driven through a real `.pod`
 * file rather than by calling the handler, because the handler is
 * static and the thing being fixed is precisely the wiring between the
 * declared parameter and the queue.
 */
static void
test_a_pod_can_send_at_a_band(void)
{
    Fixture fixture = { 0 };
    ClawtAgentManager *agents;
    ClawtMailbox *mailbox;
    g_autofree gchar *pods = NULL;
    g_autofree gchar *pod_file = NULL;
    g_autoptr(ClawtMailboxItem) item = NULL;
    g_autoptr(GError) error = NULL;
    gint64 deadline;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");

    /*
     * Written before the daemon starts: pods are read once, at start,
     * and `on_daemon_started` is emitted after that -- so this is the
     * one event a test can rely on without reaching into the bus.
     */
    pods = g_build_filename(fixture.dir, "pods", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pods, 0700), ==, 0);

    pod_file = g_build_filename(pods, "wake.pod", NULL);
    g_file_set_contents(
        pod_file,
        "pod fleet = clawtilla->new();\n"
        "fleet->on_daemon_started => clawtilla->message_agent("
        "agent: \"beta\", body: \"the build is broken\", "
        "priority: \"urgent\");\n", -1, &error);
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    agents = clawt_daemon_get_agents(fixture.daemon);
    mailbox = clawt_agent_get_mailbox(clawt_agent_manager_get(agents,
                                                              "beta"));
    g_assert_nonnull(mailbox);

    /*
     * Pumped against wall time rather than for a fixed number of
     * iterations: the pod engine does its own work off this context, so
     * a count of non-blocking iterations burns through while it is still
     * running and proves nothing.
     */
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while (item == NULL && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(fixture.context, FALSE);
        item = clawt_mailbox_lease(mailbox, 60);
    }

    g_assert_nonnull(item);
    g_assert_cmpstr(clawt_mailbox_item_get_body(item), ==,
                    "the build is broken");
    g_assert_cmpint(clawt_mailbox_item_get_priority(item), ==,
                    CLAWT_PRIORITY_URGENT);

    fixture_teardown(&fixture);
}

/*
 * And a pod's `memory_add` files what it records under the category it
 * named.
 *
 * #ClawtMemory is a boxed record with public fields and no properties,
 * and the daemon set the category with g_object_set() -- which on a
 * pointer that is not a GObject does not warn, it segfaults.  Restoring
 * the call makes this test abort with SIGSEGV rather than fail an
 * assertion, which is the size of what it was: a pod that classified
 * what it recorded killed the daemon running it.
 *
 * Driven through a real `.pod` file rather than by calling the handler,
 * because the handler is static and what is being fixed is the wiring
 * between a declared parameter and the row on disk.
 */
static void
test_a_pod_remembers_in_the_category_it_named(void)
{
    Fixture fixture = { 0 };
    ClawtAgentManager *agents;
    ClawtMemoryStore *store;
    g_autofree gchar *pods = NULL;
    g_autofree gchar *pod_file = NULL;
    g_autoptr(GPtrArray) found = NULL;
    g_autoptr(GError) error = NULL;
    gint64 deadline;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n");

    pods = g_build_filename(fixture.dir, "pods", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pods, 0700), ==, 0);

    pod_file = g_build_filename(pods, "learn.pod", NULL);
    g_file_set_contents(
        pod_file,
        "pod fleet = clawtilla->new();\n"
        "fleet->on_daemon_started => clawtilla->memory_add("
        "agent: \"alpha\", content: \"the build needs libyaml-devel\", "
        "category: \"decision\");\n", -1, &error);
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    agents = clawt_daemon_get_agents(fixture.daemon);
    store = clawt_agent_get_memory(clawt_agent_manager_get(agents, "alpha"));
    g_assert_nonnull(store);

    /*
     * Against wall time, for the same reason the band test is: the pod
     * engine does its work off this context, so a fixed number of
     * non-blocking iterations proves nothing about whether it ran.
     */
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while (clawt_memory_store_count(store, TRUE) == 0 &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, FALSE);

    g_assert_cmpuint(clawt_memory_store_count(store, TRUE), ==, 1);

    /*
     * Asked for by category rather than read back whole: the listing
     * with no category passed throughout the bug, because the memory was
     * always written -- only under the wrong name.
     */
    found = clawt_memory_store_list(store, "decision", FALSE, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(found->len, ==, 1);
    g_assert_cmpstr(((ClawtMemory *)g_ptr_array_index(found, 0))->content,
                    ==, "the build needs libyaml-devel");

    fixture_teardown(&fixture);
}

/*
 * Counts warnings that name the level a pod got wrong.
 *
 * A counter of its own rather than a parameterised matcher: what each of
 * these looks for is the point of the assertion, and one shared helper
 * taking a needle reads as though the two tests were checking the same
 * thing.
 */
static void
count_importance_warnings(const gchar *domain, GLogLevelFlags level,
                          const gchar *message, gpointer user_data)
{
    guint *seen = user_data;

    (void)domain;
    (void)level;

    if (message != NULL && strstr(message, "is not an importance") != NULL)
        (*seen)++;
}

/*
 * And it files the other four fields too.
 *
 * `memory_add` declared `agent`, `content` and `category` while
 * clawtilla_memory_add offered an agent five -- so an automation could
 * say what kind of thing it had noticed and not how much it mattered,
 * and everything a pod recorded landed at `normal` with no summary and
 * no tags.  The two write into one listing, sorted and searched by the
 * same columns, so the narrower one was the one that was wrong.
 *
 * Driven through a real `.pod` file for the same reason the category
 * test is: what is asserted is the wiring between a declared parameter
 * and the row on disk, and the handler is static.
 */
static void
test_a_pod_remembers_every_field_it_named(void)
{
    Fixture fixture = { 0 };
    ClawtAgentManager *agents;
    ClawtMemoryStore *store;
    ClawtMemory *memory;
    g_autofree gchar *pods = NULL;
    g_autofree gchar *pod_file = NULL;
    g_autoptr(GPtrArray) found = NULL;
    g_autoptr(GError) error = NULL;
    gint64 deadline;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n");

    pods = g_build_filename(fixture.dir, "pods", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pods, 0700), ==, 0);

    pod_file = g_build_filename(pods, "learn.pod", NULL);
    g_file_set_contents(
        pod_file,
        "pod fleet = clawtilla->new();\n"
        "fleet->on_daemon_started => clawtilla->memory_add("
        "agent: \"alpha\", content: \"the disk filled at 03:00\", "
        "category: \"learning\", summary: \"disk filled overnight\", "
        "importance: \"critical\", tags: \"disk,ops\");\n", -1, &error);
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    agents = clawt_daemon_get_agents(fixture.daemon);
    store = clawt_agent_get_memory(clawt_agent_manager_get(agents, "alpha"));
    g_assert_nonnull(store);

    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while (clawt_memory_store_count(store, TRUE) == 0 &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, FALSE);

    found = clawt_memory_store_list(store, NULL, FALSE, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(found->len, ==, 1);

    memory = g_ptr_array_index(found, 0);
    g_assert_cmpstr(memory->content, ==, "the disk filled at 03:00");
    g_assert_cmpstr(memory->category, ==, "learning");
    g_assert_cmpstr(memory->summary, ==, "disk filled overnight");
    g_assert_cmpstr(memory->importance, ==, "critical");
    g_assert_cmpstr(memory->tags, ==, "disk,ops");

    /*
     * And stamped as the pod rather than as alpha, which is what
     * clawtilla_memory_add records.  An automation filing something out
     * of an event payload is the case clawt_memory_provenance_rule()
     * exists for: with no source it reads back a month later as
     * something the agent worked out itself.
     */
    g_assert_cmpstr(memory->source, ==, "pod");

    fixture_teardown(&fixture);
}

/*
 * A level the pod got wrong is refused, and nothing is written.
 *
 * `importance` is a plain TEXT column and the store binds what it is
 * given, so "urgent" -- a priority band, and the near-miss somebody
 * actually writes -- would be stored and would then sort as none of the
 * four levels.  A pod runs unattended, so that is wrong on every run of
 * the rule rather than once.
 *
 * Two bindings, not one.  Asserting only that nothing was written is an
 * assertion a pod that never ran at all would pass, which is the shape
 * this tree has been caught by before; the valid one landing is what
 * says the engine reached the action.
 */
static void
test_a_pod_naming_a_level_that_is_not_one_writes_nothing(void)
{
    Fixture fixture = { 0 };
    ClawtAgentManager *agents;
    ClawtMemoryStore *store;
    g_autofree gchar *pods = NULL;
    g_autofree gchar *pod_file = NULL;
    g_autoptr(GPtrArray) found = NULL;
    g_autoptr(GError) error = NULL;
    GLogLevelFlags was_fatal;
    guint handler;
    guint warned = 0;
    gint64 deadline;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n");

    pods = g_build_filename(fixture.dir, "pods", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pods, 0700), ==, 0);

    pod_file = g_build_filename(pods, "learn.pod", NULL);
    g_file_set_contents(
        pod_file,
        "pod fleet = clawtilla->new();\n"
        "fleet->on_daemon_started => clawtilla->memory_add("
        "agent: \"alpha\", content: \"this one is fine\", "
        "category: \"fact\");\n"
        "fleet->on_daemon_started => clawtilla->memory_add("
        "agent: \"alpha\", content: \"this one is not\", "
        "importance: \"urgent\");\n", -1, &error);
    g_assert_no_error(error);

    /*
     * GTest makes a warning fatal and the refusal is reported as one, so
     * it is counted rather than left to kill the run.  Counted rather
     * than swallowed: a refusal that reached nobody and a refusal that
     * never happened both leave the store with one row in it.
     */
    was_fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK);
    handler = g_log_set_handler("Clawtilla",
                                G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL |
                                G_LOG_FLAG_RECURSION,
                                count_importance_warnings, &warned);

    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    agents = clawt_daemon_get_agents(fixture.daemon);
    store = clawt_agent_get_memory(clawt_agent_manager_get(agents, "alpha"));
    g_assert_nonnull(store);

    /*
     * Both halves waited for, so neither ordering of the two bindings
     * ends the loop with the other still to be dispatched.
     */
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while ((clawt_memory_store_count(store, TRUE) == 0 || warned == 0) &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, FALSE);

    g_assert_cmpuint(warned, >=, 1);

    found = clawt_memory_store_list(store, NULL, FALSE, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(found->len, ==, 1);
    g_assert_cmpstr(((ClawtMemory *)g_ptr_array_index(found, 0))->content,
                    ==, "this one is fine");

    g_log_remove_handler("Clawtilla", handler);
    g_log_set_always_fatal(was_fatal);

    fixture_teardown(&fixture);
}

/*
 * A screen with nothing captured yet answers, rather than failing.
 *
 * `computer.frame` returned CLAWT_ERROR_NOT_FOUND for the ordinary state
 * of a screen nobody has captured from yet -- which is every screen for
 * the first second of watching, and for as long as a VM takes to boot or
 * be rebuilt. Both clients poll this, and the GTK client toasts every
 * failed request, so that was one toast per refresh saying what the
 * panel underneath already said in place.
 *
 * Asserted on the *frame kind* rather than on the message: what was
 * wrong was the channel, not the wording, and a test phrased in terms of
 * the sentence would pass against a version that still called it an
 * error and merely said it more nicely.
 */
static void
test_a_screen_with_no_frame_yet_is_not_an_error(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *payload;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "computer.frame", "{\"agent\": \"alpha\"}");

    g_assert_false(clawt_ipc_frame_is_error(reply));

    payload = clawt_ipc_frame_get_payload(reply);
    g_assert_nonnull(payload);

    /*
     * A stated absence rather than an implied one. A client has to be
     * able to tell "there is no picture" from "this build does not send
     * one", which an absent member cannot say.
     */
    g_assert_true(json_object_has_member(payload, "pending"));
    g_assert_true(json_object_get_boolean_member(payload, "pending"));

    /* And no picture, so nothing decodes an empty string as an image. */
    g_assert_false(json_object_has_member(payload, "base64"));

    /*
     * Nobody is watching, which is one of the two reasons there is no
     * frame and the one a person can act on.
     */
    g_assert_cmpint(json_object_get_int_member(payload, "watchers"), ==, 0);

    /*
     * Not stale either: a frame that has never been taken has no age,
     * and a client labelling one would draw "55 years ago" over an empty
     * panel.
     */
    g_assert_false(json_object_get_boolean_member(payload, "stale"));
    g_assert_cmpint(json_object_get_int_member(payload, "stamp"), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A message the router routed can be recalled through the daemon.
 *
 * The index, the router and the IPC verb each work on their own; what
 * this asserts is the *wire* between them, which is the half that has
 * gone missing three times in this tree -- a factory nothing calls, a
 * signal nothing connects to, a limit nothing increments.  Driven end to
 * end through `memory.recall` for that reason: an assertion against
 * ClawtTranscriptIndex directly would pass with the router never having
 * been given one.
 */
static void
test_a_routed_message_can_be_recalled(void)
{
    Fixture fixture = { 0 };
    ClawtMailboxRouter *router;
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *hits;
    JsonObject *hit;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    router = clawt_daemon_get_router(fixture.daemon);
    g_assert_cmpint(clawt_mailbox_router_send_to(router, "alpha", "beta",
                                                 "the deploy key expired",
                                                 NULL, 0, NULL),
                    >=, 0);

    reply = request(&fixture, "memory.recall",
                    "{\"query\": \"deploy key\"}");

    hits = json_object_get_array_member(clawt_ipc_frame_get_payload(reply),
                                        "hits");

    g_assert_cmpuint(json_array_get_length(hits), ==, 1);

    hit = json_array_get_object_element(hits, 0);
    g_assert_cmpstr(json_object_get_string_member(hit, "body"), ==,
                    "the deploy key expired");
    g_assert_cmpstr(json_object_get_string_member(hit, "from"), ==, "alpha");

    /*
     * And the room, which is the reason the index is fed from the router
     * rather than from either end of the link: `beta` is an agent id,
     * and the message landed in the direct room between the two.  An
     * index that recorded what the message *said* would have filed this
     * under a room that does not exist.
     */
    g_assert_cmpstr(json_object_get_string_member(hit, "room"), !=, "beta");

    fixture_teardown(&fixture);
}

/*
 * The operator profile reaches every agent's USER.org, and leaves when
 * it is turned off.
 *
 * Written to every agent rather than only a newly created one: a fleet
 * that learnt something in March and told only the agents made after it
 * has two halves that know different things, and nothing to say which is
 * which.  And a setting somebody turned off has to take the region back
 * out, not leave it in a prompt that is already written.
 */
static void
test_the_operator_profile_reaches_every_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) written = NULL;
    g_autoptr(JsonNode) read_back = NULL;
    g_autofree gchar *user_org = NULL;
    g_autofree gchar *contents = NULL;

    fixture_setup(&fixture,
                  "memories:\n  operator_profile: true\n"
                  "agents:\n  - id: alpha\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    written = request(&fixture, "operator.set",
                      "{\"text\": \"* Vitals\\n- Timezone: UTC-5\"}");
    g_assert_nonnull(clawt_ipc_frame_get_payload(written));

    read_back = request(&fixture, "operator.get", NULL);
    g_assert_cmpstr(
        json_object_get_string_member(clawt_ipc_frame_get_payload(read_back),
                                      "text"),
        ==, "* Vitals\n- Timezone: UTC-5");

    user_org = g_build_filename(fixture.dir, "agents", "alpha", "USER.org",
                                NULL);
    g_assert_true(g_file_get_contents(user_org, &contents, NULL, NULL));

    g_assert_nonnull(strstr(contents, "BEGIN clawtilla operator profile"));
    g_assert_nonnull(strstr(contents, "UTC-5"));

    /*
     * Off, and the region goes.  Asserted on the marker rather than on
     * the text: a rewrite that left an empty heading behind would still
     * be costing every turn's context to say nothing.
     */
    {
        g_autoptr(JsonNode) off = NULL;
        g_autofree gchar *after = NULL;
        g_autoptr(GPtrArray) refusals = NULL;

        fixture_write_config(&fixture,
                             "memories:\n  operator_profile: false\n"
                             "agents:\n  - id: alpha\n");
        g_assert_true(clawt_daemon_reload(fixture.daemon, NULL));

        off = request(&fixture, "operator.set",
                      "{\"text\": \"* Vitals\\n- Timezone: UTC-5\"}");
        g_assert_nonnull(off);
        (void)refusals;

        g_assert_true(g_file_get_contents(user_org, &after, NULL, NULL));
        g_assert_null(strstr(after, "BEGIN clawtilla operator profile"));
    }

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

    /*
     * Seven hops in, and the turn begins: the depth is real and must be
     * kept for the whole of it.
     *
     * Through the typing signal rather than clawt_agent_set_activity(),
     * because the start of a turn is what decides whether the depth is
     * this turn's or the last one's -- and driving the state directly
     * would skip the decision this test is about.
     */
    clawt_agent_set_hop_depth(alpha, 7);
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "typing", "alpha", "beta", TRUE);
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
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "typing", "alpha", "beta", FALSE);
    g_assert_cmpint(clawt_agent_get_hop_depth(alpha), ==, 7);

    /*
     * And the reply does *not* carry it away.
     *
     * This asserted zero here, which encoded the defect as the
     * intention: a turn is not one message, so clearing on the first
     * outbound started the second at depth 1 and two agents signing off
     * at each other never reached max_hops. What clears it is the start
     * of a turn nothing delivered into, below.
     */
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "message", "alpha", "beta", "Done.", NULL);
    g_assert_cmpint(clawt_agent_get_hop_depth(alpha), ==, 7);

    /*
     * A second turn, with no delivery before it -- Matrix, a webhook,
     * the person typing into libreclaw directly. That one starts from
     * zero rather than nine hops into somebody else's conversation.
     */
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "typing", "alpha", "beta", TRUE);
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

    /* A deep chain reached alpha, and alpha's turn ran and answered it. */
    clawt_agent_set_hop_depth(alpha, 4);
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "typing", "alpha", "beta", TRUE);
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "typing", "alpha", "beta", FALSE);
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "message", "alpha", "beta", "Done.", NULL);

    /*
     * Now a person says something in Matrix.  Nothing in that path
     * touches the router, so no delivery records a depth for it -- and
     * the turn starting is what says so. The first hop of a new
     * conversation must be allowed.
     *
     * The turn boundary is the signal, rather than the agent's previous
     * message: a turn that sends two messages must count both from the
     * same place, which is why the depth outlives the first of them.
     */
    g_signal_emit_by_name(clawt_daemon_get_link_server(fixture.daemon),
                          "typing", "alpha", "beta", TRUE);

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
 * keys, so a stray `session:` here would silently delete the per-agent
 * persist_dir, and two agents sharing one means either resuming the
 * other's conversation.  The refusal is right; what was missing is that
 * it reached nobody.
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
                  "      session:\n"
                  "        persist_dir: /tmp/somewhere-else\n");

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
 * The refusal reader is total over JsonNodes, not just over the ones the
 * daemon happens to build.
 *
 * JSON_NODE_HOLDS_OBJECT() is true of a node whose object is NULL --
 * json_node_new(JSON_NODE_OBJECT) makes exactly that -- so a type check
 * is not the same as a pointer check.  All three clients hand this
 * function whatever came back, the GTK one on every single reply, and a
 * json-glib CRITICAL there is a line on somebody's console pointing at
 * the wrong layer entirely.
 *
 * A critical is fatal under GTest, so the call is the assertion.
 */
static void
test_the_refusal_reader_survives_an_object_that_is_not_there(void)
{
    g_autoptr(JsonNode) hollow = json_node_new(JSON_NODE_OBJECT);
    g_autoptr(JsonNode) array = json_node_new(JSON_NODE_ARRAY);
    guint counted = 1;

    g_assert_true(JSON_NODE_HOLDS_OBJECT(hollow));
    g_assert_null(json_node_get_object(hollow));

    g_assert_null(clawt_ipc_reply_refusal_text(hollow, &counted));
    g_assert_cmpuint(counted, ==, 0);

    /* The neighbours that were already safe, so this stays a set. */
    counted = 1;
    g_assert_null(clawt_ipc_reply_refusal_text(array, &counted));
    g_assert_cmpuint(counted, ==, 0);

    counted = 1;
    g_assert_null(clawt_ipc_reply_refusal_text(NULL, &counted));
    g_assert_cmpuint(counted, ==, 0);
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
 * A typing frame is a level, not an edge.
 *
 * libreclaw holds the indicator up for the whole of a turn and re-sends
 * the same TRUE every 25 seconds so Matrix does not drop it.  Read as a
 * turn start, each of those restarted the turn: the depth went back to
 * zero so `orchestration.max_hops` could not climb, the closed-exchange
 * flag went back to TRUE so a sign-off was routed and cost the other
 * agent a whole turn, the origin was cleared -- which does not mislead
 * clawtilla_message_user's guard so much as switch it off -- and the
 * task a delegation would be parented on was dropped.
 *
 * A live fleet produced 11,869 typing=true frames against 549 turns, and
 * 22% of its peer messages were sign-offs that had been delivered.
 *
 * Driven through on_link_typing() rather than by calling
 * clawt_agent_begin_turn() twice, which is what the suite already did:
 * that is the API for two *turns*, and a test written against it cannot
 * tell this build from the broken one.
 */
static void
test_a_keepalive_is_not_a_new_turn(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgentManager *agents;
    ClawtAgent *alpha;
    ClawtAgent *beta;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 0\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);
    alpha = clawt_agent_manager_get(agents, "alpha");
    beta = clawt_agent_manager_get(agents, "beta");

    /* A peer's reply: three hops in, and it closes the exchange. */
    clawt_agent_deliver_turn(beta, NULL, 3, FALSE, "alpha", "task-xyz");

    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);

    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 3);
    g_assert_false(clawt_agent_get_turn_replies(beta));
    g_assert_cmpstr(clawt_agent_get_turn_origin(beta), ==, "alpha");
    g_assert_cmpstr(clawt_agent_get_turn_task_id(beta), ==, "task-xyz");

    /* Twenty-five seconds later, the same turn, the same indicator. */
    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);

    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 3);
    g_assert_false(clawt_agent_get_turn_replies(beta));
    g_assert_cmpstr(clawt_agent_get_turn_origin(beta), ==, "alpha");
    g_assert_cmpstr(clawt_agent_get_turn_task_id(beta), ==, "task-xyz");

    /*
     * And the sign-off the turn ends with still goes nowhere.  This is
     * the assertion that costs money when it fails: "Acknowledged, no
     * reply needed or sent -- ending turn." routed to a peer is a whole
     * model turn spent on somebody saying they have nothing to say.
     *
     * max_hops is 0 here on purpose, so a pass cannot come from the hop
     * limit stopping it instead.
     */
    g_signal_emit_by_name(links, "typing", "beta", "alpha", FALSE);
    g_signal_emit_by_name(links, "message", "beta", "alpha",
                          "Acknowledged, nothing to send. Ending turn.",
                          NULL);

    g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(alpha)),
                     ==, 0);

    fixture_teardown(&fixture);
}

/*
 * Each room's turn is its own, and one is not judged by another's flags.
 *
 * An agent runs a turn per session and a session is a room, so it can
 * have several going at once -- a live fleet had one agent with three
 * rooms' typing frames overlapping and three separate falses.  These
 * fields were scalars on the agent, so those turns shared one
 * description and each was judged by whichever room wrote last.
 *
 * The reply flag is the severe one and both directions are wrong: a real
 * answer swallowed because *another* room's turn was a closed exchange
 * is silent data loss, and a sign-off delivered because another room's
 * was not costs the recipient a whole model turn.  This asserts both, in
 * the mailboxes rather than on the flags, because that is where the
 * money goes.
 *
 * max_hops is disabled so a pass cannot come from the hop limit refusing
 * one of the two messages for its own reasons.
 */
static void
test_each_room_has_its_own_turn(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgentManager *agents;
    ClawtAgent *alpha;
    ClawtAgent *gamma;
    ClawtAgent *beta;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 0\n"
                  "agents:\n  - id: alpha\n  - id: gamma\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);
    alpha = clawt_agent_manager_get(agents, "alpha");
    gamma = clawt_agent_manager_get(agents, "gamma");
    beta = clawt_agent_manager_get(agents, "beta");

    /*
     * Two conversations at once: alpha's is a peer's reply three hops in
     * that closes the exchange, gamma's is a fresh question one hop in
     * that is waiting for an answer.
     */
    clawt_agent_deliver_turn(beta, "alpha", 3, FALSE, "alpha", "task-one");
    clawt_agent_deliver_turn(beta, "gamma", 1, TRUE, "gamma", "task-two");

    /*
     * gamma's turn starts first, though alpha's message arrived first.
     *
     * Deliberately the opposite order, because arrival order and
     * turn-start order are independent -- libreclaw picks up each
     * session on its own -- and a queue drained in arrival order hands
     * gamma's turn alpha's message.  With the two orders agreeing, a
     * build that ignored the room entirely still passes.
     */
    g_signal_emit_by_name(links, "typing", "beta", "gamma", TRUE);
    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);

    g_assert_cmpuint(clawt_agent_get_typing_rooms(beta), ==, 2);

    /* Each room kept its own description, rather than the last writer's. */
    g_assert_cmpint(clawt_agent_get_hop_depth_in(beta, "alpha"), ==, 3);
    g_assert_cmpint(clawt_agent_get_hop_depth_in(beta, "gamma"), ==, 1);
    g_assert_false(clawt_agent_get_turn_replies_in(beta, "alpha"));
    g_assert_true(clawt_agent_get_turn_replies_in(beta, "gamma"));
    g_assert_cmpstr(clawt_agent_get_turn_task_id_in(beta, "alpha"), ==,
                    "task-one");
    g_assert_cmpstr(clawt_agent_get_turn_task_id_in(beta, "gamma"), ==,
                    "task-two");

    /*
     * A tool call names no room, so the agent-wide answers fold across
     * both.  The depth errs towards refusing, and the task is withheld
     * because a wrong parent is worse than no parent.
     */
    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 3);
    g_assert_null(clawt_agent_get_turn_task_id(beta));

    /* alpha's turn ends: its sign-off goes nowhere, as it always should. */
    g_signal_emit_by_name(links, "typing", "beta", "alpha", FALSE);
    g_signal_emit_by_name(links, "message", "beta", "alpha",
                          "Acknowledged, nothing to send. Ending turn.",
                          NULL);

    g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(alpha)),
                     ==, 0);

    /*
     * And gamma's answer is delivered.  This is the half that used to be
     * lost in silence: judged by alpha's closed exchange, a real answer
     * somebody was waiting for was dropped and nothing said so.
     */
    g_signal_emit_by_name(links, "typing", "beta", "gamma", FALSE);
    g_signal_emit_by_name(links, "message", "beta", "gamma",
                          "Here is the answer you asked for.", NULL);

    g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(gamma)),
                     ==, 1);

    fixture_teardown(&fixture);
}

/*
 * And one room going quiet does not end the agent's turn, or make it
 * look idle.
 *
 * clawt_daemon_turn_settle() takes no room, so a FALSE from either used
 * to settle the agent -- and `busy` came from the frame, so an agent
 * still mid-turn in one room reported itself idle, which is what stops a
 * message arriving mid-turn from being taken as a task's result.
 */
static void
test_one_room_going_quiet_does_not_end_the_agents_turn(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgent *beta;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: gamma\n"
                            "  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    beta = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                   "beta");

    clawt_agent_deliver_turn(beta, "alpha", 3, FALSE, "alpha", NULL);

    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);
    g_signal_emit_by_name(links, "typing", "beta", "gamma", TRUE);
    g_signal_emit_by_name(links, "typing", "beta", "alpha", FALSE);

    g_assert_true(clawt_agent_get_busy(beta));
    g_assert_cmpuint(clawt_agent_get_typing_rooms(beta), ==, 1);

    /*
     * And alpha's turn is still there to judge the answer by: libreclaw
     * lowers the indicator before it posts, so a table that forgot the
     * room on the falling edge would have nothing left to read.
     */
    g_assert_false(clawt_agent_get_turn_replies_in(beta, "alpha"));

    g_signal_emit_by_name(links, "typing", "beta", "gamma", FALSE);

    g_assert_false(clawt_agent_get_busy(beta));
    g_assert_cmpuint(clawt_agent_get_typing_rooms(beta), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A turn ended by something other than the runtime still starts the next
 * one fresh.
 *
 * The edge is the fix and it is also the hazard: an interrupt and the
 * grace timer both end turns the runtime will never send a FALSE for, so
 * a set left standing would mean the next real frame is not a rising
 * edge, clawt_agent_begin_turn() is skipped, and the new turn runs
 * holding the abandoned turn's depth, origin and task.  Same wrong
 * answer as the bug, reached from the other side.
 */
static void
test_a_turn_settled_elsewhere_starts_the_next_one_fresh(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgent *beta;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    beta = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                   "beta");

    clawt_agent_deliver_turn(beta, NULL, 3, FALSE, "alpha", "task-xyz");
    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);
    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 3);

    /* Whatever settles it -- an interrupt here -- goes through this. */
    clawt_daemon_turn_settle(fixture.daemon, "beta");
    g_assert_cmpuint(clawt_agent_get_typing_rooms(beta), ==, 0);

    /* The next frame is a rising edge again, and nothing delivered. */
    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);

    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 0);
    g_assert_null(clawt_agent_get_turn_origin(beta));
    g_assert_true(clawt_agent_get_turn_replies(beta));

    fixture_teardown(&fixture);
}

/*
 * The turn watchdog can actually be reached.
 *
 * clawt_turn_watch_begin() installs a *fresh* deadline, and it was
 * called on every typing frame -- so a refresh every 25 seconds moved
 * the deadline every 25 seconds, and `runtime.turn_timeout_seconds`
 * could not fire for any turn running longer than the refresh interval,
 * which is every turn it was written for.  A keepalive is a timer
 * firing, not a sign of life; clawt_daemon_turn_activity() is what an
 * agent doing something actually calls.
 *
 * On a fake clock, because a budget measured in minutes cannot be
 * reached by waiting and a test that sleeps for it is a test that hangs.
 */
static gint64 daemon_fake_now;

static gint64
daemon_fake_clock(gpointer user_data)
{
    (void)user_data;

    return daemon_fake_now;
}

static void
test_a_keepalive_does_not_extend_the_turn_budget(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtTurnWatch *watch;
    gint64 left;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: beta\n"
                  "    runtime:\n"
                  "      turn_timeout_seconds: 600\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    watch = fixture.daemon->turn_watch;
    g_assert_nonnull(watch);

    daemon_fake_now = 1000 * G_USEC_PER_SEC;
    clawt_turn_watch_set_clock(watch, daemon_fake_clock, NULL, NULL);

    /* The turn starts, with the whole budget in front of it. */
    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);
    g_assert_true(clawt_turn_watch_is_watching(watch, "beta"));

    left = clawt_turn_watch_remaining(watch, "beta");
    g_assert_cmpint(left, ==, 600 * G_USEC_PER_SEC);

    /* Five wedged minutes later, libreclaw refreshes the indicator. */
    daemon_fake_now += 300 * G_USEC_PER_SEC;
    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);

    /*
     * Five minutes gone, five left.  A frame that began the turn again
     * would read 600 here, and this turn would never expire however long
     * it stayed wedged.
     */
    left = clawt_turn_watch_remaining(watch, "beta");
    g_assert_cmpint(left, ==, 300 * G_USEC_PER_SEC);

    fixture_teardown(&fixture);
}

/*
 * Starting an agent brings every region clawtilla owns up to date.
 *
 * The marker says "rewritten on every start" and that was true of a
 * *daemon* start.  An agent start refreshed the integrations paragraph
 * and the computer paragraph and left the tools, the skills and the
 * operator profile as whatever was last written -- so `clawtilla agent
 * restart`, which is what anybody reaches for when an agent is working
 * from something stale, did not fix the file it was reached for.
 *
 * Asserted through clawt_daemon_start_agent() rather than by calling the
 * refresh, because the whole defect was that the refresh existed and
 * this path did not call it.  The start itself is expected to fail --
 * there is no CLI to spawn in a hermetic run -- and that is the point:
 * the persona is brought up to date before anything tries to read it,
 * so even a start that goes no further leaves the file correct.
 */
static void
test_starting_an_agent_refreshes_its_regions(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *config;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *before = NULL;
    g_autofree gchar *stale = NULL;
    g_autofree gchar *after = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    config = clawt_agent_get_config(
        clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                "scribe"));
    g_assert_nonnull(config);

    path = clawt_workspace_file_path(config, "TOOLS.org");
    g_assert_nonnull(path);
    g_assert_true(g_file_get_contents(path, &before, NULL, NULL));

    /* The daemon start wrote it, so there is something to make stale. */
    g_assert_nonnull(strstr(before, "clawtilla_list_agents"));

    /*
     * What an older clawtilla left behind: the markers, and between them
     * a listing that no longer resembles the live one.
     */
    {
        const gchar *begin = strstr(before, "# BEGIN clawtilla tools");
        g_autofree gchar *head = g_strndup(before, begin - before);

        stale = g_strconcat(head,
                            "# BEGIN clawtilla tools -- rewritten on every "
                            "start\n\n| Tool | What it does |\n"
                            "|------+--------------|\n"
                            "| ~clawtilla_from_another_era~ | nothing |\n\n"
                            "# END clawtilla tools\n", NULL);
    }

    g_assert_true(g_file_set_contents(path, stale, -1, NULL));

    /* It cannot actually spawn here; the regions are written regardless. */
    clawt_daemon_start_agent(fixture.daemon, "scribe", &error);

    g_assert_true(g_file_get_contents(path, &after, NULL, NULL));

    g_assert_null(strstr(after, "clawtilla_from_another_era"));
    g_assert_nonnull(strstr(after, "clawtilla_list_agents"));

    fixture_teardown(&fixture);
}

/*
 * A fan-out holds its parent open, through the daemon rather than only
 * in the task manager.
 *
 * This is the wire the manager rule needed.  The busy flag closed the
 * mid-turn half of "the turn boundary is not work completion" and left
 * the rest: an assignee that does its share, hands the remainder on and
 * ends its turn is not busy, so the task completed carrying a status
 * note.  On a real fleet the stored result said, in so many words, that
 * the report had not been sent yet -- and the delegator, holding a
 * terminal state, stopped looking.
 */
static void
test_a_turn_ending_does_not_close_a_task_with_children(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtTaskManager *tasks;
    ClawtTask *parent;
    ClawtTask *child;
    g_autoptr(GError) error = NULL;
    const gchar *parent_id;

    fixture_setup(&fixture, "agents:\n  - id: oryx\n  - id: kudu\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    tasks = clawt_daemon_get_tasks(fixture.daemon);

    parent = clawt_task_manager_create(tasks, "user", "oryx",
                                       "verify all three guests", NULL,
                                       &error);
    g_assert_nonnull(parent);
    parent_id = clawt_task_get_id(parent);

    child = clawt_task_manager_create(tasks, "oryx", "kudu",
                                      "yours as well", parent_id, &error);
    g_assert_nonnull(child);

    /* oryx works, then ends its turn with a status note. */
    clawt_agent_deliver_turn(
        clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                "oryx"),
        "dm:user:oryx", 1, TRUE, "user", parent_id);

    g_signal_emit_by_name(links, "typing", "oryx", "dm:user:oryx", TRUE);
    g_signal_emit_by_name(links, "typing", "oryx", "dm:user:oryx", FALSE);
    g_signal_emit_by_name(links, "message", "oryx", "dm:user:oryx",
                          "Mine is clean. Delegated the same checks to "
                          "kudu; no consolidated report yet.", parent_id);

    /*
     * Still open, and the note is there to read.  The assignee said the
     * report had not been sent; the lifecycle used to disagree.
     */
    g_assert_cmpint(clawt_task_get_state(parent), ==, CLAWT_TASK_RUNNING);
    g_assert_null(clawt_task_get_result(parent));
    g_assert_nonnull(strstr(clawt_task_get_progress_note(parent),
                            "no consolidated report yet"));

    /* kudu finishes, and oryx's next turn ending does close it. */
    g_assert_true(clawt_task_manager_complete(tasks,
                                              clawt_task_get_id(child),
                                              "clean here too"));

    clawt_agent_deliver_turn(
        clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                "oryx"),
        "dm:user:oryx", 1, TRUE, "user", parent_id);

    g_signal_emit_by_name(links, "typing", "oryx", "dm:user:oryx", TRUE);
    g_signal_emit_by_name(links, "typing", "oryx", "dm:user:oryx", FALSE);
    g_signal_emit_by_name(links, "message", "oryx", "dm:user:oryx",
                          "All three verified.", parent_id);

    g_assert_cmpint(clawt_task_get_state(parent), ==, CLAWT_TASK_COMPLETED);
    g_assert_cmpstr(clawt_task_get_result(parent), ==, "All three verified.");

    fixture_teardown(&fixture);
}

/*
 * And a task becomes running when its assignee actually starts a turn on
 * it, which is the first moment anybody knows.
 *
 * clawtilla_delegate marked nothing running, so agent-delegated work read
 * `pending` from creation to `completed` and clawtilla_task_list carried
 * a paragraph apologising for the column.  Creating a task says work was
 * handed out and delivering it says a mailbox took it; neither says the
 * assignee looked, because a stopped agent has a full mailbox and does
 * nothing.
 */
static void
test_a_task_starts_running_when_its_turn_does(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtTaskManager *tasks;
    ClawtAgent *worker;
    ClawtTask *task;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "agents:\n  - id: worker\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    tasks = clawt_daemon_get_tasks(fixture.daemon);

    task = clawt_task_manager_create(tasks, "chief", "worker", "a job",
                                     NULL, &error);
    g_assert_nonnull(task);
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);

    worker = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                     "worker");
    clawt_agent_deliver_turn(worker, NULL, 1, TRUE, "chief",
                             clawt_task_get_id(task));

    /* Queued, and still nobody has looked at it. */
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);

    g_signal_emit_by_name(links, "typing", "worker", "dm:chief:worker", TRUE);

    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);

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



/*
 * An agent's own reply invites none of its own, and a turn started by
 * one sends nothing.
 *
 * This is the pair that stops two agents talking for ever, and neither
 * half is any use alone.  An AI CLI cannot end a turn without writing
 * something: whatever it produces is the reply, and clawtilla used to
 * route every one -- so the delivery preamble's "end your turn without
 * replying if you have nothing to say" was asking for something no agent
 * could do.  Asked to greet a peer, two of them exchanged a greeting, an
 * acknowledgement, a correction, an acknowledgement of the correction,
 * and kept going until `orchestration.max_hops` cut it off eight turns
 * in.  The hop limit was working; it was the only thing that was.
 *
 * max_hops is disabled here on purpose.  With it set, a test could pass
 * because the *hop limit* stopped the exchange, which is the behaviour
 * being replaced rather than the one being added.
 */
static void
test_a_reply_earns_no_reply(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgentManager *agents;
    ClawtMailboxRouter *router;
    ClawtAgent *alpha;
    ClawtAgent *beta;
    ClawtMailboxFilter filter = { -1, 0, TRUE };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 0\n"
                  "  cycle_window: 0\n"
                  "  rate_limit_per_minute: 0\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);
    router = clawt_daemon_get_router(fixture.daemon);
    alpha = clawt_agent_manager_get(agents, "alpha");
    beta = clawt_agent_manager_get(agents, "beta");

    /*
     * alpha deliberately writes to beta -- a clawtilla_message_agent
     * call, which is how "say hi to oryx" reaches a peer.
     */
    g_assert_cmpint(clawt_mailbox_router_send_to(router, "alpha", "beta",
                                                 "hi, welcome to the team",
                                                 NULL, 1, &error), >, 0);
    g_assert_no_error(error);

    /*
     * beta answers.  Nothing is running, so the drain never handed the
     * message over and never set beta's turn state; stand in for it the
     * way the router would, which is what these tests do for the hop
     * depth beside it.
     */
    clawt_agent_set_turn_replies(beta, TRUE);
    g_signal_emit_by_name(links, "typing", "beta", "alpha", TRUE);
    g_signal_emit_by_name(links, "typing", "beta", "alpha", FALSE);
    g_signal_emit_by_name(links, "message", "beta", "alpha",
                          "Thanks -- noted.", NULL);

    /* It arrived, because a deliberate message earns an answer. */
    {
        g_autoptr(GPtrArray) queued =
            clawt_mailbox_list(clawt_agent_get_mailbox(alpha), &filter);

        g_assert_cmpuint(queued->len, ==, 1);

        /*
         * And it arrived marked as answering, which is the whole
         * mechanism: this is what closes alpha's next turn.
         */
        g_assert_false(clawt_mailbox_item_get_invites_reply(
                           g_ptr_array_index(queued, 0)));
    }

    /*
     * Now alpha's turn, started by that reply.  Whatever it writes goes
     * nowhere -- and a build without the rule queues "You're welcome"
     * for beta, who thanks alpha for it, for ever.
     *
     * Counted before and after rather than asserted as empty: beta's
     * first message is still sitting in its own mailbox, undrained,
     * because nothing here is running to hand it to.
     */
    {
        guint before = clawt_mailbox_depth(clawt_agent_get_mailbox(beta));

        clawt_agent_set_turn_replies(alpha, FALSE);
        g_signal_emit_by_name(links, "typing", "alpha", "beta", TRUE);
        g_signal_emit_by_name(links, "typing", "alpha", "beta", FALSE);
        g_signal_emit_by_name(links, "message", "alpha", "beta",
                              "You're welcome -- shout if you need anything.",
                              NULL);

        g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(beta)),
                         ==, before);
    }

    fixture_teardown(&fixture);
}

/*
 * A closed turn still answers the operator.
 *
 * The rule is about agents talking among themselves.  Applied to the
 * operator's own room it would meet a waiting person with silence, which
 * is a worse bug than the one it fixes -- and an easy one to introduce,
 * since the check sits on the path every reply takes.
 */
static void
test_a_closed_turn_still_answers_the_operator(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgent *worker;
    ClawtRoom *room;

    fixture_setup(&fixture,
                  "orchestration:\n  cycle_window: 0\n"
                  "agents:\n  - id: worker\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    worker = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                     "worker");

    /*
     * The room the person is looking at.  A direct room exists once
     * somebody has written in it, which in a live daemon is the
     * operator's first message.
     */
    room = clawt_room_manager_get_direct(
               clawt_daemon_get_rooms(fixture.daemon), "user", "worker");
    g_assert_nonnull(room);

    clawt_agent_set_turn_replies(worker, FALSE);
    g_signal_emit_by_name(links, "message", "worker", "dm:user:worker",
                          "Done -- the build is green.", NULL);

    {
        g_autoptr(GPtrArray) history = clawt_room_get_history(room, 10);

        g_assert_cmpuint(history->len, ==, 1);
        g_assert_cmpstr(clawt_message_get_body(
                            g_ptr_array_index(history, 0)),
                        ==, "Done -- the build is green.");
    }

    fixture_teardown(&fixture);
}

/*
 * Interrupting says why it cannot, rather than reporting success.
 *
 * Both refusals matter for the same reason: a client that was told the
 * turn stopped and then watched the agent go on working would go looking
 * at the agent, when the thing that failed was the request.
 */
static void
test_interrupting_names_what_it_cannot_do(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) missing = NULL;
    g_autoptr(GError) idle = NULL;

    fixture_setup(&fixture, "agents:\n  - id: worker\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    /* Nobody by that name. */
    g_assert_false(clawt_daemon_interrupt_agent(fixture.daemon, "nobody",
                                                NULL, &missing));
    g_assert_error(missing, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(missing->message, "nobody"));

    /*
     * And an agent that has never been started has no runtime at all,
     * which is a different failure from a runtime that cannot do it.
     */
    g_assert_false(clawt_daemon_interrupt_agent(fixture.daemon, "worker",
                                                NULL, &idle));
    g_assert_error(idle, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE);
    g_assert_nonnull(strstr(idle->message, "worker"));

    fixture_teardown(&fixture);
}

/*
 * Interrupting clears the agent's activity, and says so.
 *
 * The daemon lowers it rather than waiting for libreclaw's typing frame.
 * Killing a turn mid-flight is the one case where that frame may never
 * arrive -- the code that sends it runs when the turn finishes, and the
 * turn was just taken out from under it. An agent that shows as working
 * for ever after somebody pressed stop is exactly the state the button
 * exists to get out of.
 */
static void
test_interrupting_clears_the_activity(void)
{
    Fixture fixture = { 0 };
    ClawtLinkServer *links;
    ClawtAgent *worker;
    g_autoptr(GError) error = NULL;
    guint killed = 99;

    fixture_setup(&fixture, "agents:\n  - id: worker\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    worker = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                     "worker");

    /*
     * A runtime that exists and refuses, which is the path the guard is
     * actually on. Without one the daemon returns at its own
     * `runtime == NULL` check and never reaches the clearing at all --
     * so a test phrased around an agent that was never started passes
     * against a build that clears the flag unconditionally.
     */
    {
        ClawtAgentConfig *agent_config =
            clawt_config_get_agent(clawt_daemon_get_config(fixture.daemon),
                                   "worker");
        g_autoptr(ClawtProcessRuntime) runtime =
            clawt_process_runtime_new(agent_config, "/dev/null");

        clawt_agent_set_runtime(worker, CLAWT_AGENT_RUNTIME(runtime));
    }

    /* A turn is running as far as every client can see. */
    g_signal_emit_by_name(links, "typing", "worker", "dm:user:worker", TRUE);
    g_assert_true(clawt_agent_get_busy(worker));

    /*
     * The runtime refuses -- it was never started, so there is nothing
     * below it -- and the activity must survive that. A handler that
     * cleared the flag before finding out whether it could do anything
     * would report an idle agent that is still working, which is the lie
     * this whole path is built to avoid.
     */
    g_assert_false(clawt_daemon_interrupt_agent(fixture.daemon, "worker",
                                                &killed, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE);
    g_assert_true(clawt_agent_get_busy(worker));

    /* And the count is cleared even on the failure path. */
    g_assert_cmpuint(killed, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A restart policy changed in the config reaches the agent's runtime at
 * its next start.
 *
 * It did not.  clawt_daemon_start_agent() builds the runtime inside
 * `if (clawt_agent_get_runtime(agent) == NULL)` and set the policy in
 * the same block, and nothing ever sets a runtime back to NULL -- so
 * the policy, the backoff and the restart ceiling were read from the
 * configuration exactly once, at an agent's first start, and never
 * again.  A reload reconciles agents rather than rebuilding them, so
 * the object carrying the stale answer is the one that survives.
 *
 * The visible cost is an agent left down while holding queued mail:
 * `restart: always` was set, saved, reloaded and reported applied,
 * while the runtime went on refusing to restart a clean exit because it
 * still held the on-failure default.  Every surface agreed the change
 * had landed.
 *
 * Same shape as the shadow state that outlived the config that caused
 * it -- a live object keeping an answer it took at construction.
 */
static void
test_a_changed_restart_policy_reaches_the_runtime(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgent *agent;

    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    runtime:\n"
        "      restart: on-failure\n"
        "    computer:\n"
        "      type: none\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    /* First start: the runtime is built and takes the configured policy. */
    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "worker");
    g_assert_nonnull(agent);
    g_assert_cmpint(
        clawt_agent_runtime_get_restart_policy(clawt_agent_get_runtime(agent)),
        ==, CLAWT_RESTART_ON_FAILURE);

    clawt_daemon_stop_agent(fixture.daemon, "worker", FALSE);

    /* The operator changes it and reloads, exactly as the CLI does. */
    {
        g_autoptr(JsonNode) reply = request(
            &fixture, "agent.set",
            "{\"agent\": \"worker\", \"key\": \"runtime.restart\","
            " \"value\": \"always\"}");

        g_assert_nonnull(reply);
        g_assert_false(clawt_ipc_frame_is_error(reply));
    }

    g_assert_true(clawt_daemon_reload(fixture.daemon, &error));
    g_assert_no_error(error);

    /* Second start: the runtime survives, so it has to be told again. */
    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "worker");
    g_assert_nonnull(agent);
    g_assert_cmpint(
        clawt_agent_runtime_get_restart_policy(clawt_agent_get_runtime(agent)),
        ==, CLAWT_RESTART_ALWAYS);

    fixture_teardown(&fixture);
}



/*
 * A decision goes in, comes back on the list, and its answer reaches
 * the agent that asked.
 *
 * The last part is what makes this an inbox rather than a suggestion
 * box: without it the operator answers into the void and the agent
 * never learns.
 */
static void
test_a_decision_round_trips_through_the_daemon(void)
{
    Fixture fixture;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    fixture_setup(&fixture,
                  "agents:\n  - id: chief\n    enabled: true\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    /* Nothing yet. */
    {
        g_autoptr(JsonNode) reply = request(&fixture, "decision.list",
                                            "{\"open\": true}");

        g_assert_nonnull(reply);

        if (clawt_ipc_frame_is_error(reply)) {
            g_autofree gchar *line = clawt_ipc_frame_to_line(reply);

            g_error("decision.list refused: %s", line);
        }

        g_assert_nonnull(clawt_ipc_frame_get_payload(reply));
        g_assert_cmpint(
            json_object_get_int_member(clawt_ipc_frame_get_payload(reply),
                                       "open"), ==, 0);
    }

    /* An agent files one, through the same hook the MCP tool uses. */
    {
        g_autoptr(ClawtDecision) decision =
            clawt_decision_new(NULL, "chief", "Take the outage now?");
        ClawtDecisionStore *store =
            clawt_daemon_get_decisions(fixture.daemon);

        g_assert_nonnull(store);
        clawt_decision_set_default(decision, "after the release",
                                   "nothing is broken");
        id = clawt_decision_store_post(store, decision, &error);
        g_assert_no_error(error);
    }

    /* It is on the list, with its default and its derived flags. */
    {
        g_autoptr(JsonNode) reply = request(&fixture, "decision.list", NULL);
        JsonObject *payload = clawt_ipc_frame_get_payload(reply);
        JsonArray *items = json_object_get_array_member(payload,
                                                        "decisions");
        JsonObject *first;

        g_assert_cmpint(json_object_get_int_member(payload, "open"), ==, 1);
        g_assert_cmpuint(json_array_get_length(items), ==, 1);

        first = json_array_get_object_element(items, 0);
        g_assert_cmpstr(json_object_get_string_member(first, "agent"), ==,
                        "chief");
        g_assert_cmpstr(json_object_get_string_member(first, "default"), ==,
                        "after the release");

        /*
         * Derived by the daemon rather than by each client: both are the
         * same rule about the same clock, and two clients deriving them
         * would differ exactly once -- on the item whose deadline had
         * just passed, which is the one that matters.
         */
        g_assert_false(json_object_get_boolean_member(first, "urgent"));
        g_assert_false(json_object_get_boolean_member(first,
                                                      "settled_by_default"));
    }

    /* Answering settles it and reaches the agent. */
    {
        g_autofree gchar *body =
            g_strdup_printf("{\"decision\": \"%s\","
                            " \"answer\": \"neither, do X\"}", id);
        g_autoptr(JsonNode) reply = request(&fixture, "decision.answer",
                                            body);

        g_assert_nonnull(reply);
        g_assert_false(clawt_ipc_frame_is_error(reply));
    }

    {
        ClawtAgent *agent = clawt_agent_manager_get(
            clawt_daemon_get_agents(fixture.daemon), "chief");
        ClawtMailbox *mailbox = clawt_agent_get_mailbox(agent);
        g_autoptr(ClawtMailboxItem) item = NULL;

        g_assert_nonnull(mailbox);

        /*
         * The agent is stopped, so the answer is sitting in its mailbox
         * rather than having been handed over -- which is the case worth
         * asserting, because it is the one that proves the answer is
         * durable rather than dependent on the agent being up when the
         * operator got round to it.
         */
        item = clawt_mailbox_lease(mailbox, 0);
        g_assert_nonnull(item);
        g_assert_nonnull(strstr(clawt_mailbox_item_get_body(item),
                                "neither, do X"));
        g_assert_nonnull(strstr(clawt_mailbox_item_get_body(item),
                                "Take the outage now?"));
    }

    /* And it is no longer open, nor answerable twice. */
    {
        g_autofree gchar *body =
            g_strdup_printf("{\"decision\": \"%s\","
                            " \"answer\": \"left\"}", id);
        g_autoptr(JsonNode) again = request(&fixture, "decision.answer",
                                            body);
        g_autoptr(JsonNode) reply = request(&fixture, "decision.list", NULL);

        g_assert_true(clawt_ipc_frame_is_error(again));
        g_assert_cmpint(
            json_object_get_int_member(clawt_ipc_frame_get_payload(reply),
                                       "open"), ==, 0);
    }

    fixture_teardown(&fixture);
}



/*
 * Settling a decision says so, and says which way.
 *
 * `decision.asked` was published and nothing was published when one was
 * answered or dismissed, so a client's count only ever went up: a
 * second window, the CLI, or the venture bridge answering on the
 * operator's behalf left every other client drawing a badge for an
 * inbox that was already empty.  Nothing warned, because a count that
 * is merely too high looks exactly like work somebody has not got to.
 *
 * Asserted on the bus rather than on a client, because that is where
 * the gap was -- the GTK client already refreshes on any `decision.`
 * event and was correct the moment one arrived.
 */
typedef struct {
    guint  asked;
    guint  settled;
    gchar *how;
    gchar *subject;
} DecisionTrail;

static void
on_decision_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer data)
{
    DecisionTrail *trail = data;
    const gchar *kind = clawt_event_get_kind(event);

    (void)bus;

    if (g_strcmp0(kind, "decision.asked") == 0)
        trail->asked++;

    if (g_strcmp0(kind, "decision.settled") == 0) {
        trail->settled++;
        g_free(trail->how);
        g_free(trail->subject);
        trail->how = g_strdup(clawt_event_get_detail(event, "how"));
        trail->subject = g_strdup(clawt_event_get_subject(event));
    }
}

static gchar *
post_a_decision(Fixture *fixture, const gchar *agent, const gchar *question)
{
    g_autoptr(ClawtDecision) decision = clawt_decision_new(NULL, agent,
                                                           question);
    ClawtDecisionStore *store = clawt_daemon_get_decisions(fixture->daemon);
    g_autoptr(GError) error = NULL;
    gchar *id;

    g_assert_nonnull(store);
    id = clawt_decision_store_post(store, decision, &error);
    g_assert_no_error(error);
    g_assert_nonnull(id);

    return id;
}

static void
test_answering_a_decision_says_so(void)
{
    Fixture fixture;
    DecisionTrail trail = { 0, 0, NULL, NULL };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *body = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n    enabled: true\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_decision_bus_event), &trail);

    id = post_a_decision(&fixture, "chief", "Take the outage now?");

    body = g_strdup_printf("{\"decision\": \"%s\","
                           " \"answer\": \"neither, do X\"}", id);

    {
        g_autoptr(JsonNode) reply = request(&fixture, "decision.answer", body);

        g_assert_nonnull(reply);
        g_assert_false(clawt_ipc_frame_is_error(reply));
    }

    g_assert_cmpuint(trail.settled, ==, 1);
    g_assert_cmpstr(trail.how, ==, "answered");

    /*
     * The agent, matching `decision.asked` -- a client that groups
     * events by who they are about must be able to file both the same
     * way, and a subject of NULL would put the settlement on the fleet.
     */
    g_assert_cmpstr(trail.subject, ==, "chief");

    /* And a second answer, which is refused, publishes nothing more. */
    {
        g_autoptr(JsonNode) again = request(&fixture, "decision.answer", body);

        g_assert_true(clawt_ipc_frame_is_error(again));
    }

    g_assert_cmpuint(trail.settled, ==, 1);

    g_free(trail.how);
    g_free(trail.subject);
    fixture_teardown(&fixture);
}

/*
 * And dismissing one says so too, differently.
 *
 * Two ways to leave the inbox and a client redrawing a list needs to
 * tell them apart; one that only wants to decrement a counter can
 * ignore `how`.
 */
static void
test_dismissing_a_decision_says_so(void)
{
    Fixture fixture;
    DecisionTrail trail = { 0, 0, NULL, NULL };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *body = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n    enabled: true\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_decision_bus_event), &trail);

    id = post_a_decision(&fixture, "chief", "Ship it?");
    body = g_strdup_printf("{\"decision\": \"%s\"}", id);

    {
        g_autoptr(JsonNode) reply = request(&fixture, "decision.dismiss",
                                            body);

        g_assert_nonnull(reply);
        g_assert_false(clawt_ipc_frame_is_error(reply));
    }

    g_assert_cmpuint(trail.settled, ==, 1);
    g_assert_cmpstr(trail.how, ==, "dismissed");
    g_assert_cmpstr(trail.subject, ==, "chief");

    g_free(trail.how);
    g_free(trail.subject);
    fixture_teardown(&fixture);
}

/* ── Autostart ───────────────────────────────────────────────────── */

/*
 * How many of the fleet have left STOPPED.
 */
/*
 * Turns the loop until @wanted agents have started, or time runs out.
 *
 * Counted in wall time rather than in iterations: an agent's computer is
 * started on a worker thread now, so most iterations have nothing ready
 * and a fixed count of non-blocking turns burns through instantly while
 * the thread is still running.  The first draft did exactly that and
 * reported one agent started out of three.
 */
static void
pump_until_started(Fixture *fixture, guint wanted, guint seconds);

static guint
started_count(ClawtDaemon *daemon)
{
    GPtrArray *agents = clawt_agent_manager_list(
        clawt_daemon_get_agents(daemon));
    guint started = 0;
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);

        if (clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_STOPPED)
            started++;
    }

    return started;
}

static void
pump_until_started(Fixture *fixture, guint wanted, guint seconds)
{
    gint64 deadline = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;

    while (started_count(fixture->daemon) < wanted &&
           g_get_monotonic_time() < deadline) {
        if (!g_main_context_iteration(fixture->context, FALSE))
            g_usleep(1000);
    }
}

/*
 * Three autostart agents, and clawt_daemon_start() starts none of them.
 *
 * It used to start all of them, inline, before any main loop existed --
 * so for however long the fleet took to come up the daemon dispatched
 * nothing at all.  On ~30 container agents that is minutes of a process
 * that answers no IPC frame and cannot run its own SIGTERM handler,
 * which from outside is a daemon that will not reply and will not die.
 *
 * The assertion that matters is the middle one: *one* agent per turn of
 * the loop.  A single idle that started the whole fleet in one callback
 * would pass "start does not block" and be the same outage -- the point
 * is that the loop gets to dispatch between agents.
 */
static void
test_autostart_does_not_run_inside_start(void)
{
    Fixture fixture;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *fake =
        g_build_filename(CLAWT_TEST_FIXTURES, "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    guint spun;

    if (!g_file_test(fake, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("the fake libreclaw fixture is not executable");
        return;
    }

    /*
     * The child has to stay up, or an agent that started is
     * indistinguishable from one that never did: the runtime notices a
     * child that exits and puts the agent back to STOPPED, and with the
     * loop now running that happens while the test is still watching.
     *
     * Through the agent's own `env:` block rather than g_setenv(),
     * because the process runtime builds a child environment from an
     * allowlist -- a variable set in the test's own environment reaches
     * nothing.  The first draft did that and read every agent as
     * stopped, which looks exactly like autostart never running.
     */
    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: one\n"
        "    enabled: true\n"
        "    env:\n"
        "      FAKE_LIBRECLAW_SLEEP: \"30\"\n"
        "    runtime:\n"
        "      autostart: true\n"
        "  - id: two\n"
        "    enabled: true\n"
        "    env:\n"
        "      FAKE_LIBRECLAW_SLEEP: \"30\"\n"
        "    runtime:\n"
        "      autostart: true\n"
        "  - id: three\n"
        "    enabled: true\n"
        "    env:\n"
        "      FAKE_LIBRECLAW_SLEEP: \"30\"\n"
        "    runtime:\n"
        "      autostart: true\n",
        fake);

    fixture_setup(&fixture, extra);

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    /* Nothing yet: the loop has not run. */
    g_assert_cmpuint(started_count(fixture.daemon), ==, 0);

    /*
     * They arrive one at a time, so this counts turns rather than
     * asserting a number after a fixed few: each agent costs an idle to
     * reach it and an idle to come back, and pinning the exact ratio
     * would be a test of GTask's scheduling rather than of this.
     */
    pump_until_started(&fixture, 3, 20);

    g_assert_cmpuint(started_count(fixture.daemon), ==, 3);

    /*
     * And it stops asking.  A queue that rescheduled itself for ever
     * would spin the loop for the life of the daemon, at a priority
     * chosen to be polite about exactly that.
     */
    for (spun = 0; spun < 50; spun++)
        g_main_context_iteration(fixture.context, FALSE);

    g_assert_cmpuint(started_count(fixture.daemon), ==, 3);

    fixture_teardown(&fixture);
}

/*
 * Accepts the connection, and then says nothing.
 *
 * Which is what a wedged podman socket looks like, and is the case that
 * turned a slow start into an unkillable process.
 */
typedef struct {
    GSocketListener *listener;
    GCancellable    *cancel;
} MutePodman;

static gpointer
mute_podman(gpointer data)
{
    MutePodman *mute = data;
    GPtrArray *held = g_ptr_array_new_with_free_func(g_object_unref);
    GSocketConnection *conn;

    /*
     * Held, not dropped.  Unreffing each connection closes it, and a
     * socket that hangs up is answered immediately -- which is a
     * different failure from one that goes quiet, and not the one this
     * test is about.  The first draft did exactly that and reported
     * "Connection reset by peer" from a read that was supposed to hang.
     */
    while ((conn = g_socket_listener_accept(mute->listener, NULL,
                                            mute->cancel, NULL)) != NULL)
        g_ptr_array_add(held, conn);

    g_ptr_array_unref(held);

    return NULL;
}

/*
 * Cancelled and *joined*, not left to finish on its own.
 *
 * The thread is parked inside g_socket_listener_accept(), so dropping
 * the last reference to the listener while it is in there hands it a
 * finalised object -- which is invisible in an ordinary build and is
 * `g_socket_accept: assertion 'G_IS_SOCKET (socket)' failed` under ASAN,
 * arriving after the last test has reported ok.
 */
static void
mute_podman_stop(MutePodman *mute, GThread *thread)
{
    g_cancellable_cancel(mute->cancel);
    g_thread_join(thread);
    g_clear_object(&mute->cancel);
}

/*
 * A podman socket that never answers no longer delays start at all.
 *
 * This is the reported failure in its own shape: several container
 * agents, an API socket that accepts and goes quiet, and a
 * clawt_daemon_start() that used to sit inside a blocking read for as
 * long as that took -- which, before podomation grew a socket timeout,
 * was for ever.
 *
 * The context is deliberately never iterated: the fleet is queued and
 * the queue is dropped by the stop in teardown, so this measures start
 * and nothing else.  Iterating would provision an agent against the mute
 * socket and wait out podomation's timeout, which is a different test
 * and a much slower one.
 */
static void
test_start_returns_with_a_podman_socket_that_never_answers(void)
{
    Fixture fixture;
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketListener) listener = NULL;
    g_autoptr(GSocketAddress) addr = NULL;
    GThread *server;
    MutePodman mute;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *sock = NULL;
    g_autofree gchar *extra = NULL;
    g_autofree gchar *module = NULL;
    gint64 began;
    gint64 took;

    /*
     * Skipped rather than passed when the module is missing.
     *
     * Without it the container backend refuses before it opens a socket,
     * so start is fast for a reason that has nothing to do with this
     * fix -- which is how the first draft of this test passed against a
     * deliberately broken daemon.
     */
    module = g_build_filename(CLAWT_TEST_POD_MODULE_DIR,
                              "libpod-module-container.so", NULL);

    if (!g_file_test(module, G_FILE_TEST_IS_REGULAR)) {
        g_test_skip("podomation's container module has not been built");
        return;
    }

    dir = g_dir_make_tmp("clawt-mute-podman-XXXXXX", NULL);
    g_assert_nonnull(dir);
    sock = g_build_filename(dir, "podman.sock", NULL);

    listener = g_socket_listener_new();
    addr = g_unix_socket_address_new(sock);

    g_assert_true(g_socket_listener_add_address(listener, addr,
                                                G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_DEFAULT,
                                                NULL, NULL, NULL));

    mute.listener = listener;
    mute.cancel = g_cancellable_new();
    server = g_thread_new("mute-podman", mute_podman, &mute);

    /*
     * The env override rather than daemon.pod_module_dir, because the
     * fixture's extra YAML is appended inside the `defaults:` block and
     * this key belongs to `daemon:` -- and re-opening a top-level key
     * that is already in the file is how YAML silently discards the
     * first one.
     */
    g_setenv("CLAWT_POD_MODULE_DIR", CLAWT_TEST_POD_MODULE_DIR, TRUE);

    extra = g_strdup_printf(
        "agents:\n"
        "  - id: one\n"
        "    enabled: true\n"
        "    runtime:\n"
        "      autostart: true\n"
        "    computer:\n"
        "      type: container\n"
        "      container:\n"
        "        connection: \"unix://%s\"\n"
        "  - id: two\n"
        "    enabled: true\n"
        "    runtime:\n"
        "      autostart: true\n"
        "    computer:\n"
        "      type: container\n"
        "      container:\n"
        "        connection: \"unix://%s\"\n",
        sock, sock);

    fixture_setup(&fixture, extra);

    began = g_get_monotonic_time();
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    took = g_get_monotonic_time() - began;
    g_assert_no_error(error);

    /*
     * Generous, because this is not a benchmark: the failure it guards
     * against is measured in minutes, or in never.
     */
    g_assert_cmpint(took, <, 5 * G_USEC_PER_SEC);

    fixture_teardown(&fixture);

    mute_podman_stop(&mute, server);
    g_socket_listener_close(listener);
    clawt_test_remove_tree(dir);
    g_unsetenv("CLAWT_POD_MODULE_DIR");
}

/*
 * A source of our own, to prove the loop is still turning.
 */
static gboolean
note_a_tick(gpointer user_data)
{
    guint *ticks = user_data;

    (*ticks)++;

    return G_SOURCE_CONTINUE;
}

/*
 * The loop keeps running while an agent's computer is blocked.
 *
 * This is the reported failure stated as a property, and it is the one
 * an idle source alone does not give you.  Moving autostart out of
 * clawt_daemon_start() fixes *when* the fleet is started and not *where*
 * the waiting happens: a container agent's start is a blocking podman
 * request, so an idle that called it directly still held the loop for
 * the length of that request.  Measured against a socket that accepts
 * and says nothing, that was sixty seconds an agent -- long enough that
 * `agent list` timed out and SIGTERM went unanswered, which is exactly
 * the report, from a version that looked fixed.
 *
 * So the assertion is not about the agent at all.  It is that an
 * unrelated source attached to the same context is still being
 * dispatched while the provision is outstanding.
 */
static void
test_the_loop_runs_while_a_computer_is_provisioning(void)
{
    Fixture fixture;
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketListener) listener = NULL;
    g_autoptr(GSocketAddress) addr = NULL;
    GSource *ticker;
    GThread *server;
    MutePodman mute;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *sock = NULL;
    g_autofree gchar *extra = NULL;
    g_autofree gchar *module = NULL;
    guint ticks = 0;
    gint64 deadline;

    module = g_build_filename(CLAWT_TEST_POD_MODULE_DIR,
                              "libpod-module-container.so", NULL);

    if (!g_file_test(module, G_FILE_TEST_IS_REGULAR)) {
        g_test_skip("podomation's container module has not been built");
        return;
    }

    dir = g_dir_make_tmp("clawt-provision-loop-XXXXXX", NULL);
    g_assert_nonnull(dir);
    sock = g_build_filename(dir, "podman.sock", NULL);

    listener = g_socket_listener_new();
    addr = g_unix_socket_address_new(sock);

    g_assert_true(g_socket_listener_add_address(listener, addr,
                                                G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_DEFAULT,
                                                NULL, NULL, NULL));

    mute.listener = listener;
    mute.cancel = g_cancellable_new();
    server = g_thread_new("mute-podman", mute_podman, &mute);

    g_setenv("CLAWT_POD_MODULE_DIR", CLAWT_TEST_POD_MODULE_DIR, TRUE);

    extra = g_strdup_printf(
        "agents:\n"
        "  - id: stuck\n"
        "    enabled: true\n"
        "    runtime:\n"
        "      autostart: true\n"
        "    computer:\n"
        "      type: container\n"
        "      container:\n"
        "        connection: \"unix://%s\"\n",
        sock);

    fixture_setup(&fixture, extra);

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    ticker = g_timeout_source_new(50);
    g_source_set_callback(ticker, note_a_tick, &ticks, NULL);
    g_source_attach(ticker, fixture.context);

    /*
     * Well short of podomation's own socket timeout, so the provision is
     * certainly still outstanding when this finishes.  Before the wait
     * moved to a worker thread, nothing in here would have run at all.
     */
    deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

    while (g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, TRUE);

    g_assert_cmpuint(ticks, >=, 10);

    /*
     * And the agent is genuinely still stuck, so the ticks above were
     * not counted after it had quietly failed for some other reason --
     * which would pass while proving nothing.
     */
    {
        ClawtAgent *agent = clawt_agent_manager_get(
            clawt_daemon_get_agents(fixture.daemon), "stuck");

        g_assert_nonnull(agent);
        g_assert_cmpint(clawt_agent_get_state(agent), ==,
                        CLAWT_AGENT_STATE_STOPPED);
    }

    g_source_destroy(ticker);
    g_source_unref(ticker);

    fixture_teardown(&fixture);

    mute_podman_stop(&mute, server);
    g_socket_listener_close(listener);
    clawt_test_remove_tree(dir);
    g_unsetenv("CLAWT_POD_MODULE_DIR");
}

/*
 * The loop keeps running while an agent's computer_exec is outstanding.
 *
 * Provisioning was moved to a worker thread and this test's sibling above
 * proves it.  Exec was not, and it is the worse of the two: a provision
 * takes as long as podman takes, while an exec takes as long as the
 * *command* takes, chosen by the agent rather than by clawtilla.  One
 * agent running a build therefore makes the daemon deaf to every other
 * agent, every client, the event stream and its own SIGTERM.
 *
 * Driven over the real socket rather than through clawt_daemon_handle_request(),
 * because the property under test is about the daemon's dispatch and
 * calling the handler directly would block this thread instead of its
 * loop -- and would pass in a build where the bug is present.
 *
 * A host computer, so it needs neither podman nor a module: the wait is
 * the same wait, and `sleep` is the smallest command whose duration is
 * known.  The assertion is not about the exec at all.  It is that an
 * unrelated source attached to the same context is still dispatched
 * while the exec is in flight.
 */
static void
test_the_loop_runs_while_an_exec_is_outstanding(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketClient) raw = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(JsonNode) frame = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *payload = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *wire = NULL;
    GOutputStream *out;
    GSource *ticker;
    guint ticks = 0;
    gint64 deadline;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: worker\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      host:\n"
                  "        confirm_host_control: true\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    /*
     * The computer is attached here rather than by starting the agent.
     * A configured computer is only built when the agent starts, and
     * starting one launches a real child process -- which this test does
     * not need and could not rely on.  What is under test is the
     * daemon's dispatch while an exec is outstanding, so the exec itself
     * is still driven all the way through the real socket and the real
     * handler; only the computer is placed by hand.
     */
    {
        ClawtAgent *agent = clawt_agent_manager_get(
            clawt_daemon_get_agents(fixture.daemon), "worker");
        g_autoptr(ClawtSandbox) sandbox = NULL;
        g_autoptr(ClawtComputer) computer = NULL;

        g_assert_nonnull(agent);

        sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, fixture.dir);
        computer = clawt_host_computer_new("worker", sandbox);
        clawt_agent_set_computer(agent, computer);
    }

    /*
     * tool.rpc authenticates against the agent's own token file, so the
     * test writes one rather than reaching past the check.  Going around
     * the authentication would exercise a path no agent takes.
     */
    state_dir = clawt_config_agent_state_dir(
        clawt_daemon_get_config(fixture.daemon), "worker");
    g_assert_nonnull(state_dir);
    g_assert_cmpint(g_mkdir_with_parents(state_dir, 0700), ==, 0);

    token_path = g_build_filename(state_dir, "token", NULL);
    g_assert_true(g_file_set_contents(token_path, "s3cret", -1, NULL));

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    raw = g_socket_client_new();
    address = g_unix_socket_address_new(socket_path);
    connection = g_socket_client_connect(raw, G_SOCKET_CONNECTABLE(address),
                                         NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);

    /*
     * Two seconds of sleep, and a timeout well above it, so the command
     * is certainly still running for the whole measurement window and
     * the host backend's own deadline never fires.
     */
    payload = g_strdup_printf(
        "{\"agent\": \"worker\", \"token\": \"s3cret\","
        " \"request\": {\"jsonrpc\": \"2.0\", \"id\": 1,"
        " \"method\": \"tools/call\","
        " \"params\": {\"name\": \"clawtilla_computer_exec\","
        " \"arguments\": {\"command\": \"/bin/sleep 2\","
        " \"timeout\": 60}}}}");

    frame = clawt_ipc_request_new("tool.rpc", "wedge-1");
    g_assert_true(json_parser_load_from_data(parser, payload, -1, NULL));
    clawt_ipc_frame_set_payload(frame,
                                json_node_copy(json_parser_get_root(parser)));

    line = clawt_ipc_frame_to_line(frame);
    wire = g_strconcat(line, "\n", NULL);

    out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    g_assert_true(g_output_stream_write_all(out, wire, strlen(wire), NULL,
                                            NULL, &error));
    g_assert_no_error(error);

    ticker = g_timeout_source_new(50);
    g_source_set_callback(ticker, note_a_tick, &ticks, NULL);
    g_source_attach(ticker, fixture.context);

    /*
     * Comfortably inside the sleep, so the exec is still outstanding when
     * this finishes.  While the exec runs on the main context nothing in
     * here is dispatched at all and the count stays at zero.
     */
    deadline = g_get_monotonic_time() + (3 * G_USEC_PER_SEC) / 2;

    while (g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, TRUE);

    g_assert_cmpuint(ticks, >=, 10);

    /*
     * And the exec really ran, so the ticks above were not counted while
     * the request failed instantly for some unrelated reason -- an agent
     * with no computer answers at once, and this test would then pass in
     * a build where nothing was fixed.
     */
    {
        GSocket *socket = g_socket_connection_get_socket(connection);
        gchar buffer[4096];
        GString *reply = g_string_new(NULL);
        gint64 wait_until = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;

        /*
         * Non-blocking, and the loop is iterated between reads.  The
         * answer is produced *by* this daemon, so a blocking read here
         * would wait for a frame that only arrives once the loop it is
         * keeping this thread out of has run.
         */
        g_socket_set_blocking(socket, FALSE);

        while (strchr(reply->str, '\n') == NULL &&
               g_get_monotonic_time() < wait_until) {
            gssize got;

            g_main_context_iteration(fixture.context, FALSE);

            got = g_socket_receive(socket, buffer, sizeof buffer, NULL,
                                   NULL);

            if (got > 0)
                g_string_append_len(reply, buffer, got);
            else
                g_usleep(10 * 1000);
        }

        g_assert_nonnull(strchr(reply->str, '\n'));
        g_assert_null(strstr(reply->str, "no computer"));
        g_assert_nonnull(strstr(reply->str, "\"result\""));

        g_string_free(reply, TRUE);
    }

    g_source_destroy(ticker);
    g_source_unref(ticker);

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    fixture_teardown(&fixture);
}


/* ── The operator's own computer.exec ────────────────────────────── */

/*
 * Places a real host computer on an agent without starting it.
 *
 * A configured computer is only built at agent start, and starting one
 * launches a real child; what these tests are about is the daemon's
 * dispatch around an exec, so the exec is driven all the way through the
 * real socket and the real handler and only the computer is placed by
 * hand.
 */
static void
attach_a_host_computer(Fixture *fixture, const gchar *agent_id,
                       const gchar *root)
{
    ClawtAgent *agent = clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture->daemon), agent_id);
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;

    g_assert_nonnull(agent);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, root);
    computer = clawt_host_computer_new(agent_id, sandbox);
    clawt_agent_set_computer(agent, computer);
}

/* Opens a client connection to the fixture's daemon socket. */
static GSocketConnection *
dial_the_daemon(Fixture *fixture)
{
    g_autoptr(GSocketClient) raw = g_socket_client_new();
    g_autofree gchar *socket_path =
        g_build_filename(fixture->dir, "daemon.sock", NULL);
    g_autoptr(GSocketAddress) address =
        g_unix_socket_address_new(socket_path);
    g_autoptr(GError) error = NULL;
    GSocketConnection *connection;

    connection = g_socket_client_connect(raw, G_SOCKET_CONNECTABLE(address),
                                         NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);

    return connection;
}

/* Sends one frame down a connection opened with dial_the_daemon(). */
static void
send_a_frame(GSocketConnection *connection, const gchar *kind,
             const gchar *id, const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, id);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *wire = NULL;
    GOutputStream *out;

    g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
    clawt_ipc_frame_set_payload(frame,
                                json_node_copy(json_parser_get_root(parser)));

    line = clawt_ipc_frame_to_line(frame);
    wire = g_strconcat(line, "\n", NULL);

    out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    g_assert_true(g_output_stream_write_all(out, wire, strlen(wire), NULL,
                                            NULL, &error));
    g_assert_no_error(error);
}

/*
 * Reads one line back, iterating the daemon's loop while it waits.
 *
 * The answer is produced *by* this daemon, so a blocking read would wait
 * for a frame that only arrives once the loop it is keeping this thread
 * out of has run.
 */
static gchar *
read_a_frame(Fixture *fixture, GSocketConnection *connection, guint seconds)
{
    GSocket *socket = g_socket_connection_get_socket(connection);
    GString *reply = g_string_new(NULL);
    gint64 wait_until = g_get_monotonic_time() +
                        (gint64)seconds * G_USEC_PER_SEC;
    gchar buffer[4096];

    g_socket_set_blocking(socket, FALSE);

    while (strchr(reply->str, '\n') == NULL &&
           g_get_monotonic_time() < wait_until) {
        gssize got;

        g_main_context_iteration(fixture->context, FALSE);

        got = g_socket_receive(socket, buffer, sizeof buffer, NULL, NULL);

        if (got > 0)
            g_string_append_len(reply, buffer, got);
        else
            g_usleep(10 * 1000);
    }

    g_assert_nonnull(strchr(reply->str, '\n'));

    return g_string_free(reply, FALSE);
}

/*
 * The loop keeps running while the *operator's* exec is outstanding.
 *
 * The tool path was moved off the main context first, and this is the
 * same defect reached from a different caller: an IPC handler runs on the
 * daemon's main context and answered synchronously, so a command from the
 * CLI blocked every other agent's messages, task delivery and timers for
 * as long as it took -- up to the advertised 120 second default.
 *
 * It is arguably the worse of the two, because a person at a terminal is
 * the caller most likely to run something long on purpose, and a whole
 * fleet appearing to hang while a command they can see running is still
 * going reads as the fleet being broken.
 *
 * Phrased as a property of an *unrelated* timer, deliberately.  A test
 * asserting anything about the exec itself cannot tell the two versions
 * apart -- the answer arrives either way.
 */
static void
test_the_loop_runs_while_an_operator_exec_is_outstanding(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autofree gchar *reply = NULL;
    GSource *ticker;
    guint ticks = 0;
    gint64 deadline;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: worker\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      host:\n"
                  "        confirm_host_control: true\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    attach_a_host_computer(&fixture, "worker", fixture.dir);

    connection = dial_the_daemon(&fixture);

    /*
     * Two seconds of sleep and a timeout well above it, so the command is
     * certainly still running for the whole measurement window and the
     * host backend's own deadline never fires.
     */
    send_a_frame(connection, "computer.exec", "op-1",
                 "{\"agent\": \"worker\", \"command\": \"/bin/sleep 2\","
                 " \"timeout\": 60}");

    ticker = g_timeout_source_new(50);
    g_source_set_callback(ticker, note_a_tick, &ticks, NULL);
    g_source_attach(ticker, fixture.context);

    deadline = g_get_monotonic_time() + (3 * G_USEC_PER_SEC) / 2;

    while (g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, TRUE);

    g_assert_cmpuint(ticks, >=, 10);

    /*
     * And the command really ran.  An agent with no computer is refused
     * at once, and this test would then pass in a build where nothing had
     * been fixed.
     */
    reply = read_a_frame(&fixture, connection, 30);

    g_assert_null(strstr(reply, "no computer"));
    g_assert_nonnull(strstr(reply, "\"ok\":true"));
    g_assert_nonnull(strstr(reply, "\"exit\":0"));

    g_source_destroy(ticker);
    g_source_unref(ticker);

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    fixture_teardown(&fixture);
}

typedef struct {
    guint  count;
    gchar *command;
    gint64 exit_status;
    gint64 at;              /* when the record was written */
} ExecTrail;

static void
on_exec_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ExecTrail *trail = user_data;

    (void)bus;

    if (g_strcmp0(clawt_event_get_kind(event), "computer.exec") != 0)
        return;

    g_free(trail->command);
    trail->command = g_strdup(clawt_event_get_detail(event, "command"));
    trail->exit_status = clawt_event_get_detail_int(event, "exit");
    trail->at = g_get_monotonic_time();
    trail->count++;
}

/*
 * The audit line survives the move off the main context.
 *
 * Running something on the machine is the most consequential thing this
 * socket can do, so every command is recorded whoever asked for it.  The
 * record used to be written on the line after the blocking call; moving
 * the wait to a worker moves the moment the command ended with it, and a
 * trail written when the request *arrived* would report an exit status
 * nothing had produced yet.
 *
 * Once, and after the command has actually finished -- both, because a
 * record written twice and a record written early are different bugs with
 * the same fix.
 */
static void
test_an_operator_exec_is_recorded_once_when_it_ends(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autofree gchar *reply = NULL;
    ExecTrail trail = { 0 };
    gint64 sent_at;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: worker\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      host:\n"
                  "        confirm_host_control: true\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    attach_a_host_computer(&fixture, "worker", fixture.dir);

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_exec_bus_event), &trail);

    connection = dial_the_daemon(&fixture);

    sent_at = g_get_monotonic_time();
    send_a_frame(connection, "computer.exec", "op-2",
                 "{\"agent\": \"worker\", \"command\": \"/bin/sleep 1\","
                 " \"timeout\": 60}");

    reply = read_a_frame(&fixture, connection, 30);
    g_assert_nonnull(strstr(reply, "\"exit\":0"));

    /*
     * The reply and the record are produced by the same callback, so
     * there is nothing left to wait for -- but the loop is turned once
     * more so a second record, if one were written, would have arrived
     * before the count is read.
     */
    g_main_context_iteration(fixture.context, FALSE);

    g_assert_cmpuint(trail.count, ==, 1);
    g_assert_cmpstr(trail.command, ==, "/bin/sleep 1");
    g_assert_cmpint(trail.exit_status, ==, 0);
    g_assert_cmpint(trail.at - sent_at, >=, G_USEC_PER_SEC);

    g_free(trail.command);

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    fixture_teardown(&fixture);
}

/*
 * The IPC verb refuses a command line that needs a shell, and only when
 * the caller sent a string.
 *
 * A rule enforced at one call site is a rule about that call site.  The
 * agent-facing tool and this verb both lex a command line and spawn it
 * directly, so both had the same silent failure -- exit 0, the rest of
 * the line echoed back, nothing logged.
 *
 * An `argv` caller is deliberately exempt: the CLI already has the
 * arguments separated by the shell that split them, so it means every
 * one of them literally, and `find . -exec rm {} ';'` is a legitimate
 * argv.  Refusing that would break a client for saying exactly what it
 * meant.
 */
static void
test_the_exec_verb_refuses_a_shell_line(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autofree gchar *refused = NULL;
    g_autofree gchar *accepted = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: worker\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      host:\n"
                  "        confirm_host_control: true\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    attach_a_host_computer(&fixture, "worker", fixture.dir);
    connection = dial_the_daemon(&fixture);

    send_a_frame(connection, "computer.exec", "op-shell",
                 "{\"agent\": \"worker\", "
                 "\"command\": \"echo one; echo two\", \"timeout\": 30}");

    refused = read_a_frame(&fixture, connection, 30);
    g_assert_nonnull(strstr(refused, "\"ok\":false"));
    g_assert_nonnull(strstr(refused, "bash -c"));

    /*
     * And the same operators, sent as an argv, are the caller's own
     * words and run.
     */
    send_a_frame(connection, "computer.exec", "op-argv",
                 "{\"agent\": \"worker\", "
                 "\"argv\": [\"/bin/echo\", \"one;\", \"two\"], "
                 "\"timeout\": 30}");

    accepted = read_a_frame(&fixture, connection, 30);
    g_assert_nonnull(strstr(accepted, "\"ok\":true"));

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    fixture_teardown(&fixture);
}

/*
 * A command that could not be run at all is still recorded.
 *
 * A trail holding only the successes answers the wrong question: the
 * command somebody looks up afterwards is precisely the one that did not
 * work.  An exit of -1 reads as "we do not know what it did", where an
 * absent entry reads as "it did not happen".
 */
static void
test_an_operator_exec_that_is_refused_is_still_recorded(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autofree gchar *reply = NULL;
    g_autofree gchar *outside = NULL;
    g_autofree gchar *command = NULL;
    ExecTrail trail = { 0 };

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: worker\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      host:\n"
                  "        confirm_host_control: true\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    attach_a_host_computer(&fixture, "worker", fixture.dir);

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_exec_bus_event), &trail);

    connection = dial_the_daemon(&fixture);

    /*
     * Refused by the sandbox before anything is spawned, which is the
     * shape of failure that produces no exec result at all -- as opposed
     * to a command that ran and failed, which has one.
     */
    outside = g_build_filename(fixture.dir, "..", "elsewhere", NULL);
    command = g_strdup_printf("{\"agent\": \"worker\", \"argv\":"
                              " [\"/bin/cat\", \"%s\"], \"timeout\": 60}",
                              outside);

    send_a_frame(connection, "computer.exec", "op-3", command);

    reply = read_a_frame(&fixture, connection, 30);
    g_assert_nonnull(strstr(reply, "\"ok\":false"));

    g_main_context_iteration(fixture.context, FALSE);

    g_assert_cmpuint(trail.count, ==, 1);
    g_assert_cmpint(trail.exit_status, ==, -1);

    /*
     * An argv caller has no command string, so the trail records what the
     * command line would have been.  A record saying a command ran and
     * not which one answers the wrong question.
     */
    g_assert_nonnull(trail.command);
    g_assert_true(g_str_has_prefix(trail.command, "/bin/cat "));

    g_free(trail.command);

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    fixture_teardown(&fixture);
}

/* ── What the persona costs ──────────────────────────────────────── */

typedef struct {
    guint  count;
    gchar *verdict;
    gchar *over;
    gint64 bytes;
} IdentityWatch;

static void
on_identity_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer data)
{
    IdentityWatch *watch = data;

    (void)bus;

    if (g_strcmp0(clawt_event_get_kind(event), "agent.identity") != 0)
        return;

    g_free(watch->verdict);
    g_free(watch->over);
    watch->verdict = g_strdup(clawt_event_get_detail(event, "verdict"));
    watch->over = g_strdup(clawt_event_get_detail(event, "over"));
    watch->bytes = clawt_event_get_detail_int(event, "bytes");
    watch->count++;
}

/* Fills an agent's SOUL.org so the assembled prompt lands near @total. */
static void
grow_the_persona(Fixture *fixture, const gchar *agent_id, gsize total)
{
    ClawtAgentConfig *config = clawt_config_get_agent(
        clawt_daemon_get_config(fixture->daemon), agent_id);
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *filler = NULL;

    g_assert_nonnull(config);

    workspace = clawt_agent_config_get_workspace(config);
    g_assert_nonnull(workspace);
    g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);

    path = g_build_filename(workspace, "SOUL.org", NULL);
    filler = g_strnfill(total, 'x');
    g_assert_true(g_file_set_contents(path, filler, (gssize)total, NULL));
}

/*
 * Starting an agent whose persona has outgrown the limit says so, on the
 * bus, before anything tries to spawn with it.
 *
 * The failure this warns about is silent right up to the cliff and then
 * arrives as "Argument list too long", which names neither the files, the
 * size, nor the limit -- and because a *resumed* session is never handed
 * a system prompt, it arrives long after the paragraph that caused it.
 *
 * On the bus rather than only in the journal, so it reaches both clients'
 * alert panels: a warning nobody reads is the same as no warning, and the
 * whole point here is to be seen before something breaks.
 */
static void
test_an_oversized_persona_is_announced_at_start(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    IdentityWatch watch = { 0 };
    GLogLevelFlags fatal;

    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    persona:\n"
        "      identity_files: [SOUL.org]\n"
        "    computer:\n"
        "      type: none\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    grow_the_persona(&fixture, "worker", CLAWT_ARG_LIMIT + 1024);

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_identity_bus_event), &watch);

    /*
     * The warning is deliberate, so GTest is told not to abort on it.
     * g_test_expect_message() is the wrong tool: with an expectation
     * pending every *other* message the daemon logs becomes fatal in
     * turn, and starting an agent logs several innocuous ones.
     */
    fatal = g_log_set_always_fatal(0);
    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_log_set_always_fatal(fatal);
    g_assert_no_error(error);

    g_assert_cmpuint(watch.count, ==, 1);
    g_assert_cmpstr(watch.over, ==, "true");
    g_assert_cmpint(watch.bytes, >, CLAWT_ARG_LIMIT);
    g_assert_nonnull(watch.verdict);
    g_assert_nonnull(strstr(watch.verdict, "SOUL.org"));

    /*
     * And it is a NOTICE, so both clients' alert panels draw it rather
     * than filing it with the routine stream nothing counts.
     */
    {
        g_autoptr(ClawtEvent) event = clawt_event_new("agent.identity",
                                                      "worker");

        g_assert_cmpint(clawt_alert_tier_for_event(event), ==,
                        CLAWT_ALERT_NOTICE);
    }

    /* Started, not refused: the default backend spills the prompt to a
     * file and has no ceiling, so refusing would stop an agent that
     * works. */
    g_assert_cmpint(clawt_agent_get_state(
                        clawt_agent_manager_get(
                            clawt_daemon_get_agents(fixture.daemon),
                            "worker")),
                    !=, CLAWT_AGENT_STATE_SHADOW);

    clawt_daemon_stop_agent(fixture.daemon, "worker", FALSE);

    g_free(watch.verdict);
    g_free(watch.over);

    fixture_teardown(&fixture);
}

/*
 * And an ordinary agent says nothing at all.
 *
 * A measurement reported on every start is one nobody reads on the agent
 * it matters for -- so the threshold is the feature, and a test that only
 * covered the loud case would pass against a build that shouted about
 * every agent in the fleet.
 */
static void
test_an_ordinary_persona_is_not_announced(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    IdentityWatch watch = { 0 };

    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    persona:\n"
        "      identity_files: [SOUL.org]\n"
        "    computer:\n"
        "      type: none\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    grow_the_persona(&fixture, "worker", 8192);

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_identity_bus_event), &watch);

    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    g_assert_cmpuint(watch.count, ==, 0);

    clawt_daemon_stop_agent(fixture.daemon, "worker", FALSE);
    fixture_teardown(&fixture);
}

/*
 * agent.show carries the numbers whatever the size, and the sentence only
 * when there is one.
 *
 * Always reported so a client never has to decide whether to ask -- the
 * GTK inspector and the web summary both draw the size on every agent,
 * and both draw the warning only when `verdict` is present.  A client
 * that had to guess would be a second copy of the threshold.
 */
static void
test_agent_show_carries_the_identity_size(void)
{
    Fixture fixture;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: worker\n"
                  "    persona:\n"
                  "      identity_files: [SOUL.org]\n");

    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    grow_the_persona(&fixture, "worker", 4096);

    {
        g_autoptr(JsonNode) reply = request(&fixture, "agent.show",
                                            "{\"agent\": \"worker\"}");
        JsonObject *identity;

        g_assert_nonnull(reply);
        g_assert_false(clawt_ipc_frame_is_error(reply));

        identity = json_object_get_object_member(payload_of(reply),
                                                 "identity");
        g_assert_nonnull(identity);
        g_assert_cmpint(json_object_get_int_member(identity, "limit"), ==,
                        CLAWT_ARG_LIMIT);
        g_assert_cmpint(json_object_get_int_member(identity, "bytes"), >,
                        4096);
        g_assert_false(json_object_has_member(identity, "verdict"));

        /* The breakdown, so a client can name the file to shorten. */
        {
            JsonArray *files = json_object_get_array_member(identity,
                                                            "files");
            JsonObject *first = json_array_get_object_element(files, 0);

            g_assert_cmpuint(json_array_get_length(files), ==, 1);
            g_assert_cmpstr(json_object_get_string_member(first, "name"), ==,
                            "SOUL.org");
            g_assert_true(json_object_get_boolean_member(first, "present"));
        }
    }

    grow_the_persona(&fixture, "worker", CLAWT_ARG_LIMIT);

    {
        g_autoptr(JsonNode) reply = request(&fixture, "agent.show",
                                            "{\"agent\": \"worker\"}");
        JsonObject *identity =
            json_object_get_object_member(payload_of(reply),
                                          "identity");

        g_assert_nonnull(identity);
        g_assert_true(json_object_has_member(identity, "verdict"));
        g_assert_nonnull(strstr(
            json_object_get_string_member(identity, "verdict"),
            "SOUL.org"));
    }

    fixture_teardown(&fixture);
}

/* ── Task events ─────────────────────────────────────────────────── */

typedef struct {
    gchar *subject;
    gchar *state;
    gchar *agent;
    guint  count;
} TaskWatch;

static void
on_task_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    TaskWatch *watch = user_data;

    (void)bus;

    if (!g_str_has_prefix(clawt_event_get_kind(event), "task."))
        return;

    g_free(watch->subject);
    g_free(watch->state);
    g_free(watch->agent);

    watch->subject = g_strdup(clawt_event_get_subject(event));
    watch->state = g_strdup(clawt_event_get_detail(event, "state"));
    watch->agent = g_strdup(clawt_event_get_detail(event, "agent"));
    watch->count++;
}

/*
 * Every state a task passes through reaches the bus.
 *
 * ClawtTaskManager::task-changed has been emitted since the manager was
 * written and the daemon has connected to it for nearly as long -- to
 * decide on a notification, and to do nothing else. So no `task.` kind
 * was ever published: a client following a task had to poll
 * `task.list`, and podomation's `on_task_changed` binding, which is
 * declared, documented and mapped to `task.changed`, named an event
 * that could not fire.
 *
 * Asserted on the *whole* run rather than on the completion. A version
 * that published only the interesting state would pass a test that
 * watched the end and would still leave a client unable to tell a task
 * that had started from one that was queued.
 */
static void
test_a_task_change_reaches_the_bus(void)
{
    Fixture fixture = { 0 };
    TaskWatch watch = { 0 };
    ClawtTaskManager *tasks;
    ClawtTask *task;
    g_autofree gchar *task_id = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n  - id: scribe\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
                     G_CALLBACK(on_task_bus_event), &watch);

    tasks = clawt_daemon_get_tasks(fixture.daemon);
    task = clawt_task_manager_create(tasks, "chief", "scribe",
                                     "summarise yesterday", NULL, NULL);
    g_assert_nonnull(task);

    task_id = g_strdup(clawt_task_get_id(task));

    /* Created is a change too, and it is the one a client has nothing to
     * poll for yet -- the task did not exist a moment ago. */
    g_assert_cmpuint(watch.count, ==, 1);
    g_assert_cmpstr(watch.subject, ==, task_id);
    g_assert_cmpstr(watch.state, ==, "pending");

    /*
     * The subject is the task, which is what `event.list` filters on,
     * and the assignee travels as a detail -- the pod module reads it to
     * decide whether a scoped pod hears the event at all.
     */
    g_assert_cmpstr(watch.agent, ==, "scribe");

    g_assert_true(clawt_task_manager_start(tasks, task_id));
    g_assert_cmpuint(watch.count, ==, 2);
    g_assert_cmpstr(watch.state, ==, "running");

    g_assert_true(clawt_task_manager_complete(tasks, task_id, "done"));
    g_assert_cmpuint(watch.count, ==, 3);
    g_assert_cmpstr(watch.state, ==, "completed");
    g_assert_cmpstr(watch.subject, ==, task_id);

    g_free(watch.subject);
    g_free(watch.state);
    g_free(watch.agent);
    fixture_teardown(&fixture);
}

/* ── agent.set ───────────────────────────────────────────────────── */

/*
 * Correcting the key an agent was shadowed for un-shadows it, with no
 * restart.
 *
 * The shadow decision was taken once, when the config was parsed.  So
 * `agent set computer.host.confirm_host_control true` wrote the value,
 * answered with the key and its new value, and left the agent disabled
 * with the old reason -- `agent list` still saying `shadow`, which reads
 * as the setting having been ignored.  The only remedy was restarting
 * the daemon, and against a remote one there was no way to ask for even
 * that.
 *
 * Both halves are needed, and the test would pass with either one
 * missing if it checked only the reply: ClawtAgentConfig carries the
 * reason and ClawtAgent carries the state a client is shown, and
 * clawt_agent_set_config() returns early when handed the object it
 * already holds -- which is exactly what `agent set` edits.  So the
 * agent.show assertion is the load-bearing one.
 */
static void
test_correcting_a_shadow_key_clears_the_shadow(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) before = NULL;
    g_autoptr(JsonNode) set = NULL;
    g_autoptr(JsonNode) after = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: bold\n"
                  "    computer:\n"
                  "      type: host\n");
    /*
     * Starting logs the refusal, which is the point of the fixture -- and
     * GTest makes a warning fatal, so it is swallowed rather than left to
     * kill the test that provoked it deliberately.
     */
    {
        GLogLevelFlags was_fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK);
        guint handler = g_log_set_handler("Clawtilla",
                                          G_LOG_LEVEL_WARNING |
                                          G_LOG_FLAG_FATAL |
                                          G_LOG_FLAG_RECURSION,
                                          swallow_warnings, NULL);

        g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

        g_log_remove_handler("Clawtilla", handler);
        g_log_set_always_fatal(was_fatal);
    }

    before = request(&fixture, "agent.show", "{\"agent\":\"bold\"}");
    g_assert_cmpstr(
        json_object_get_string_member(
            json_object_get_object_member(payload_of(before), "agent"),
            "state"), ==, "shadow");

    set = request(&fixture, "agent.set",
                  "{\"agent\":\"bold\","
                  "\"key\":\"computer.host.confirm_host_control\","
                  "\"value\":\"true\"}");
    g_assert_false(clawt_ipc_frame_is_error(set));

    /* The reply says so, so a client need not ask again to find out. */
    g_assert_false(json_object_get_boolean_member(payload_of(set),
                                                  "shadow"));

    after = request(&fixture, "agent.show", "{\"agent\":\"bold\"}");
    g_assert_cmpstr(
        json_object_get_string_member(
            json_object_get_object_member(payload_of(after), "agent"),
            "state"), ==, "stopped");

    fixture_teardown(&fixture);
}

/*
 * And back: a setting that makes an agent unusable shadows it again.
 *
 * Without this the test above passes against a build that simply stopped
 * shadowing host agents at all.
 */
static void
test_breaking_a_key_shadows_the_agent_again(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) set = NULL;
    g_autoptr(JsonNode) after = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: bold\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      host:\n"
                  "        confirm_host_control: true\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    set = request(&fixture, "agent.set",
                  "{\"agent\":\"bold\","
                  "\"key\":\"computer.host.confirm_host_control\","
                  "\"value\":\"false\"}");
    g_assert_false(clawt_ipc_frame_is_error(set));
    g_assert_true(json_object_get_boolean_member(payload_of(set), "shadow"));
    g_assert_nonnull(json_object_get_string_member(payload_of(set),
                                                   "shadow_reason"));

    after = request(&fixture, "agent.show", "{\"agent\":\"bold\"}");
    g_assert_cmpstr(
        json_object_get_string_member(
            json_object_get_object_member(payload_of(after), "agent"),
            "state"), ==, "shadow");

    fixture_teardown(&fixture);
}

/*
 * A value the enum has never heard of is refused, naming what is
 * allowed.
 *
 * It used to be accepted: written to clawtilla.yaml, echoed back as
 * saved, and then read as the schema default by everything that asked
 * -- so the agent went on with the computer it already had while the
 * file said otherwise. The bill arrived at the *next* daemon load, where
 * the validator turns an agent with an unknown computer type into a
 * shadow, a long way from the command that caused it.
 *
 * The reply is checked for the allowed values as well as for the
 * refusal. A refusal that only says no gets retried in a different
 * shape, and the whole reason somebody typed a wrong nickname is that
 * they did not know the right one.
 */
static void
test_agent_set_refuses_a_value_the_enum_lacks(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) accepted = NULL;
    g_autoptr(JsonNode) shown = NULL;
    const gchar *message;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    refused = request(&fixture, "agent.set",
                      "{\"agent\":\"chief\",\"key\":\"computer.type\","
                      "\"value\":\"teleporter\"}");

    g_assert_true(clawt_ipc_frame_is_error(refused));

    message = json_object_get_string_member(json_node_get_object(refused),
                                            "error");
    g_assert_nonnull(message);
    g_assert_nonnull(strstr(message, "teleporter"));
    g_assert_nonnull(strstr(message, "computer.type"));

    /*
     * Read off the enum's own GType, so this is the list the loader
     * will accept rather than a sentence written beside it.
     */
    g_assert_nonnull(strstr(message, "container"));
    g_assert_nonnull(strstr(message, "vm"));

    /* And nothing was written: the setting the agent reports is still
     * the one it had. */
    shown = request(&fixture, "agent.show", "{\"agent\":\"chief\"}");
    g_assert_cmpstr(
        json_object_get_string_member(
            json_object_get_object_member(payload_of(shown), "settings"),
            "computer.type"),
        ==, "none");

    /* A value the enum does have still goes through, so the check is a
     * check and not a wall. */
    accepted = request(&fixture, "agent.set",
                       "{\"agent\":\"chief\",\"key\":\"computer.type\","
                       "\"value\":\"container\"}");
    g_assert_false(clawt_ipc_frame_is_error(accepted));

    fixture_teardown(&fixture);
}

/*
 * Puts one agent into %CLAWT_AGENT_STATE_RUNNING and returns the
 * connection holding it there.
 *
 * There is no shortcut. A started agent is *starting* -- the process
 * being up is not the same as being reachable -- and only a link
 * arriving makes it running, which is exactly the distinction
 * `restart_required` turns on. So the fixture dials the daemon's own
 * agent socket with the token the render wrote, which is what a real
 * libreclaw does.
 *
 * Returns: (transfer full) (nullable): the connection, to be closed by
 *   the caller; %NULL if the fake libreclaw is not there to be run
 */
static GSocketConnection *
run_one_agent(Fixture *fixture, const gchar *agent_id)
{
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *hello = NULL;
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GError) error = NULL;
    GSocketConnection *connection;
    ClawtAgent *agent;
    guint spun;

    g_assert_true(clawt_daemon_start_agent(fixture->daemon, agent_id,
                                           &error));
    g_assert_no_error(error);

    state_dir = clawt_config_agent_state_dir(
        clawt_daemon_get_config(fixture->daemon), agent_id);
    token_path = g_build_filename(state_dir, "token", NULL);

    g_assert_true(g_file_get_contents(token_path, &token, NULL, NULL));
    g_strstrip(token);

    socket_path = g_build_filename(fixture->dir, "state", "agents.sock",
                                   NULL);
    address = g_unix_socket_address_new(socket_path);

    connection = g_socket_client_connect(client,
                                         G_SOCKET_CONNECTABLE(address),
                                         NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);

    hello = g_strdup_printf(
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":"
        "{\"agent_id\":\"%s\",\"token\":\"%s\"}}\n", agent_id, token);

    g_assert_true(g_output_stream_write_all(
        g_io_stream_get_output_stream(G_IO_STREAM(connection)),
        hello, strlen(hello), NULL, NULL, &error));
    g_assert_no_error(error);

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture->daemon),
                                    agent_id);
    g_assert_nonnull(agent);

    /*
     * Pumped against a bounded number of turns rather than a sleep: the
     * hello has to be read, authenticated and answered on the daemon's
     * own context, all of which is this loop's work to do.
     */
    for (spun = 0; spun < 500; spun++) {
        if (clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_RUNNING)
            break;

        g_main_context_iteration(fixture->context, FALSE);
        g_usleep(1000);
    }

    g_assert_cmpint(clawt_agent_get_state(agent), ==,
                    CLAWT_AGENT_STATE_RUNNING);

    return connection;
}

/*
 * Granting a running agent the chief's role says a restart is needed.
 *
 * `chief_of_staff` and `team_role` are the two halves of one condition
 * in clawt_mcp_tools_is_permitted(): between them they decide whether
 * the delegation tool is offered at all, exactly as `tools.manage_fleet`
 * does. Only the `tools.` and `persona.` prefixes were reported, so
 * making an agent the chief while it ran changed its gate, its files and
 * its TOOLS.org, kept a session that had already listed its tools, and
 * said nothing about it -- and the answer to the bug report was an
 * instruction to flip a switch that was flipped before it was read.
 *
 * `name` is checked in the same run, because a rule that reported
 * everything would be the same as one that reported nothing: somebody
 * told to restart after every edit stops reading the line.
 */
static void
test_agent_set_reports_a_role_needs_a_new_session(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *fake = g_build_filename(CLAWT_TEST_FIXTURES,
                                              "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GSocketConnection) link = NULL;
    g_autoptr(JsonNode) chief = NULL;
    g_autoptr(JsonNode) role = NULL;
    g_autoptr(JsonNode) name = NULL;

    if (!g_file_test(fake, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("the fake libreclaw fixture is not executable");
        return;
    }

    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: chief\n"
        "    enabled: true\n"
        "    env:\n"
        "      FAKE_LIBRECLAW_SLEEP: \"30\"\n"
        "    computer:\n"
        "      type: none\n",
        fake);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    link = run_one_agent(&fixture, "chief");

    chief = request(&fixture, "agent.set",
                    "{\"agent\":\"chief\",\"key\":\"chief_of_staff\","
                    "\"value\":\"true\"}");
    g_assert_false(clawt_ipc_frame_is_error(chief));
    g_assert_true(json_object_get_boolean_member(payload_of(chief),
                                                 "restart_required"));

    role = request(&fixture, "agent.set",
                   "{\"agent\":\"chief\",\"key\":\"team_role\","
                   "\"value\":\"lead\"}");
    g_assert_false(clawt_ipc_frame_is_error(role));
    g_assert_true(json_object_get_boolean_member(payload_of(role),
                                                 "restart_required"));

    name = request(&fixture, "agent.set",
                   "{\"agent\":\"chief\",\"key\":\"name\","
                   "\"value\":\"Chief\"}");
    g_assert_false(clawt_ipc_frame_is_error(name));
    g_assert_false(json_object_get_boolean_member(payload_of(name),
                                                  "restart_required"));

    g_io_stream_close(G_IO_STREAM(link), NULL, NULL);
    fixture_teardown(&fixture);
}

/*
 * Starting an agent with no id is an error, not a critical.
 *
 * The id reaches this from an IPC payload, an agent's config or a pod
 * file, none of which are code -- and it was guarded by
 * g_return_val_if_fail(), which prints a stack trace and returns FALSE
 * with @error untouched. So the caller had a failure with nothing in it
 * to report: the pod action path warned "it did not work" above a GLib
 * critical about an assertion nobody had written, which sends whoever
 * reads it looking for a bug in the daemon rather than for the missing
 * word in their pod.
 *
 * The refusal has to name the *argument*. A generic failure is what was
 * there before.
 */
static void
test_starting_an_agent_with_no_id_is_an_error(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    g_assert_false(clawt_daemon_start_agent(fixture.daemon, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_nonnull(strstr(error->message, "id"));

    fixture_teardown(&fixture);
}

/*
 * A second `agent start` reaches the agent's computer.
 *
 * start_agent_prepare() assigned its out-parameter inside the branch that
 * *builds* a computer, so from an agent's first start until the daemon
 * exited, clawt_computer_start() was unreachable for it. Destroy the
 * container behind the daemon's back and neither `agent start` nor
 * `agent restart` could bring it back -- both reported success having
 * asked nothing, because relaunching the child genuinely does succeed:
 * the libreclaw process runs on the host and does not need the container
 * to exist.
 *
 * Stated here without a container, because the defect is not the
 * container's. The state is put back to ABSENT by hand to stand for
 * whatever the backend lost -- what is under test is that the *daemon*
 * asks the computer to start again, and the assertion is on the real
 * start path having run.
 */
static void
test_a_second_start_reaches_the_computer(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgent *agent;
    ClawtComputer *computer;

    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "worker");
    g_assert_nonnull(agent);

    computer = clawt_agent_get_computer(agent);
    g_assert_nonnull(computer);

    /*
     * The first start really did start it, so the second assertion below
     * is measuring a recovery rather than a state that was never left.
     */
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_RUNNING);

    clawt_daemon_stop_agent(fixture.daemon, "worker", FALSE);

    /* Whatever the backend was holding is gone. */
    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ABSENT, NULL);

    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    /*
     * The same computer object, so this is the agent's own computer being
     * started again rather than a second one quietly built beside it.
     */
    g_assert_true(clawt_agent_get_computer(agent) == computer);
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_RUNNING);

    fixture_teardown(&fixture);
}


/*
 * What one probe found, and a flag saying it has finished.
 *
 * The probe blocks until the far end answers, and the far end here is
 * this test's own daemon whose loop is this thread -- so it runs on a
 * thread while the loop keeps being iterated. A real client has the same
 * problem for the same reason, which is why the probe's documentation
 * says not to call it on the loop.
 */
typedef struct {
    ClawtConnection       *connection;
    ClawtConnectionStatus *status;
    gint                   finished;
} ProbeRun;

static gpointer
probe_worker(gpointer data)
{
    ProbeRun *run = data;

    run->status = clawt_connection_probe(run->connection);
    g_atomic_int_set(&run->finished, 1);

    return NULL;
}

static ClawtConnectionStatus *
probe_while_the_loop_runs(Fixture *fixture, ClawtConnection *connection)
{
    ProbeRun run = { connection, NULL, 0 };
    GThread *thread = g_thread_new("probe", probe_worker, &run);
    gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

    while (!g_atomic_int_get(&run.finished) &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture->context, FALSE);

    g_thread_join(thread);

    return run.status;
}

/*
 * A saved connection can be asked whether it is up, without switching.
 *
 * Until now the only way to find out was to switch to it and fail --
 * which is a destructive way to ask a read-only question, because
 * switching tears down and rebuilds the whole window state, so "is that
 * machine up?" could not be asked without committing to the answer.
 *
 * Both halves in one test. The unreachable case on its own would pass in
 * a build whose probe never answered anything else, and "everything is
 * unreachable" is exactly as useless to a reader as the silence it
 * replaces.
 */
static void
test_a_saved_connection_can_be_asked_if_it_is_up(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *missing = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n  - id: scribe\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    socket_path = g_build_filename(fixture.dir, "daemon.sock", NULL);
    missing = g_build_filename(fixture.dir, "nobody-is-here.sock", NULL);

    {
        g_autoptr(ClawtConnection) live =
            clawt_connection_new_local("live", socket_path);
        ClawtConnectionStatus *status =
            probe_while_the_loop_runs(&fixture, live);

        g_assert_nonnull(status);
        g_assert_cmpint(status->reach, ==, CLAWT_REACH_REACHABLE);

        /*
         * And it brought back what a menu would draw, rather than only a
         * verdict: the version nobody was comparing and the agent count.
         */
        g_assert_nonnull(status->version);
        g_assert_cmpuint(status->agents, ==, 2);

        clawt_connection_status_free(status);
    }

    {
        g_autoptr(ClawtConnection) dead =
            clawt_connection_new_local("dead", missing);
        ClawtConnectionStatus *status =
            probe_while_the_loop_runs(&fixture, dead);

        g_assert_nonnull(status);
        g_assert_cmpint(status->reach, ==, CLAWT_REACH_UNREACHABLE);
        g_assert_nonnull(status->detail);

        clawt_connection_status_free(status);
    }

    fixture_teardown(&fixture);
}


/*
 * A refusal reaches the probe as a refusal, not as silence.
 *
 * The classification is a pure function and is tested as one in
 * test-connection.c, but nothing showed that a real daemon's refusal
 * *arrives* as CLAWT_ERROR_AUTH from inside clawt_client_connect() --
 * and a classifier that is right about an input nothing produces is the
 * shape of feature this tree keeps finding: correct, tested, and wired
 * to nothing. REFUSED is the one verdict the whole type exists for, so
 * it is the one that most needs the wire checked.
 *
 * Behind CLAWT_TEST_INTEGRATION because it cannot be done without a
 * listener: the unix socket authenticates by SO_PEERCRED and has no
 * token to get wrong, so producing a refusal at all means a TCP port --
 * and `make test` opens no network socket.
 *
 * The reachable case is asserted in the same run, over the same TCP
 * connection with the right token. Without it "everything is refused"
 * would pass, which is exactly as useless as the silence this replaces.
 */
static void
test_a_wrong_token_is_refused_rather_than_silent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *token = NULL;
    guint16 port;

    if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }

    /*
     * A port derived from this process, so two runs on one machine do
     * not collide -- and the bind is then *confirmed* rather than
     * assumed. clawt_ipc_server_is_listening_on() exists because an
     * address that was asked for is not an address that was bound, and
     * a probe against a port nothing listens on would report
     * UNREACHABLE and look like the feature failing.
     */
    port = (guint16)(20000 + (getpid() % 20000));

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    /*
     * Written into the file rather than set on the config object: the
     * daemon has no config until it starts -- clawt_daemon_new() only
     * remembers the path -- so clawt_daemon_get_config() is NULL here.
     * Inserted into the existing `daemon:` block rather than appended as
     * a second one, because YAML keeps the last of two identical
     * top-level keys and silently discards everything under the first,
     * which would take the fixture's socket and state_dir with it.
     */
    {
        g_autofree gchar *yaml = NULL;
        g_autofree gchar *patched = NULL;

        g_assert_true(g_file_get_contents(fixture.config_path, &yaml, NULL,
                                          NULL));
        g_assert_true(g_str_has_prefix(yaml, "daemon:\n"));

        patched = g_strdup_printf(
            "daemon:\n  tcp_enabled: true\n  tcp_port: %u\n%s",
            port, yaml + strlen("daemon:\n"));

        g_assert_true(g_file_set_contents(fixture.config_path, patched, -1,
                                          NULL));
    }

    /*
     * The daemon is right to warn here -- a TCP listener with no
     * certificate puts the token on the wire in clear -- and GTest makes
     * a warning fatal, so the warning is swallowed for the length of the
     * start. The same idiom the other daemon-start tests in this file
     * use, for the same reason.
     */
    {
        GLogLevelFlags was_fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK);
        guint handler = g_log_set_handler("Clawtilla",
                                          G_LOG_LEVEL_WARNING |
                                          G_LOG_FLAG_FATAL |
                                          G_LOG_FLAG_RECURSION,
                                          swallow_warnings, NULL);

        g_assert_true(clawt_daemon_start(fixture.daemon, &error));

        g_log_remove_handler("Clawtilla", handler);
        g_log_set_always_fatal(was_fatal);
    }

    g_assert_no_error(error);

    if (!clawt_ipc_server_is_listening_on(
            clawt_daemon_get_ipc_server(fixture.daemon), "127.0.0.1",
            port)) {
        g_test_skip("the daemon bound no TCP port");
        fixture_teardown(&fixture);
        return;
    }

    /* The daemon generates one when token_file names nothing. */
    token_path = g_build_filename(fixture.dir, "state", "tcp-token", NULL);
    g_assert_true(g_file_get_contents(token_path, &token, NULL, NULL));
    g_strstrip(token);

    {
        g_autoptr(ClawtConnection) wrong = clawt_connection_new_remote(
            "wrong", "127.0.0.1", port, "definitely-not-the-token");
        ClawtConnectionStatus *status =
            probe_while_the_loop_runs(&fixture, wrong);

        g_assert_nonnull(status);
        g_assert_cmpint(status->reach, ==, CLAWT_REACH_REFUSED);
        g_assert_nonnull(status->detail);

        clawt_connection_status_free(status);
    }

    {
        g_autoptr(ClawtConnection) right = clawt_connection_new_remote(
            "right", "127.0.0.1", port, token);
        ClawtConnectionStatus *status =
            probe_while_the_loop_runs(&fixture, right);

        g_assert_nonnull(status);
        g_assert_cmpint(status->reach, ==, CLAWT_REACH_REACHABLE);

        clawt_connection_status_free(status);
    }

    fixture_teardown(&fixture);
}


/*
 * What agent.list says about one agent's busy flag.
 *
 * Read back through the real request handler rather than off the
 * ClawtAgent, because that reply is the defect's whole surface: the
 * graphical clients draw a spinner from it, and `clawtilla agent list`
 * does not print it, which is why a permanently-busy agent went
 * unnoticed for hours.
 */
static gboolean
busy_in_agent_list(Fixture *fixture, const gchar *agent_id)
{
    g_autoptr(JsonNode) reply = request(fixture, "agent.list", NULL);
    JsonObject *payload;
    JsonArray *agents;
    guint i;

    g_assert_nonnull(reply);
    g_assert_false(clawt_ipc_frame_is_error(reply));

    payload = clawt_ipc_frame_get_payload(reply);
    g_assert_nonnull(payload);
    agents = json_object_get_array_member(payload, "agents");
    g_assert_nonnull(agents);

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);

        if (g_strcmp0(json_object_get_string_member(agent, "id"),
                      agent_id) == 0)
            return json_object_get_boolean_member(agent, "busy");
    }

    g_assert_not_reached();

    return FALSE;
}

/*
 * Stopping an agent mid-turn ends the turn as far as anybody can see.
 *
 * busy had one setter -- delivery -- and one clearer: the link reporting
 * typing = FALSE. Stopping closes the link, which is precisely what
 * guarantees that message can never arrive, so an agent stopped while
 * answering stayed "working" for the life of the daemon. Observed on a
 * fleet where two agents had been stopped for nine hours and were still
 * drawn with a spinner.
 *
 * The stop is driven through clawt_daemon_stop_agent(), not by setting
 * the state: the bug lives in what that function does and does not do,
 * so a test that put the agent into STOPPED by hand would sit on the
 * wrong side of the very transition it is checking.
 */
static void
test_stopping_an_agent_ends_its_turn(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgent *agent;

    /*
     * The child stays up, so the agent is genuinely mid-turn when it is
     * stopped rather than already gone.  Through `env:` because the
     * runtime's environment is an allowlist and does not inherit this
     * process's -- see the sibling test, where g_setenv() silently
     * reached nothing.
     */
    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    env:\n"
        "      FAKE_LIBRECLAW_SLEEP: \"60\"\n"
        "    runtime:\n"
        "      restart: never\n"
        "    computer:\n"
        "      type: none\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "worker");
    g_assert_nonnull(agent);

    /*
     * The turn begins. This is what delivery does, and it is the setup
     * rather than the thing under test -- what is under test is what
     * happens to it when the agent is stopped.
     */
    clawt_agent_set_activity(agent, TRUE, "researcher");

    /*
     * The positive control, in the same run: the flag really does reach
     * agent.list. Without it the assertion below would pass in a build
     * whose reply never says busy at all.
     */
    g_assert_true(busy_in_agent_list(&fixture, "worker"));

    g_assert_true(clawt_daemon_stop_agent(fixture.daemon, "worker", FALSE));

    g_assert_false(busy_in_agent_list(&fixture, "worker"));

    /*
     * And who it was for survives. set_activity() keeps the peer
     * deliberately, so a finished turn can still say "answered
     * researcher" rather than "idle" -- clearing it here would trade one
     * wrong answer for a less informative one.
     */
    g_assert_cmpstr(clawt_agent_get_activity_peer(agent), ==, "researcher");

    fixture_teardown(&fixture);
}

/*
 * An agent whose process dies mid-turn is not left working either.
 *
 * This is the case a graceful-stop test cannot reach, and it is the one
 * that decides where the fix goes. A killed agent never enters
 * clawt_daemon_stop_agent() and never enters clawt_agent_stop(): its
 * runtime reports `exited` with clean = FALSE while the state is not
 * STOPPING, so on_runtime_exited() takes it to ERROR. A clear placed on
 * either stop path leaves this one busy for ever.
 *
 * Driven by killing the real child, so the state change comes from the
 * runtime noticing rather than from the test asserting it.
 */
static void
test_an_agent_that_dies_mid_turn_is_not_left_working(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgent *agent;
    ClawtAgentRuntime *runtime;
    GPid pid;
    gint64 deadline;

    /*
     * Alive until something kills it, so the process is really running
     * when the turn starts rather than having exited on its own.
     *
     * Through the agent's own `env:` block, not g_setenv(): the process
     * runtime builds the child's environment from an allowlist and does
     * not inherit the daemon's, so a variable set in this process reaches
     * the fake not at all.  Verified by reading /proc/<child>/environ --
     * with g_setenv() it holds PATH, HOME, USER, LOGNAME, SHELL, LANG and
     * the XDG entries and nothing else, so the fake exited immediately and
     * the SIGKILL below was racing its own exit for which state the agent
     * would land in.  It won every time here and would not have on a
     * loaded machine: a clean exit goes to STOPPED, and the assertion is
     * on ERROR.
     */
    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    env:\n"
        "      FAKE_LIBRECLAW_SLEEP: \"60\"\n"
        "    runtime:\n"
        "      restart: never\n"
        "    computer:\n"
        "      type: none\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    /*
     * Pushed around the start, because g_subprocess_wait_async() captures
     * the thread-default context at the moment it is called and answers
     * there. A real daemon runs its own context as the thread default,
     * so this is what production looks like; without it the child's exit
     * is reported to a context this test never iterates and the agent
     * sits in STARTING for the whole timeout, which reads exactly like
     * the fix not working.
     */
    g_main_context_push_thread_default(fixture.context);
    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_main_context_pop_thread_default(fixture.context);
    g_assert_no_error(error);

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "worker");
    g_assert_nonnull(agent);

    runtime = clawt_agent_get_runtime(agent);
    g_assert_nonnull(runtime);
    pid = clawt_agent_runtime_get_pid(runtime);
    g_assert_cmpint(pid, >, 0);

    clawt_agent_set_activity(agent, TRUE, "researcher");
    g_assert_true(busy_in_agent_list(&fixture, "worker"));

    g_assert_cmpint(kill(pid, SIGKILL), ==, 0);

    /*
     * Waited for rather than assumed: the runtime learns about the exit
     * from a child watch on this context, so nothing has happened until
     * the loop has run.
     *
     * Waited for by naming the state it must reach, not by waiting to
     * leave RUNNING. This agent never reaches RUNNING at all -- the fake
     * never opens a link, so it sits in STARTING -- and a loop written
     * as "while RUNNING" exits immediately without iterating once,
     * which makes the assertion below pass or fail for reasons that have
     * nothing to do with the fix.
     */
    deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

    while (clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_ERROR &&
           clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_STOPPED &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture.context, TRUE);

    /*
     * ERROR, not STOPPED: nobody asked this agent to stop, so
     * on_runtime_exited() sees a state that is not STOPPING and an exit
     * that is not clean. That is exactly why a clear placed on either
     * stop path would not cover this case.
     */
    g_assert_cmpint(clawt_agent_get_state(agent), ==,
                    CLAWT_AGENT_STATE_ERROR);

    g_assert_false(busy_in_agent_list(&fixture, "worker"));

    fixture_teardown(&fixture);
}


/*
 * Every message an agent sends in one turn counts from the same depth.
 *
 * A turn is not one message. A chief-of-staff answers its operator *and*
 * hands work to a peer; a lead reports upwards and asks downwards. The
 * depth was cleared as soon as the first of those went out, so the
 * second started a fresh chain at 1 -- and two agents signing off at each
 * other could then do it for ever, because max_hops was measuring the
 * last message of a turn rather than the conversation.
 */
static void
test_a_turn_sends_every_message_at_the_same_depth(void)
{
    Fixture fixture;
    ClawtLinkServer *links;
    ClawtAgentManager *agents;
    ClawtAgent *alpha;
    ClawtAgent *beta;
    g_autoptr(GPtrArray) queued = NULL;

    fixture_setup(&fixture,
                  "orchestration:\n"
                  "  max_hops: 8\n"
                  "  cycle_window: 0\n"
                  "agents:\n  - id: alpha\n  - id: beta\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    links = clawt_daemon_get_link_server(fixture.daemon);
    agents = clawt_daemon_get_agents(fixture.daemon);
    alpha = clawt_agent_manager_get(agents, "alpha");
    beta = clawt_agent_manager_get(agents, "beta");

    /* A peer's message reached alpha having travelled three hops. */
    clawt_agent_set_hop_depth(alpha, 3);

    g_signal_emit_by_name(links, "typing", "alpha", "beta", TRUE);

    /* Two messages out of one turn, which is the ordinary case. */
    g_signal_emit_by_name(links, "message", "alpha", "beta", "First.", NULL);
    g_signal_emit_by_name(links, "message", "alpha", "beta", "Second.", NULL);

    queued = clawt_mailbox_list(clawt_agent_get_mailbox(beta), NULL);
    g_assert_nonnull(queued);
    g_assert_cmpuint(queued->len, ==, 2);

    /*
     * Both at four. The second one arriving at 1 is the whole bug: it
     * means the chain has restarted, and a pair of agents exchanging
     * sign-offs never reaches max_hops however long they keep it up.
     */
    g_assert_cmpint(clawt_mailbox_item_get_depth(
                        g_ptr_array_index(queued, 0)), ==, 4);
    g_assert_cmpint(clawt_mailbox_item_get_depth(
                        g_ptr_array_index(queued, 1)), ==, 4);

    fixture_teardown(&fixture);
}

/*
 * Stopping an agent stops the machine it was using.
 *
 * `computer.container.keep` has said "keep the container when the agent
 * stops, instead of removing it" for as long as it has existed, and
 * nothing stopped the computer when an agent stopped -- so the setting
 * described a moment that never happened and every container an agent
 * had used went on running under a stopped agent.
 *
 * Driven against the *host* backend, which needs nothing but a temporary
 * directory and whose start and stop both move the state. A container
 * would need podman to say anything at all.
 */
static void
test_stopping_an_agent_stops_its_machine(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgent *agent;
    ClawtComputer *computer;

    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "worker");
    g_assert_nonnull(agent);

    computer = clawt_agent_get_computer(agent);
    g_assert_nonnull(computer);
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_RUNNING);

    /*
     * FALSE leaves it alone. `agent reset` and `agent remove` stop the
     * process as a step in something larger and have no business
     * powering the machine down underneath it.
     */
    clawt_daemon_stop_agent(fixture.daemon, "worker", FALSE);
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_RUNNING);

    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));
    g_assert_no_error(error);

    /*
     * And the agent's computer is rebuilt by a start, so the one to ask
     * afterwards is the one it holds now.
     */
    computer = clawt_agent_get_computer(agent);

    clawt_daemon_stop_agent(fixture.daemon, "worker", TRUE);
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_STOPPED);

    fixture_teardown(&fixture);
}

/*
 * And the frame a person presses does it, which is the wiring rather
 * than the rule. A parameter defaulting the wrong way at one of five
 * call sites is exactly how this feature would exist and reach nobody.
 */
static void
test_the_agent_stop_frame_stops_the_machine(void)
{
    Fixture fixture;
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *extra = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = NULL;
    ClawtAgent *agent;
    ClawtComputer *computer;

    extra = g_strdup_printf(
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    enabled: true\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n",
        binary);

    fixture_setup(&fixture, extra);
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);
    g_assert_true(clawt_daemon_start_agent(fixture.daemon, "worker", &error));

    agent = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "worker");
    computer = clawt_agent_get_computer(agent);
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_RUNNING);

    reply = request(&fixture, "agent.stop", "{\"agent\": \"worker\"}");
    g_assert_false(clawt_ipc_frame_is_error(reply));

    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_STOPPED);

    fixture_teardown(&fixture);
}

/*
 * Powering an agent's machine on and off is offered only where there is
 * one, and refused where there is not.
 *
 * The refusal is on the *type* rather than on whether the backend
 * happens to have a stop(): host_stop() exists and is a no-op, so
 * reaching it would answer "stopped" about the operator's own
 * workstation -- which is the quiet lie the whole path exists to avoid.
 */
static void
test_a_machineless_agent_cannot_be_powered(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) chat_only = NULL;
    g_autoptr(JsonNode) on_host = NULL;
    const gchar *message;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: nobody\n"
                  "    computer:\n      type: none\n"
                  "  - id: onhost\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      host:\n        confirm_host_control: true\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    chat_only = request(&fixture, "computer.stop",
                        "{\"agent\": \"nobody\"}");
    g_assert_true(clawt_ipc_frame_is_error(chat_only));

    on_host = request(&fixture, "computer.start",
                      "{\"agent\": \"onhost\"}");
    g_assert_true(clawt_ipc_frame_is_error(on_host));

    /*
     * And it says why in the host's own terms.  "No computer" would be
     * wrong there and would send somebody to the config to add one.
     */
    message = json_object_get_string_member(json_node_get_object(on_host),
                                            "error");
    g_assert_nonnull(strstr(message, "runs on this one"));

    fixture_teardown(&fixture);
}

/*
 * Stopping a container destroys it unless keep is set, so the daemon
 * refuses until it is told to go ahead.
 *
 * A fence rather than care: the contents are gone rather than offline,
 * and "stop" is not a word anybody reads that way. Both clients warn
 * first; this is what protects the one that does not.
 */
static void
test_a_destroying_stop_is_fenced(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) refused = NULL;
    const gchar *message;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: boxy\n"
                  "    computer:\n"
                  "      type: container\n"
                  "      container:\n"
                  "        image: \"alpine\"\n        keep: false\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    refused = request(&fixture, "computer.stop", "{\"agent\": \"boxy\"}");

    g_assert_true(clawt_ipc_frame_is_error(refused));

    message = json_object_get_string_member(json_node_get_object(refused),
                                            "error");

    /* It names both the flag and the setting that makes it unnecessary. */
    g_assert_nonnull(strstr(message, "remove"));
    g_assert_nonnull(strstr(message, "computer.container.keep"));

    fixture_teardown(&fixture);
}

/*
 * The listing says what can be powered and what a stop costs, so a
 * client never has to work either out from the type.
 *
 * `computer` stays a string. The first draft of this reported the two
 * new facts as an object under that same name, and json-glib keeps the
 * *last* member of a duplicated key -- so the object was silently
 * dropped and the only symptom was a client reading a string where an
 * object should have been.
 */
static void
test_the_listing_says_what_can_be_powered(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *agents;
    guint i;
    guint seen = 0;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: nobody\n"
                  "    computer:\n      type: none\n"
                  "  - id: boxy\n"
                  "    computer:\n"
                  "      type: container\n"
                  "      container:\n"
                  "        image: \"alpine\"\n        keep: false\n"
                  "  - id: keepy\n"
                  "    computer:\n"
                  "      type: container\n"
                  "      container:\n"
                  "        image: \"alpine\"\n        keep: true\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, NULL));

    reply = request(&fixture, "agent.list", NULL);
    g_assert_false(clawt_ipc_frame_is_error(reply));

    agents = json_object_get_array_member(
        json_object_get_object_member(json_node_get_object(reply), "payload"),
        "agents");
    g_assert_nonnull(agents);

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = json_object_get_string_member(agent, "id");

        /* Still a string, and still the type. */
        g_assert_true(JSON_NODE_HOLDS_VALUE(
            json_object_get_member(agent, "computer")));

        if (g_strcmp0(id, "nobody") == 0) {
            g_assert_cmpstr(json_object_get_string_member(agent, "computer"),
                            ==, "none");
            g_assert_false(json_object_get_boolean_member(
                agent, "computer_machine"));
            g_assert_false(json_object_get_boolean_member(
                agent, "computer_stop_removes"));
            seen++;
        } else if (g_strcmp0(id, "boxy") == 0) {
            g_assert_true(json_object_get_boolean_member(
                agent, "computer_machine"));

            /* keep turned off, so a stop takes the container with it. */
            g_assert_true(json_object_get_boolean_member(
                agent, "computer_stop_removes"));
            seen++;
        } else if (g_strcmp0(id, "keepy") == 0) {
            /* And on -- which is the default -- it survives. */
            g_assert_true(json_object_get_boolean_member(
                agent, "computer_machine"));
            g_assert_false(json_object_get_boolean_member(
                agent, "computer_stop_removes"));
            seen++;
        }
    }

    g_assert_cmpuint(seen, ==, 3);

    fixture_teardown(&fixture);
}


/* ── Triggers ────────────────────────────────────────────────────── */

/*
 * A new trigger is created switched off, with a secret shown once.
 *
 * All three parts matter and all three are easy to lose. The secret must
 * cross IPC exactly here; the trigger must start off, because the first
 * delivery is captured rather than run; and the endpoint must come back,
 * because without it there is nothing to put in the forge's form.
 */
static void
test_adding_a_trigger_shows_its_secret_once(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) added = NULL;
    g_autoptr(JsonNode) listed = NULL;
    JsonObject *reply;
    JsonArray *triggers;
    JsonObject *trigger;
    g_autofree gchar *secret = NULL;

    fixture_setup(&fixture, "agents:\n  - id: builder\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    added = request(&fixture, "trigger.add",
                    "{\"id\": \"ci\", \"agent\": \"builder\","
                    " \"instructions\": \"Look at {{repo}}.\","
                    " \"provider\": \"forgejo\", \"enabled\": true}");

    g_assert_false(clawt_ipc_frame_is_error(added));
    reply = payload_of(added);

    g_assert_true(json_object_has_member(reply, "secret"));
    g_assert_true(json_object_get_boolean_member(reply, "secret_shown_once"));
    g_assert_true(json_object_has_member(reply, "endpoint"));

    secret = g_strdup(json_object_get_string_member(reply, "secret"));
    g_assert_cmpuint(strlen(secret), >=, 32);

    /*
     * Off, even though the request asked for it on.
     *
     * Honouring `enabled: true` here would run an agent on the first
     * body anybody sent, which is precisely what the handshake exists to
     * prevent -- so it is overridden rather than obeyed.
     */
    listed = request(&fixture, "trigger.list", NULL);
    triggers = json_object_get_array_member(payload_of(listed), "triggers");
    g_assert_cmpuint(json_array_get_length(triggers), ==, 1);

    trigger = json_array_get_object_element(triggers, 0);
    g_assert_false(json_object_get_boolean_member(trigger, "enabled"));
    g_assert_true(json_object_get_boolean_member(trigger,
                                                 "pending_verification"));
    g_assert_true(json_object_get_boolean_member(trigger, "has_secret"));

    fixture_teardown(&fixture);
}

/*
 * The secret is in that one reply and nowhere else, ever.
 *
 * This is the assertion the whole phase turns on. `trigger.list` walks
 * the schema to build its rows, and the generic branch of that loop
 * would read `secret` as a string and put the reference -- for an env or
 * command backend, effectively the credential's whereabouts -- into
 * every listing every client makes, on every tailnet the daemon answers
 * on. Asserted on the serialised frame rather than on named members, so
 * a member added later cannot smuggle it back in under another name.
 */
static void
test_a_listing_never_carries_the_secret(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) added = NULL;
    g_autoptr(JsonNode) listed = NULL;
    g_autoptr(JsonNode) deliveries = NULL;
    g_autofree gchar *secret = NULL;
    g_autofree gchar *listing = NULL;
    g_autofree gchar *receipts = NULL;

    fixture_setup(&fixture, "agents:\n  - id: builder\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    added = request(&fixture, "trigger.add",
                    "{\"id\": \"ci\", \"agent\": \"builder\","
                    " \"instructions\": \"go\"}");
    g_assert_false(clawt_ipc_frame_is_error(added));

    secret = g_strdup(json_object_get_string_member(payload_of(added),
                                                    "secret"));

    listed = request(&fixture, "trigger.list", NULL);
    listing = json_to_string(listed, FALSE);

    g_assert_null(strstr(listing, secret));

    /* Nor the reference, which says where the credential lives. */
    g_assert_null(strstr(listing, "trigger-ci"));
    g_assert_null(strstr(listing, "\"secret\""));

    deliveries = request(&fixture, "trigger.deliveries", NULL);
    receipts = json_to_string(deliveries, FALSE);
    g_assert_null(strstr(receipts, secret));

    fixture_teardown(&fixture);
}

/*
 * A trigger cannot be switched on before it has ever been called.
 *
 * The two states together are a lie: the listing would say "on" and the
 * next delivery would still be captured rather than run, so somebody
 * watching for a run the client told them to expect would check the
 * forge, the secret and the filters before finding out the trigger had
 * simply never been verified.
 */
static void
test_enabling_before_the_first_delivery_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) added = NULL;
    g_autoptr(JsonNode) enabled = NULL;
    g_autoptr(JsonNode) listed = NULL;
    JsonObject *trigger;

    fixture_setup(&fixture, "agents:\n  - id: builder\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    added = request(&fixture, "trigger.add",
                    "{\"id\": \"ci\", \"agent\": \"builder\","
                    " \"instructions\": \"go\"}");
    g_assert_false(clawt_ipc_frame_is_error(added));

    enabled = request(&fixture, "trigger.update",
                      "{\"id\": \"ci\", \"enabled\": true}");

    g_assert_true(clawt_ipc_frame_is_error(enabled));

    /* And the refusal says what to do instead, rather than only no. */
    {
        const gchar *text = json_object_get_string_member(
            json_node_get_object(enabled), "error");

        g_assert_nonnull(text);
        g_assert_nonnull(strstr(text, "capture"));
    }

    /* It is still off, rather than half-applied. */
    listed = request(&fixture, "trigger.list", NULL);
    trigger = json_array_get_object_element(
        json_object_get_array_member(payload_of(listed), "triggers"), 0);
    g_assert_false(json_object_get_boolean_member(trigger, "enabled"));

    fixture_teardown(&fixture);
}

/*
 * Rotating hands back a new secret and a new address in one step.
 *
 * Rotating because a secret leaked and leaving the endpoint in place
 * would mean whoever had it still knows where to knock -- so the two
 * move together, and both have to come back or the webhook cannot be
 * repointed.
 */
static void
test_rotating_changes_the_secret_and_the_address(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) added = NULL;
    g_autoptr(JsonNode) rotated = NULL;
    g_autofree gchar *first_secret = NULL;
    g_autofree gchar *first_endpoint = NULL;

    fixture_setup(&fixture, "agents:\n  - id: builder\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    added = request(&fixture, "trigger.add",
                    "{\"id\": \"ci\", \"agent\": \"builder\","
                    " \"instructions\": \"go\"}");
    first_secret = g_strdup(json_object_get_string_member(payload_of(added),
                                                          "secret"));
    first_endpoint = g_strdup(json_object_get_string_member(
                                  payload_of(added), "endpoint"));

    rotated = request(&fixture, "trigger.rotate", "{\"id\": \"ci\"}");
    g_assert_false(clawt_ipc_frame_is_error(rotated));

    g_assert_cmpstr(json_object_get_string_member(payload_of(rotated),
                                                  "secret"),
                    !=, first_secret);
    g_assert_cmpstr(json_object_get_string_member(payload_of(rotated),
                                                  "endpoint"),
                    !=, first_endpoint);

    fixture_teardown(&fixture);
}

/*
 * `trigger.test` shows the prompt without asking an agent for anything.
 *
 * The point is that somebody can see the placeholders filled in and the
 * untrusted-payload fence before a forge sends anything real -- so the
 * default has to be a preview, and running has to be the thing you ask
 * for.
 */
static void
test_testing_a_trigger_previews_rather_than_runs(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) added = NULL;
    g_autoptr(JsonNode) tested = NULL;
    const gchar *prompt;

    fixture_setup(&fixture, "agents:\n  - id: builder\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    added = request(&fixture, "trigger.add",
                    "{\"id\": \"ci\", \"agent\": \"builder\","
                    " \"repo\": \"zach/clawtilla\","
                    " \"instructions\": \"Look at {{repo}} for {{event}}.\"}");
    g_assert_false(clawt_ipc_frame_is_error(added));

    tested = request(&fixture, "trigger.test", "{\"id\": \"ci\"}");
    g_assert_false(clawt_ipc_frame_is_error(tested));

    /* A prompt, and no task: nothing was asked of anybody. */
    g_assert_false(json_object_has_member(payload_of(tested), "task"));

    prompt = json_object_get_string_member(payload_of(tested), "prompt");
    g_assert_nonnull(strstr(prompt, "Look at zach/clawtilla for push."));
    g_assert_nonnull(strstr(prompt, "untrusted-event-payload"));

    fixture_teardown(&fixture);
}

/*
 * With the receiver off, the listing says so once rather than per row.
 *
 * Every trigger is equally unreachable in that state, so repeating it
 * per trigger would read as a per-trigger fault -- and somebody would go
 * looking at the secret of whichever one they cared about.
 */
static void
test_the_listing_says_when_nothing_is_listening(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) listed = NULL;
    g_autoptr(JsonNode) deliveries = NULL;

    fixture_setup(&fixture, "agents:\n  - id: builder\n");
    g_assert_true(clawt_daemon_start(fixture.daemon, &error));
    g_assert_no_error(error);

    listed = request(&fixture, "trigger.list", NULL);

    /*
     * FALSE, because `daemon.webhook_enabled` defaults off -- which is
     * also what keeps `make test` from opening a network socket.
     */
    g_assert_false(json_object_get_boolean_member(payload_of(listed),
                                                  "receiving"));

    /* And an empty receipt list says which kind of empty it is. */
    deliveries = request(&fixture, "trigger.deliveries", NULL);
    g_assert_nonnull(strstr(
        json_object_get_string_member(payload_of(deliveries), "note"),
        "webhook_enabled"));

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
    g_test_add_func("/daemon/correcting-a-shadow-key-clears-it",
                    test_correcting_a_shadow_key_clears_the_shadow);
    g_test_add_func("/daemon/breaking-a-key-shadows-again",
                    test_breaking_a_key_shadows_the_agent_again);
    g_test_add_func("/daemon/refuses-a-second-on-the-same-state-dir",
                    test_refuses_a_second_daemon_on_the_same_state_dir);
    g_test_add_func("/daemon/stopping-releases-the-state-lock",
                    test_stopping_releases_the_state_lock);
    g_test_add_func("/daemon/reply-after-turn-counts-hops",
                    test_a_reply_after_the_turn_ends_still_counts_hops);
    g_test_add_func("/daemon/a-reply-earns-no-reply",
                    test_a_reply_earns_no_reply);
    g_test_add_func("/daemon/interrupt-names-what-it-cannot-do",
                    test_interrupting_names_what_it_cannot_do);
    g_test_add_func("/daemon/interrupt-keeps-activity-on-refusal",
                    test_interrupting_clears_the_activity);
    g_test_add_func("/daemon/closed-turn-still-answers-the-operator",
                    test_a_closed_turn_still_answers_the_operator);
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
    g_test_add_func("/daemon/refusal-reader-survives-a-hollow-node",
                    test_the_refusal_reader_survives_an_object_that_is_not_there);
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
    g_test_add_func("/daemon/passthrough-channels-merge",
                    test_passthrough_channels_are_merged);
    g_test_add_func("/daemon/passthrough-channel-key-collides",
                    test_a_colliding_channel_key_is_refused);
    g_test_add_func("/daemon/event-log-readable",
                    test_the_event_log_can_be_read_back);
    g_test_add_func("/daemon/attachment-served-as-bytes",
                    test_an_attachment_is_served_as_bytes);
    g_test_add_func("/daemon/isolated-routine-gets-a-room",
                    test_an_isolated_routine_gets_its_own_room);
    g_test_add_func("/daemon/ordinary-routine-shares-the-room",
                    test_an_ordinary_routine_stays_in_the_conversation);
    g_test_add_func("/daemon/token-stable",
                    test_token_is_stable_across_renders);

    g_test_add_func("/daemon/queue-for-stopped-agent",
                    test_message_to_a_stopped_agent_queues);
    g_test_add_func("/daemon/cycle-window-reaches-the-guard",
                    test_the_cycle_window_reaches_the_guard);
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
    g_test_add_func("/daemon/hops/a-rooms-limit-overrides-the-fleets",
                    test_a_rooms_hop_limit_overrides_the_fleets);
    g_test_add_func("/daemon/sweep/applies-the-exchange-cap",
                    test_the_sweep_applies_the_exchange_cap);
    g_test_add_func("/daemon/audit/tools-get-the-event-bus",
                    test_the_daemon_gives_the_tools_its_event_bus);
    g_test_add_func("/daemon/priority/urgent-overtakes-an-earlier-normal",
                    test_urgent_overtakes_an_earlier_ordinary_message);
    g_test_add_func("/daemon/priority/a-pod-can-send-at-a-band",
                    test_a_pod_can_send_at_a_band);
    g_test_add_func("/daemon/screen/no-frame-yet-is-not-an-error",
                    test_a_screen_with_no_frame_yet_is_not_an_error);
    g_test_add_func("/daemon/memory/a-pod-remembers-in-its-category",
                    test_a_pod_remembers_in_the_category_it_named);
    g_test_add_func("/daemon/memory/a-pod-remembers-every-field",
                    test_a_pod_remembers_every_field_it_named);
    g_test_add_func("/daemon/memory/a-pod-level-that-is-not-one",
                    test_a_pod_naming_a_level_that_is_not_one_writes_nothing);
    g_test_add_func("/daemon/memory/a-routed-message-can-be-recalled",
                    test_a_routed_message_can_be_recalled);
    g_test_add_func("/daemon/memory/operator-profile-reaches-agents",
                    test_the_operator_profile_reaches_every_agent);
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
    g_test_add_func("/daemon/decision-round-trip",
                    test_a_decision_round_trips_through_the_daemon);
    g_test_add_func("/daemon/answering-a-decision-says-so",
                    test_answering_a_decision_says_so);
    g_test_add_func("/daemon/dismissing-a-decision-says-so",
                    test_dismissing_a_decision_says_so);
    g_test_add_func("/daemon/autostart-does-not-run-inside-start",
                    test_autostart_does_not_run_inside_start);
    g_test_add_func("/daemon/start-returns-with-a-mute-podman-socket",
                    test_start_returns_with_a_podman_socket_that_never_answers);
    g_test_add_func("/daemon/a-wrong-token-is-refused",
                    test_a_wrong_token_is_refused_rather_than_silent);
    g_test_add_func("/daemon/a-saved-connection-can-be-asked",
                    test_a_saved_connection_can_be_asked_if_it_is_up);
    g_test_add_func("/daemon/loop-runs-while-a-computer-provisions",
                    test_the_loop_runs_while_a_computer_is_provisioning);
    g_test_add_func("/daemon/loop-runs-while-an-exec-is-outstanding",
                    test_the_loop_runs_while_an_exec_is_outstanding);
    g_test_add_func("/daemon/loop-runs-while-an-operator-exec-is-outstanding",
                    test_the_loop_runs_while_an_operator_exec_is_outstanding);
    g_test_add_func("/daemon/operator-exec-is-recorded-once-when-it-ends",
                    test_an_operator_exec_is_recorded_once_when_it_ends);
    g_test_add_func("/daemon/refused-operator-exec-is-still-recorded",
                    test_an_operator_exec_that_is_refused_is_still_recorded);
    g_test_add_func("/daemon/oversized-persona-is-announced-at-start",
                    test_an_oversized_persona_is_announced_at_start);
    g_test_add_func("/daemon/ordinary-persona-is-not-announced",
                    test_an_ordinary_persona_is_not_announced);
    g_test_add_func("/daemon/agent-show-carries-the-identity-size",
                    test_agent_show_carries_the_identity_size);
    g_test_add_func("/daemon/stopping-an-agent-ends-its-turn",
                    test_stopping_an_agent_ends_its_turn);
    g_test_add_func("/daemon/a-dead-agent-is-not-working",
                    test_an_agent_that_dies_mid_turn_is_not_left_working);
    g_test_add_func("/daemon/restart-policy-reaches-the-runtime",
                    test_a_changed_restart_policy_reaches_the_runtime);
    g_test_add_func("/daemon/second-start-reaches-the-computer",
                    test_a_second_start_reaches_the_computer);
    g_test_add_func("/daemon/task-change-reaches-the-bus",
                    test_a_task_change_reaches_the_bus);
    g_test_add_func("/daemon/agent-set-refuses-an-unknown-enum-value",
                    test_agent_set_refuses_a_value_the_enum_lacks);
    g_test_add_func("/daemon/agent-set-reports-a-role-needs-a-session",
                    test_agent_set_reports_a_role_needs_a_new_session);
    g_test_add_func("/daemon/hop-depth/one-turn-one-depth",
                    test_a_turn_sends_every_message_at_the_same_depth);
    g_test_add_func("/daemon/computer/agent-stop-stops-the-machine",
                    test_stopping_an_agent_stops_its_machine);
    g_test_add_func("/daemon/computer/agent-stop-frame-stops-the-machine",
                    test_the_agent_stop_frame_stops_the_machine);
    g_test_add_func("/daemon/computer/no-machine-is-refused",
                    test_a_machineless_agent_cannot_be_powered);
    g_test_add_func("/daemon/computer/a-destroying-stop-is-fenced",
                    test_a_destroying_stop_is_fenced);
    g_test_add_func("/daemon/computer/listing-says-what-can-be-powered",
                    test_the_listing_says_what_can_be_powered);
    g_test_add_func("/daemon/trigger/secret-shown-once",
                    test_adding_a_trigger_shows_its_secret_once);
    g_test_add_func("/daemon/trigger/listing-has-no-secret",
                    test_a_listing_never_carries_the_secret);
    g_test_add_func("/daemon/trigger/enable-before-verified",
                    test_enabling_before_the_first_delivery_is_refused);
    g_test_add_func("/daemon/trigger/rotate",
                    test_rotating_changes_the_secret_and_the_address);
    g_test_add_func("/daemon/trigger/test-previews",
                    test_testing_a_trigger_previews_rather_than_runs);
    g_test_add_func("/daemon/trigger/not-listening",
                    test_the_listing_says_when_nothing_is_listening);

    g_test_add_func("/daemon/start-agent-with-no-id",
                    test_starting_an_agent_with_no_id_is_an_error);

    g_test_add_func("/daemon/start/refreshes-the-owned-regions",
                    test_starting_an_agent_refreshes_its_regions);
    g_test_add_func("/daemon/typing/a-keepalive-is-not-a-new-turn",
                    test_a_keepalive_is_not_a_new_turn);
    g_test_add_func("/daemon/typing/each-room-has-its-own-turn",
                    test_each_room_has_its_own_turn);
    g_test_add_func("/daemon/typing/one-quiet-room-does-not-end-the-turn",
                    test_one_room_going_quiet_does_not_end_the_agents_turn);
    g_test_add_func("/daemon/typing/settled-elsewhere-starts-fresh",
                    test_a_turn_settled_elsewhere_starts_the_next_one_fresh);
    g_test_add_func("/daemon/typing/a-keepalive-does-not-extend-the-budget",
                    test_a_keepalive_does_not_extend_the_turn_budget);
    g_test_add_func("/daemon/task/children-hold-the-parent-open",
                    test_a_turn_ending_does_not_close_a_task_with_children);
    g_test_add_func("/daemon/task/running-when-its-turn-starts",
                    test_a_task_starts_running_when_its_turn_does);
    g_test_add_func("/daemon/computer-exec/a-shell-line-is-refused",
                    test_the_exec_verb_refuses_a_shell_line);

    status = g_test_run();

    clawt_test_remove_tree(data_dir);

    return status;
}
