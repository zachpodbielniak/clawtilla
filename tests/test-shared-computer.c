/*
 * test-shared-computer.c - Machines several agents share
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two shared machines and three agents, because every bug this shape has
 * had was a scalar one key could not see: a and b on alpha, c on beta.
 * An agent on a shared machine reads every computer.* key from the
 * machine's entry; the machine is built once, under the machine's name,
 * with every user's workspace; one agent at a time may drive its screen;
 * and agent.set refuses what it could only save to be ignored.
 *
 * The daemon half includes core/clawt-daemon-private.h, as test-policy
 * does. The machines are qemu VMs because constructing one touches
 * nothing -- nothing here is ever started.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

static const gchar FLEET[] =
    "computers:\n"
    "  - id: alpha\n"
    "    description: the build box\n"
    "    type: vm\n"
    "    vm:\n      backend: qemu\n      cpus: 6\n"
    "    desktop:\n      driver_idle_seconds: 300\n"
    "  - id: beta\n"
    "    type: vm\n"
    "    vm:\n      backend: qemu\n      cpus: 2\n"
    "agents:\n"
    "  - id: a\n    computer:\n      use: alpha\n"
    "    runtime:\n      autostart: false\n"
    "  - id: b\n    computer:\n      use: alpha\n"
    "    runtime:\n      autostart: false\n"
    "  - id: c\n    computer:\n      use: beta\n"
    "    runtime:\n      autostart: false\n"
    "  - id: solo\n    computer:\n      type: none\n"
    "    runtime:\n      autostart: false\n";

/* ── The config ─────────────────────────────────────────────────── */

/* Every computer.* key but use is the machine's; use is the agent's. */
static void
test_keys_come_from_the_machine(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(FLEET, &error);
    ClawtAgentConfig *a;
    ClawtAgentConfig *c;
    g_autoptr(GPtrArray) ids = NULL;

    g_assert_no_error(error);
    a = clawt_config_get_agent(config, "a");
    c = clawt_config_get_agent(config, "c");
    g_assert_false(clawt_agent_config_is_shadow(a));

    g_assert_cmpstr(clawt_agent_config_get_string(a, "computer.use"), ==, "alpha");
    g_assert_cmpstr(clawt_agent_config_get_string(a, "computer.type"), ==, "vm");
    g_assert_cmpint(clawt_agent_config_get_int(a, "computer.vm.cpus"), ==, 6);
    g_assert_cmpint(clawt_agent_config_get_int(c, "computer.vm.cpus"), ==, 2);
    g_assert_cmpint(clawt_agent_config_get_int(a, "computer.desktop.driver_idle_seconds"),
                    ==, 300);
    /* Not in beta's entry: the schema default, not alpha's value. */
    g_assert_cmpint(clawt_agent_config_get_int(c, "computer.desktop.driver_idle_seconds"),
                    ==, 120);

    ids = clawt_config_get_computer_ids(config);
    g_assert_cmpuint(ids->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(ids, 0), ==, "alpha");
    g_assert_cmpstr(g_ptr_array_index(ids, 1), ==, "beta");
    g_assert_true(clawt_config_has_computer(config, "beta"));
    g_assert_false(clawt_config_has_computer(config, "gamma"));
    g_assert_cmpstr(clawt_config_get_computer_string(config, "alpha", "description"),
                    ==, "the build box");
}

/* A machine that is not declared, or that cannot be shared, shadows the agent. */
static void
test_bad_use_shadows(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "computers:\n"
        "  - id: mine\n    type: host\n"
        "agents:\n"
        "  - id: lost\n    computer:\n      use: nowhere\n"
        "  - id: greedy\n    computer:\n      use: mine\n", &error);
    ClawtAgentConfig *lost;
    ClawtAgentConfig *greedy;

    g_assert_no_error(error);
    lost = clawt_config_get_agent(config, "lost");
    greedy = clawt_config_get_agent(config, "greedy");

    g_assert_true(clawt_agent_config_is_shadow(lost));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(lost), "no computer 'nowhere'"));
    g_assert_true(clawt_agent_config_is_shadow(greedy));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(greedy), "host computer"));
}

/* ── The daemon ─────────────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-shared-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "config.yaml", NULL);
    yaml = g_strdup_printf("daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
                           "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n%s",
                           fixture->dir, fixture->dir, fixture->dir, fixture->dir, FLEET);
    g_assert_true(g_file_set_contents(path, yaml, -1, &error));
    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(path, fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

static JsonNode *
request(DaemonFixture *fixture, const gchar *kind, const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
    clawt_ipc_frame_set_payload(frame, json_node_copy(json_parser_get_root(parser)));
    return clawt_daemon_handle_request(fixture->daemon, frame);
}

/* Whether @computer mounts something at @target. */
static gboolean
mounts_at(ClawtComputer *computer, const gchar *target)
{
    GPtrArray *mounts = clawt_computer_get_mounts(computer);
    guint i;

    for (i = 0; mounts != NULL && i < mounts->len; i++)
        if (g_strcmp0(clawt_mount_get_target(g_ptr_array_index(mounts, i)), target) == 0)
            return TRUE;

    return FALSE;
}

/* One object per machine, named after the machine, with every user's workspace. */
static void
test_pool_shares_one_machine(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtComputer) for_a = NULL;
    g_autoptr(ClawtComputer) for_b = NULL;
    g_autoptr(ClawtComputer) for_c = NULL;
    g_autofree gchar *a_inside = clawt_computer_shared_workspace_path("a");
    g_autofree gchar *b_inside = clawt_computer_shared_workspace_path("b");
    g_autofree gchar *c_inside = clawt_computer_shared_workspace_path("c");

    daemon_setup(&fixture);

    for_a = clawt_daemon_shared_computer_get(fixture.daemon,
        clawt_agent_manager_get(fixture.daemon->agents, "a"), &error);
    g_assert_no_error(error);
    for_b = clawt_daemon_shared_computer_get(fixture.daemon,
        clawt_agent_manager_get(fixture.daemon->agents, "b"), &error);
    g_assert_no_error(error);
    for_c = clawt_daemon_shared_computer_get(fixture.daemon,
        clawt_agent_manager_get(fixture.daemon->agents, "c"), &error);
    g_assert_no_error(error);

    g_assert_true(for_a == for_b);
    g_assert_true(for_a != for_c);

    /* Named after the machine, which no agent id can collide with. */
    g_assert_cmpstr(clawt_computer_get_agent_id(for_a), ==, "c.alpha");
    g_assert_cmpstr(clawt_computer_get_agent_id(for_c), ==, "c.beta");

    /* Every user's workspace on its machine, and nobody else's. */
    g_assert_cmpstr(a_inside, ==, CLAWT_SHARED_WORKSPACES_ROOT "/a");
    g_assert_true(mounts_at(for_a, a_inside));
    g_assert_true(mounts_at(for_a, b_inside));
    g_assert_false(mounts_at(for_a, c_inside));
    g_assert_true(mounts_at(for_c, c_inside));
    g_assert_false(mounts_at(for_c, a_inside));

    /* Nobody is running, so nothing on either machine is busy. */
    {
        g_autofree gchar *busy = clawt_daemon_shared_busy_refusal(fixture.daemon, "a", "stop it");
        g_autoptr(GPtrArray) users = clawt_daemon_shared_users(fixture.daemon, "alpha",
                                                               FALSE, NULL);

        g_assert_null(busy);
        g_assert_cmpuint(users->len, ==, 2);
    }

    daemon_teardown(&fixture);
}

/* The description names the machine, the others on it and this agent's own folder. */
static void
test_agent_is_told_who_else_is_there(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgent *a;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autofree gchar *text = NULL;

    daemon_setup(&fixture);
    a = clawt_agent_manager_get(fixture.daemon->agents, "a");
    computer = clawt_daemon_shared_computer_get(fixture.daemon, a, &error);
    g_assert_no_error(error);
    clawt_agent_set_computer(a, computer);

    text = clawt_agent_describe_computer(a);
    g_assert_nonnull(strstr(text, "shared computer 'alpha'"));
    g_assert_nonnull(strstr(text, "share with b"));
    g_assert_null(strstr(text, "share with b, c"));
    g_assert_nonnull(strstr(text, CLAWT_SHARED_WORKSPACES_ROOT "/a;"));
    /* Found live: the backend's opening sentence called it "of your own". */
    g_assert_null(strstr(text, "of your own"));
    g_assert_true(clawt_computer_is_shared(computer));

    clawt_agent_set_computer(a, NULL);
    daemon_teardown(&fixture);
}

/* One driver per screen; another machine's screen is another lease. */
static void
test_one_driver_per_screen(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *why = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *answer;

    daemon_setup(&fixture);

    g_assert_true(clawt_daemon_shared_take_screen(fixture.daemon, "a", NULL));
    g_assert_false(clawt_daemon_shared_take_screen(fixture.daemon, "b", &why));
    g_assert_nonnull(strstr(why, "a is driving the screen of the shared computer 'alpha'"));

    /* The holder keeps it; c is on beta and never waits for a. */
    g_assert_true(clawt_daemon_shared_take_screen(fixture.daemon, "a", NULL));
    g_assert_true(clawt_daemon_shared_take_screen(fixture.daemon, "c", NULL));
    /* An agent with a machine of its own has no lease to take. */
    g_assert_true(clawt_daemon_shared_take_screen(fixture.daemon, "solo", NULL));

    /* Through the gate the relay asks: acting refused, looking allowed. */
    reply = request(&fixture, "computer.control", "{\"agent\":\"b\",\"tool\":\"mouse_click\"}");
    answer = clawt_ipc_frame_get_payload(reply);
    g_assert_false(json_object_get_boolean_member(answer, "allowed"));
    g_assert_nonnull(strstr(json_object_get_string_member(answer, "refusal"), "a is driving"));
    g_clear_pointer(&reply, json_node_unref);

    reply = request(&fixture, "computer.control", "{\"agent\":\"b\",\"tool\":\"screenshot\"}");
    answer = clawt_ipc_frame_get_payload(reply);
    g_assert_true(json_object_get_boolean_member(answer, "allowed"));

    daemon_teardown(&fixture);
}

/* agent.set refuses what it would only save to be ignored or shadowed. */
static void
test_set_refuses_at_arrival(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    daemon_setup(&fixture);

    reply = request(&fixture, "agent.set",
                    "{\"agent\":\"solo\",\"key\":\"computer.use\",\"value\":\"gamma\"}");
    g_assert_true(clawt_ipc_frame_is_error(reply));
    g_clear_pointer(&reply, json_node_unref);

    /* The machine's own keys are the machine's. */
    reply = request(&fixture, "agent.set",
                    "{\"agent\":\"a\",\"key\":\"computer.vm.cpus\",\"value\":\"12\"}");
    g_assert_true(clawt_ipc_frame_is_error(reply));
    g_clear_pointer(&reply, json_node_unref);

    /* Moving onto one that exists is fine. */
    reply = request(&fixture, "agent.set",
                    "{\"agent\":\"solo\",\"key\":\"computer.use\",\"value\":\"beta\"}");
    g_assert_false(clawt_ipc_frame_is_error(reply));
    g_clear_pointer(&reply, json_node_unref);

    /* And the listing reports it among beta's users. */
    {
        g_autoptr(JsonNode) listing = request(&fixture, "computer.shared", "{}");
        g_autofree gchar *text = json_to_string(listing, FALSE);

        g_assert_nonnull(strstr(text, "\"id\":\"beta\""));
        g_assert_nonnull(strstr(text, "\"users\":[\"c\",\"solo\"]"));
        g_assert_nonnull(strstr(text, "\"users\":[\"a\",\"b\"]"));
        g_assert_nonnull(strstr(text, "\"description\":\"the build box\""));
    }

    daemon_teardown(&fixture);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* The shadowed fleet warns at load on purpose; criticals stay fatal. */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

    g_test_add_func("/shared-computer/keys-come-from-the-machine",
                    test_keys_come_from_the_machine);
    g_test_add_func("/shared-computer/bad-use-shadows", test_bad_use_shadows);
    g_test_add_func("/shared-computer/pool-shares-one-machine", test_pool_shares_one_machine);
    g_test_add_func("/shared-computer/agent-is-told-who-else", test_agent_is_told_who_else_is_there);
    g_test_add_func("/shared-computer/one-driver-per-screen", test_one_driver_per_screen);
    g_test_add_func("/shared-computer/set-refuses-at-arrival", test_set_refuses_at_arrival);

    return g_test_run();
}
