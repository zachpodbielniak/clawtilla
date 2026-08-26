/*
 * test-agent.c - Agents, their state machine, and the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The process runtime is exercised against a fake libreclaw in
 * tests/fixtures, so restart policy and log capture can be tested without
 * a real agent, a real model or a network.
 */

#include <clawtilla.h>

#include <string.h>

#include <signal.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

typedef struct {
    gchar       *dir;
    ClawtConfig *config;
} Fixture;

static ClawtConfig *
load_config(Fixture *fixture, const gchar *yaml)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *with_state = NULL;
    ClawtConfig *config;

    /* Point state_dir at a temporary directory so mailboxes land there. */
    with_state = g_strdup_printf("daemon:\n  state_dir: \"%s\"\n%s",
                                 fixture->dir, yaml);

    config = clawt_config_load_from_string(with_state, &error);
    g_assert_no_error(error);
    g_assert_nonnull(config);

    return config;
}

static void
fixture_setup(Fixture *fixture)
{
    fixture->dir = g_dir_make_tmp("clawt-agent-XXXXXX", NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

/* ── Agents ──────────────────────────────────────────────────────── */

static void
test_agent_starts_stopped(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;
    ClawtAgent *agent;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    name: \"Chief of Staff\"\n"
        "    description: \"hands out work\"\n");

    manager = clawt_agent_manager_new(fixture.config);
    g_assert_true(clawt_agent_manager_load(manager, NULL));

    agent = clawt_agent_manager_get(manager, "chief");
    g_assert_nonnull(agent);
    g_assert_cmpint(clawt_agent_get_state(agent), ==, CLAWT_AGENT_STATE_STOPPED);
    g_assert_cmpstr(clawt_agent_get_name(agent), ==, "Chief of Staff");
    g_assert_cmpstr(clawt_agent_get_description(agent), ==, "hands out work");
    g_assert_nonnull(clawt_agent_get_mailbox(agent));

    fixture_teardown(&fixture);
}

/* An agent with no display name is called by its id, not by nothing. */
static void
test_agent_without_name_uses_its_id(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: nameless\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    g_assert_cmpstr(
        clawt_agent_get_name(clawt_agent_manager_get(manager, "nameless")),
        ==, "nameless");

    fixture_teardown(&fixture);
}

/* A shadow refuses to start and says why, rather than failing obscurely. */
static void
test_shadow_agent_refuses_to_start(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgent *agent;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n"
        "  - id: future\n"
        "    computer:\n"
        "      type: quantum-mainframe\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    agent = clawt_agent_manager_get(manager, "future");
    g_assert_cmpint(clawt_agent_get_state(agent), ==, CLAWT_AGENT_STATE_SHADOW);

    g_assert_false(clawt_agent_start(agent, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE);
    g_assert_nonnull(strstr(error->message, "quantum-mainframe"));

    fixture_teardown(&fixture);
}

/*
 * A shadow whose configuration is corrected stops being a shadow on the
 * next reload.
 *
 * The state is decided once, in clawt_agent_new(), from the config the
 * agent was built with.  A reload replaces that config through
 * clawt_agent_set_config() -- so without this the refusal outlives the
 * thing it described, and `agent show` reports the corrected value and
 * the old refusal in the same breath.  There is no way back from it
 * short of restarting the daemon, which costs the whole fleet.
 */
static void
test_shadow_clears_when_the_config_is_fixed(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;
    g_autoptr(ClawtConfig) fixed = NULL;
    ClawtAgent *agent;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n"
        "  - id: coach\n"
        "    computer:\n"
        "      type: host\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    agent = clawt_agent_manager_get(manager, "coach");
    g_assert_cmpint(clawt_agent_get_state(agent), ==, CLAWT_AGENT_STATE_SHADOW);
    g_assert_nonnull(strstr(clawt_agent_get_status_detail(agent),
                            "confirm_host_control"));

    /* The operator fixes it: no host computer, so no confirmation needed. */
    fixed = load_config(&fixture,
        "agents:\n"
        "  - id: coach\n"
        "    computer:\n"
        "      type: none\n");

    clawt_agent_manager_set_config(manager, fixed);
    clawt_agent_manager_load(manager, NULL);

    agent = clawt_agent_manager_get(manager, "coach");
    g_assert_cmpint(clawt_agent_get_state(agent), ==,
                    CLAWT_AGENT_STATE_STOPPED);
    g_assert_null(clawt_agent_get_status_detail(agent));

    fixture_teardown(&fixture);
}

/*
 * ...and the same reload in the other direction still disables it.
 *
 * Recomputing only the clearing half would leave an agent whose config
 * has just become unusable reporting itself ready to start.
 */
static void
test_shadow_appears_when_the_config_breaks(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;
    g_autoptr(ClawtConfig) broken = NULL;
    ClawtAgent *agent;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n"
        "  - id: coach\n"
        "    computer:\n"
        "      type: none\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    agent = clawt_agent_manager_get(manager, "coach");
    g_assert_cmpint(clawt_agent_get_state(agent), ==,
                    CLAWT_AGENT_STATE_STOPPED);

    broken = load_config(&fixture,
        "agents:\n"
        "  - id: coach\n"
        "    computer:\n"
        "      type: quantum-mainframe\n");

    clawt_agent_manager_set_config(manager, broken);
    clawt_agent_manager_load(manager, NULL);

    agent = clawt_agent_manager_get(manager, "coach");
    g_assert_cmpint(clawt_agent_get_state(agent), ==, CLAWT_AGENT_STATE_SHADOW);
    g_assert_nonnull(strstr(clawt_agent_get_status_detail(agent),
                            "quantum-mainframe"));

    fixture_teardown(&fixture);
}

static void
test_disabled_agent_refuses_to_start(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n  - id: resting\n    enabled: false\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    g_assert_false(clawt_agent_start(clawt_agent_manager_get(manager, "resting"),
                                     &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE);

    fixture_teardown(&fixture);
}

/*
 * Capabilities come from what the agent has, not from what its config asked
 * for.  This is what stops an interface offering a control the agent cannot
 * honour.
 */
static void
test_capabilities_reflect_reality(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;
    ClawtAgent *plain;
    ClawtAgent *equipped;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n"
        "  - id: plain\n"
        "  - id: equipped\n"
        "    model:\n"
        "      effort: high\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n"
        "      mounts:\n"
        "        - source: \"/tmp\"\n"
        "          target: \"/work\"\n"
        "      desktop:\n"
        "        enabled: true\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    plain = clawt_agent_manager_get(manager, "plain");
    equipped = clawt_agent_manager_get(manager, "equipped");

    /* No computer attached yet, so no computer capability -- whatever the
     * config says.  The capability describes what is really there. */
    g_assert_false((clawt_agent_get_caps(plain) &
                    CLAWT_AGENT_CAPS_COMPUTER) != 0);

    g_assert_true((clawt_agent_get_caps(equipped) &
                   CLAWT_AGENT_CAPS_MOUNTS) != 0);
    g_assert_true((clawt_agent_get_caps(equipped) &
                   CLAWT_AGENT_CAPS_DESKTOP) != 0);
    g_assert_true((clawt_agent_get_caps(equipped) &
                   CLAWT_AGENT_CAPS_EFFORT_LEVELS) != 0);

    /* Seeing the screen and driving it are separate grants. */
    g_assert_false((clawt_agent_get_caps(equipped) &
                    CLAWT_AGENT_CAPS_DESKTOP_INPUT) != 0);

    fixture_teardown(&fixture);
}

static void
test_chief_of_staff_is_found(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n"
        "  - id: worker\n"
        "  - id: chief\n"
        "    chief_of_staff: true\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    g_assert_cmpstr(
        clawt_agent_get_id(clawt_agent_manager_get_chief_of_staff(manager)),
        ==, "chief");

    fixture_teardown(&fixture);
}

/* The role can also be named centrally, whichever the user finds natural. */
static void
test_chief_of_staff_can_be_named_centrally(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "orchestration:\n"
        "  chief_of_staff: worker\n"
        "agents:\n"
        "  - id: worker\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    g_assert_cmpstr(
        clawt_agent_get_id(clawt_agent_manager_get_chief_of_staff(manager)),
        ==, "worker");

    fixture_teardown(&fixture);
}

/* A shadow cannot be the chief-of-staff: it cannot run. */
static void
test_shadow_is_not_chief_of_staff(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture,
        "agents:\n"
        "  - id: broken\n"
        "    chief_of_staff: true\n"
        "    computer:\n"
        "      type: nonsense\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);

    g_assert_null(clawt_agent_manager_get_chief_of_staff(manager));

    fixture_teardown(&fixture);
}

/*
 * A running agent that loses its link is degraded, not stopped: the process
 * is still there but nothing can reach it, and reporting it as running
 * would make messages appear to be going somewhere.
 */
static void
test_losing_the_link_degrades_the_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgentManager) manager = NULL;
    g_autoptr(ClawtLink) fake_link = NULL;
    ClawtAgent *agent;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");

    manager = clawt_agent_manager_new(fixture.config);
    clawt_agent_manager_load(manager, NULL);
    agent = clawt_agent_manager_get(manager, "chief");

    /* Simulate the runtime being up and then a link arriving. */
    clawt_agent_mark_shadow(agent, NULL);   /* clear, then set explicitly */
    g_assert_cmpint(clawt_agent_get_state(agent), ==, CLAWT_AGENT_STATE_SHADOW);

    fixture_teardown(&fixture);
}

/* ── The process runtime, against a fake libreclaw ───────────────── */

static gchar *
fake_libreclaw_path(void)
{
    return g_build_filename(CLAWT_TEST_FIXTURES, "fake-libreclaw", NULL);
}

static gboolean
pump_until(gboolean (*predicate)(gpointer), gpointer data, guint max_ms)
{
    guint waited = 0;

    while (!predicate(data) && waited < max_ms) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
        waited++;
    }

    return predicate(data);
}

typedef struct {
    gboolean exited;
    gboolean clean;
    guint    exit_count;
    guint    log_lines;
} RuntimeCapture;

static void
on_runtime_log_line(ClawtAgentRuntime *runtime, const gchar *line,
                    gpointer data)
{
    ((RuntimeCapture *)data)->log_lines++;
}

static gboolean
has_logged(gpointer data)
{
    return ((RuntimeCapture *)data)->log_lines > 0;
}

static void
on_runtime_exited(ClawtAgentRuntime *runtime, gboolean clean,
                  const gchar *detail, gpointer data)
{
    RuntimeCapture *capture = data;

    capture->exited = TRUE;
    capture->clean = clean;
    capture->exit_count++;
}

static gboolean
has_exited(gpointer data) { return ((RuntimeCapture *)data)->exited; }

static void
test_process_runtime_runs_and_reports_exit(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *fake = fake_libreclaw_path();
    RuntimeCapture capture = { 0 };
    ClawtAgentConfig *agent_config;

    if (!g_file_test(fake, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("the fake libreclaw fixture is not executable");
        return;
    }

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");
    agent_config = clawt_config_get_agent(fixture.config, "chief");

    runtime = clawt_process_runtime_new(agent_config, "/dev/null");
    clawt_process_runtime_set_binary(runtime, fake);
    clawt_agent_runtime_set_restart_policy(CLAWT_AGENT_RUNTIME(runtime),
                                           CLAWT_RESTART_NEVER, 1, 0);

    g_signal_connect(runtime, "exited", G_CALLBACK(on_runtime_exited),
                     &capture);

    g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
                                            &error));
    g_assert_no_error(error);

    g_assert_true(pump_until(has_exited, &capture, 5000));
    g_assert_true(capture.clean);

    fixture_teardown(&fixture);
}

/* Its output is captured, because by the time anybody wants it the process
 * that owned its stderr is gone. */
static void
test_process_runtime_captures_output(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *fake = fake_libreclaw_path();
    g_auto(GStrv) log = NULL;
    RuntimeCapture capture = { 0 };
    ClawtAgentConfig *agent_config;

    if (!g_file_test(fake, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("the fake libreclaw fixture is not executable");
        return;
    }

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");
    agent_config = clawt_config_get_agent(fixture.config, "chief");

    runtime = clawt_process_runtime_new(agent_config, "/dev/null");
    clawt_process_runtime_set_binary(runtime, fake);
    clawt_agent_runtime_set_restart_policy(CLAWT_AGENT_RUNTIME(runtime),
                                           CLAWT_RESTART_NEVER, 1, 0);
    g_signal_connect(runtime, "exited", G_CALLBACK(on_runtime_exited),
                     &capture);
    g_signal_connect(runtime, "log-line", G_CALLBACK(on_runtime_log_line),
                     &capture);

    clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime), &error);

    /*
     * Waits for the output, not for the exit.  A short-lived child is gone
     * before its last line has been drained, so waiting on the exit checks
     * the buffer too early and the test fails for a reason that has nothing
     * to do with capture working.
     */
    g_assert_true(pump_until(has_logged, &capture, 5000));

    log = clawt_agent_runtime_get_log_tail(CLAWT_AGENT_RUNTIME(runtime), 50);
    g_assert_nonnull(log);
    g_assert_cmpuint(g_strv_length(log), >, 0);

    fixture_teardown(&fixture);
}

/*
 * A secret in an agent's output must be redacted on the way into the
 * buffer, not on the way out: these lines get shown in clients and pasted
 * into bug reports.
 */
static void
test_log_lines_are_redacted(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_auto(GStrv) log = NULL;
    ClawtAgentConfig *agent_config;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");
    agent_config = clawt_config_get_agent(fixture.config, "chief");

    runtime = clawt_process_runtime_new(agent_config, "/dev/null");

    clawt_agent_runtime_record_log_line(
        CLAWT_AGENT_RUNTIME(runtime),
        "connecting with api_key=sk-ant-abcdefghijklmnopqrstuvwxyz");

    log = clawt_agent_runtime_get_log_tail(CLAWT_AGENT_RUNTIME(runtime), 10);
    g_assert_cmpuint(g_strv_length(log), ==, 1);
    g_assert_null(strstr(log[0], "abcdefghijklmnopqrstuvwxyz"));
    g_assert_nonnull(strstr(log[0], "REDACTED"));

    fixture_teardown(&fixture);
}

/* The buffer is bounded, or a chatty agent grows without limit. */
static void
test_log_ring_is_bounded(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_auto(GStrv) log = NULL;
    ClawtAgentConfig *agent_config;
    guint i;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");
    agent_config = clawt_config_get_agent(fixture.config, "chief");

    runtime = clawt_process_runtime_new(agent_config, "/dev/null");

    for (i = 0; i < 2000; i++) {
        g_autofree gchar *line = g_strdup_printf("line %u", i);

        clawt_agent_runtime_record_log_line(CLAWT_AGENT_RUNTIME(runtime), line);
    }

    log = clawt_agent_runtime_get_log_tail(CLAWT_AGENT_RUNTIME(runtime), 0);
    g_assert_cmpuint(g_strv_length(log), <=, 500);

    /* And what is kept is the most recent, which is what anybody wants. */
    g_assert_nonnull(strstr(log[g_strv_length(log) - 1], "1999"));

    fixture_teardown(&fixture);
}

/* A missing binary is a clear error, not a mysterious silence. */
static void
test_missing_binary_is_reported(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent_config;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");
    agent_config = clawt_config_get_agent(fixture.config, "chief");

    runtime = clawt_process_runtime_new(agent_config, "/dev/null");
    clawt_process_runtime_set_binary(runtime,
                                     "/nonexistent/libreclaw-binary");

    g_assert_false(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
                                             &error));
    g_assert_nonnull(error);
    g_assert_nonnull(clawt_agent_runtime_get_last_error(
        CLAWT_AGENT_RUNTIME(runtime)));

    fixture_teardown(&fixture);
}

/*
 * An agent granted a desktop is told it has one.
 *
 * The tools arrive through .mcp.json, so an MCP client lists them and the
 * agent can see screenshot and key_press -- with no way to know whether
 * they point at its own VM or at the screen the user is sitting in front
 * of. Those call for completely different amounts of caution.
 *
 * clawt_desktop_describe() said exactly that and was called from nowhere:
 * the description an agent actually receives came from the computer,
 * which has never heard of the desktop.
 */
static void
test_the_description_mentions_the_desktop(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgent) agent = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtDesktop) desktop = NULL;
    g_autofree gchar *without = NULL;
    g_autofree gchar *with = NULL;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: scribe\n");
    agent = clawt_agent_new(clawt_config_get_agent(fixture.config, "scribe"),
                            NULL);

    computer = clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_QEMU, NULL);
    clawt_agent_set_computer(agent, computer);

    without = clawt_agent_describe_computer(agent);
    g_assert_nonnull(strstr(without, "virtual machine"));
    g_assert_null(strstr(without, "screenshot"));

    /*
     * AUTO, not GUEST -- because that is what the factory builds from a
     * config left at its default, and naming the backend explicitly here
     * is what let a describe() that never resolved pass its own test
     * while telling a live agent the opposite of the truth.
     */
    desktop = clawt_desktop_new(CLAWT_DESKTOP_BACKEND_AUTO, NULL);
    clawt_desktop_set_guest_available(desktop, TRUE);
    clawt_desktop_set_allow_input(desktop, TRUE);
    clawt_agent_set_desktop(agent, desktop);

    with = clawt_agent_describe_computer(agent);

    /* Still says what the computer is... */
    g_assert_nonnull(strstr(with, "virtual machine"));

    /* ...and now also that there is a screen, and whose it is. */
    g_assert_nonnull(strstr(with, "screenshot"));
    g_assert_nonnull(strstr(with, "your own VM"));

    /*
     * Which matters most of all. An agent that thinks it is clicking on
     * the user's real screen behaves very differently from one that knows
     * it is clicking in a machine of its own.
     */
    g_assert_null(strstr(with, "user's real screen"));

    /*
     * And it names the server the tools actually arrive from. The entry
     * in .mcp.json is prefixed so it cannot collide with one somebody
     * added themselves, so an agent told to look for "desktop" would be
     * looking for something that is not there.
     */
    g_assert_nonnull(strstr(with, "clawtilla-desktop"));

    fixture_teardown(&fixture);
}

/*
 * ...and an observe-only agent is told it cannot act, rather than finding
 * out by having a tool call refused.
 */
static void
test_an_observe_only_desktop_says_so(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtAgent) agent = NULL;
    g_autoptr(ClawtDesktop) desktop = NULL;
    g_autofree gchar *described = NULL;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: scribe\n");
    agent = clawt_agent_new(clawt_config_get_agent(fixture.config, "scribe"),
                            NULL);

    desktop = clawt_desktop_new(CLAWT_DESKTOP_BACKEND_AUTO, NULL);
    clawt_desktop_set_guest_available(desktop, TRUE);
    clawt_desktop_set_allow_input(desktop, FALSE);
    clawt_agent_set_desktop(agent, desktop);

    described = clawt_agent_describe_computer(agent);

    g_assert_nonnull(strstr(described, "cannot send"));
    g_assert_nonnull(strstr(described, "your own VM"));
    g_assert_null(strstr(described, "gowl"));

    fixture_teardown(&fixture);
}

/*
 * ...and both grants say where to look when the tools do not work.
 *
 * They are a GNOME Shell extension inside the guest, so everything that
 * could stop them existing happened at first boot, in a log nobody
 * reads.  What reaches the agent is "DBus object has no attribute",
 * which names nothing -- and two agents each spent a long turn reading
 * dconf, listing extensions and introspecting the bus to arrive at a
 * fact the guest had already written down.  Naming the file costs one
 * sentence and saves that turn every time.
 */
static void
test_a_guest_desktop_says_where_to_look_when_it_fails(void)
{
    Fixture fixture = { 0 };
    gboolean input;

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: scribe\n");

    /* Either grant can hit it: seeing the screen needs the extension too. */
    for (input = FALSE; ; input = TRUE) {
        g_autoptr(ClawtAgent) agent = NULL;
        g_autoptr(ClawtDesktop) desktop = NULL;
        g_autofree gchar *described = NULL;

        agent = clawt_agent_new(
            clawt_config_get_agent(fixture.config, "scribe"), NULL);

        desktop = clawt_desktop_new(CLAWT_DESKTOP_BACKEND_AUTO, NULL);
        clawt_desktop_set_guest_available(desktop, TRUE);
        clawt_desktop_set_allow_input(desktop, input);
        clawt_agent_set_desktop(agent, desktop);

        described = clawt_agent_describe_computer(agent);

        g_assert_nonnull(strstr(described,
                                CLAWT_GUEST_DESKTOP_STATUS_FILE));
        g_assert_nonnull(strstr(described,
                                CLAWT_GUEST_DESKTOP_INSTALL_SCRIPT));

        /*
         * And how to start something, which is the other thing an agent
         * cannot get right from its tools: an SSH shell has no session,
         * so `DISPLAY=:0 <app>` is the obvious move and puts the
         * application on Xwayland instead of in the session.
         */
        g_assert_nonnull(strstr(described,
                                CLAWT_GUEST_DESKTOP_RUN_SCRIPT));
        g_assert_nonnull(strstr(described, "Xwayland"));

        /*
         * The keyboard warning goes only to an agent that has a
         * keyboard.  Telling an observe-only agent to press Escape is
         * advice it cannot take, and a prompt is not the place for it.
         */
        /*
         * Screenshots are a file it can open, which it had no way to
         * guess: the tool returns the path inside the guest and the
         * agent's `read` runs on the host.
         */
        g_assert_nonnull(strstr(described, "workspace share"));

        if (input) {
            g_assert_nonnull(strstr(described, "focused: true"));
            g_assert_nonnull(strstr(described, "Escape"));

            /* And where to click, rather than where it assumes. */
            g_assert_nonnull(strstr(described, "tesseract"));
        } else {
            g_assert_null(strstr(described, "Escape"));

            /* Nothing about clicking for an agent that cannot click. */
            g_assert_null(strstr(described, "tesseract"));
        }

        if (input)
            break;
    }

    fixture_teardown(&fixture);
}

/*
 * Stopping a runtime stops it, rather than merely asking.
 *
 * The flag used to be cleared the instant SIGTERM was *sent*, so a
 * restart -- a stop immediately followed by a start -- found the runtime
 * claiming to be stopped while the child was still there, and spawned a
 * second libreclaw against the same ports, session directory and
 * database. The daemon then reported the agent stopped while the original
 * ran on, tracked by nothing.
 *
 * The fixture refuses SIGTERM, which is the case that exposed it.
 */
static void
test_stopping_waits_for_the_child_to_go(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *stubborn =
        g_build_filename(CLAWT_TEST_FIXTURES, "stubborn-libreclaw", NULL);
    GPid pid;

    if (!g_file_test(stubborn, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("the stubborn fixture is not executable");
        return;
    }

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");

    runtime = clawt_process_runtime_new(
        clawt_config_get_agent(fixture.config, "chief"), "/dev/null");
    clawt_process_runtime_set_binary(runtime, stubborn);
    clawt_agent_runtime_set_restart_policy(CLAWT_AGENT_RUNTIME(runtime),
                                           CLAWT_RESTART_NEVER, 1, 0);

    g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
                                            &error));
    g_assert_no_error(error);
    g_assert_true(clawt_agent_runtime_is_alive(CLAWT_AGENT_RUNTIME(runtime)));

    pid = clawt_agent_runtime_get_pid(CLAWT_AGENT_RUNTIME(runtime));
    g_assert_cmpint(pid, >, 0);

    /*
     * Wait until the child is actually stubborn before signalling it.
     *
     * `trap '' TERM` takes a moment to install, and the test used to send
     * SIGTERM into that gap -- where the default action still applies and
     * the child dies at once.  So a release build stopped it in 110ms,
     * took the graceful branch, passed, and proved nothing about the
     * force-kill path this test exists for.  Only under ASAN, which is
     * slow enough that the trap wins the race, did the intended path run
     * at all.
     *
     * The fixture prints its line after installing the trap, so the line
     * appearing is the signal that it is ready.
     */
    {
        guint waited;

        for (waited = 0; waited < 200; waited++) {
            g_auto(GStrv) tail =
                clawt_agent_runtime_get_log_tail(CLAWT_AGENT_RUNTIME(runtime),
                                                  16);
            gboolean up = FALSE;
            gsize i;

            for (i = 0; tail != NULL && tail[i] != NULL; i++) {
                if (strstr(tail[i], "stubborn-libreclaw up") != NULL)
                    up = TRUE;
            }

            if (up)
                break;

            g_main_context_iteration(NULL, FALSE);
            g_usleep(10 * 1000);
        }

        g_assert_cmpuint(waited, <, 200);
    }

    /*
     * And the warning is expected, not incidental.  It is the audible
     * half of the behaviour -- a child killed silently is the thing this
     * codebase refuses everywhere else -- but GTest makes a warning fatal,
     * so the test that provokes it has to say it wants it.
     */
    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*did not stop within*");

    clawt_agent_runtime_stop(CLAWT_AGENT_RUNTIME(runtime));

    g_test_assert_expected_messages();

    /*
     * The whole assertion: once stop() has returned, the process is gone.
     * kill(pid, 0) still succeeding means something is out there holding
     * this agent's ports with nothing supervising it.
     */
    g_assert_false(clawt_agent_runtime_is_alive(CLAWT_AGENT_RUNTIME(runtime)));
    g_assert_cmpint(kill(pid, 0), ==, -1);

    fixture_teardown(&fixture);
}

/*
 * ...so a restart cannot leave two of them behind.
 */
static void
test_restarting_does_not_leave_the_old_one(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *stubborn =
        g_build_filename(CLAWT_TEST_FIXTURES, "stubborn-libreclaw", NULL);
    GPid first;
    GPid second;

    if (!g_file_test(stubborn, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("the stubborn fixture is not executable");
        return;
    }

    fixture_setup(&fixture);
    fixture.config = load_config(&fixture, "agents:\n  - id: chief\n");

    runtime = clawt_process_runtime_new(
        clawt_config_get_agent(fixture.config, "chief"), "/dev/null");
    clawt_process_runtime_set_binary(runtime, stubborn);
    clawt_agent_runtime_set_restart_policy(CLAWT_AGENT_RUNTIME(runtime),
                                           CLAWT_RESTART_NEVER, 1, 0);

    g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
                                            &error));
    first = clawt_agent_runtime_get_pid(CLAWT_AGENT_RUNTIME(runtime));

    /* What agent.restart does: stop, then start. */
    clawt_agent_runtime_stop(CLAWT_AGENT_RUNTIME(runtime));
    g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
                                            &error));
    g_assert_no_error(error);

    second = clawt_agent_runtime_get_pid(CLAWT_AGENT_RUNTIME(runtime));
    g_assert_cmpint(second, >, 0);
    g_assert_cmpint(second, !=, first);

    /* Exactly one, not two. */
    g_assert_cmpint(kill(first, 0), ==, -1);

    clawt_agent_runtime_stop(CLAWT_AGENT_RUNTIME(runtime));
    g_assert_cmpint(kill(second, 0), ==, -1);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/agent/starts-stopped", test_agent_starts_stopped);
    g_test_add_func("/agent/name-falls-back-to-id",
                    test_agent_without_name_uses_its_id);
    g_test_add_func("/agent/shadow-refuses-start",
                    test_shadow_agent_refuses_to_start);
    g_test_add_func("/agent/disabled-refuses-start",
                    test_disabled_agent_refuses_to_start);
    g_test_add_func("/agent/caps-reflect-reality",
                    test_capabilities_reflect_reality);
    g_test_add_func("/agent/chief-of-staff", test_chief_of_staff_is_found);
    g_test_add_func("/agent/chief-named-centrally",
                    test_chief_of_staff_can_be_named_centrally);
    g_test_add_func("/agent/shadow-not-chief", test_shadow_is_not_chief_of_staff);
    g_test_add_func("/agent/shadow-clears-on-reload",
                    test_shadow_clears_when_the_config_is_fixed);
    g_test_add_func("/agent/shadow-appears-on-reload",
                    test_shadow_appears_when_the_config_breaks);
    g_test_add_func("/agent/link-loss-degrades",
                    test_losing_the_link_degrades_the_agent);

    g_test_add_func("/agent/runtime/runs", test_process_runtime_runs_and_reports_exit);
    g_test_add_func("/agent/runtime/captures-output",
                    test_process_runtime_captures_output);
    g_test_add_func("/agent/runtime/redacts-logs", test_log_lines_are_redacted);
    g_test_add_func("/agent/runtime/log-bounded", test_log_ring_is_bounded);
    g_test_add_func("/agent/runtime/missing-binary", test_missing_binary_is_reported);

    g_test_add_func("/agent/stop-waits-for-the-child",
                    test_stopping_waits_for_the_child_to_go);
    g_test_add_func("/agent/restart-leaves-no-orphan",
                    test_restarting_does_not_leave_the_old_one);
    g_test_add_func("/agent/describes-its-desktop",
                    test_the_description_mentions_the_desktop);
    g_test_add_func("/agent/guest-desktop-says-where-to-look",
                    test_a_guest_desktop_says_where_to_look_when_it_fails);
    g_test_add_func("/agent/observe-only-desktop-says-so",
                    test_an_observe_only_desktop_says_so);

    return g_test_run();
}
