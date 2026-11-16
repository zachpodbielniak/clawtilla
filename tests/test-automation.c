/*
 * test-automation.c - What loaded, what did not, and who can see it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * ClawtAutomation had no test at all, and its two accessors have no
 * caller: the daemon owns the object and a client reaches the daemon
 * over IPC, so nothing outside src/plugin can ask either question.
 * clawt_automation_get_problems()'s own header said it was kept "so a
 * client can show that a pod file is broken", which was never reachable
 * -- the header says what is actually true now, and docs/automation.org
 * has always been honest that the daemon's log is the whole report.
 *
 * What that leaves is state nothing reads, which is the state most
 * likely to be quietly wrong on the day somebody finally reads it.  So
 * the contract is pinned here: one entry per file that would not load,
 * naming the file; nothing for a file that did; and a reload replaces
 * the list rather than appending to it.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    ClawtConfig     *config;
    ClawtEventBus   *bus;
    ClawtAutomation *automation;
    gchar           *dir;      /* the fixture's own root */
    gchar           *pods;     /* daemon.automation_dir, resolved */
} Fixture;

/*
 * A pod's actions never run here -- nothing fires an event -- but the
 * module refuses to be built without somewhere to send one.
 */
static gboolean
never_called(const gchar *action, GHashTable *params,
             GHashTable **out_result, gpointer user_data, GError **error)
{
    (void)action;
    (void)params;
    (void)out_result;
    (void)user_data;

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "no action should have run in this test");
    return FALSE;
}

/*
 * The directory comes out of a real config through
 * clawt_config_get_path_value(), which is the route
 * clawt_daemon_start() takes -- a fixture that built the path itself
 * would pass with `daemon.automation_dir` spelled any way at all.
 *
 * state_dir, socket and workspace_root are pinned for the reason
 * CLAUDE.md gives: workspace_root defaults to ~/.clawtilla/agents, and a
 * fixture that takes the default scaffolds into the real fleet.
 */
static void
fixture_setup(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-pods-XXXXXX", NULL);
    g_assert_nonnull(fixture->dir);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "  tailscale: false\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture->config);

    fixture->pods = clawt_config_get_path_value(fixture->config,
                                                "daemon.automation_dir");
    g_assert_nonnull(fixture->pods);

    fixture->bus = clawt_event_bus_new(16);
    fixture->automation = clawt_automation_new(fixture->bus, NULL,
                                               never_called, NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    clawt_automation_stop(fixture->automation);

    g_clear_object(&fixture->automation);
    g_clear_object(&fixture->bus);
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->pods, g_free);
    g_clear_pointer(&fixture->dir, g_free);
}

static void
write_pod(Fixture *fixture, const gchar *name, const gchar *text)
{
    g_autofree gchar *path = g_build_filename(fixture->pods, name, NULL);

    g_assert_true(clawt_ensure_dir(fixture->pods, 0700, NULL));
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/*
 * A pod that parses.  `on_agent_state` is one of the module's own
 * events and the action is never reached, because nothing here
 * publishes anything.
 */
static const gchar *const GOOD_POD =
    "pod watcher = clawtilla->new(\"researcher\");\n"
    "watcher->on_agent_state =>\n"
    "    clawtilla->notify(title: \"a state changed\");\n";

/*
 * The dot is the one the DSL cannot parse, and it is the mistake
 * docs/automation.org warns about first.
 */
static const gchar *const BAD_POD =
    "pod broken = clawtilla->new(\"researcher\");\n"
    "broken->agent.state =>\n"
    "    clawtilla->notify(title: \"never\");\n";

/*
 * A file that does not parse is one problem, naming the file.
 *
 * The name is the assertion, not the count: a message that says
 * something failed without saying which file leaves somebody reading
 * every pod they have.
 */
static void
test_a_pod_that_does_not_parse_is_a_problem_naming_the_file(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    GLogLevelFlags fatal;
    GPtrArray *problems;
    gboolean named = FALSE;
    guint i;

    fixture_setup(&fixture);
    write_pod(&fixture, "broken.pod", BAD_POD);

    /*
     * The load warns about the file it could not read, which is the
     * behaviour being pinned -- so the warning is swallowed rather than
     * avoided, and always_fatal is put straight back.
     */
    fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
    g_assert_true(clawt_automation_load(fixture.automation, fixture.pods,
                                        &error));
    g_log_set_always_fatal(fatal);
    g_assert_no_error(error);

    problems = clawt_automation_get_problems(fixture.automation);
    g_assert_nonnull(problems);
    g_assert_cmpuint(problems->len, ==, 1);

    for (i = 0; i < problems->len; i++) {
        if (strstr(g_ptr_array_index(problems, i), "broken.pod") != NULL)
            named = TRUE;
    }

    g_assert_true(named);

    fixture_teardown(&fixture);
}

/*
 * And a file that does parse is not a problem.
 *
 * Without this the test above passes against a get_problems() that
 * reports every file it saw.
 */
static void
test_a_pod_that_parses_is_not_a_problem(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) pods = NULL;
    GPtrArray *problems;

    fixture_setup(&fixture);
    write_pod(&fixture, "watcher.pod", GOOD_POD);

    g_assert_true(clawt_automation_load(fixture.automation, fixture.pods,
                                        &error));
    g_assert_no_error(error);

    problems = clawt_automation_get_problems(fixture.automation);
    g_assert_nonnull(problems);
    g_assert_cmpuint(problems->len, ==, 0);

    /* The other accessor with no caller, asked the matching question. */
    pods = clawt_automation_list_pods(fixture.automation);
    g_assert_nonnull(pods);
    g_assert_cmpstr(pods[0], ==, "watcher");
    g_assert_null(pods[1]);

    fixture_teardown(&fixture);
}

/*
 * A second load is refused, and refusing it does not throw away the
 * first load's report.
 *
 * The engine underneath will not start twice, and it says so with
 * "Engine is not in IDLE state (current: 1)" -- a sentence about
 * podomation, arriving under an "automation:" prefix as though a pod
 * file were at fault.  Worse, clawt_automation_load() cleared the
 * problems array before it got there, so the second call wiped the
 * report of the first and then failed: whatever was wrong with somebody's
 * pods, the answer afterwards was "no problems".
 */
static void
test_a_second_load_is_refused_and_keeps_the_first_report(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(GError) again = NULL;
    GLogLevelFlags fatal;
    GPtrArray *problems;

    fixture_setup(&fixture);
    write_pod(&fixture, "broken.pod", BAD_POD);

    fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
    g_assert_true(clawt_automation_load(fixture.automation, fixture.pods,
                                        &error));
    g_log_set_always_fatal(fatal);
    g_assert_no_error(error);
    g_assert_cmpuint(
        clawt_automation_get_problems(fixture.automation)->len, ==, 1);

    g_assert_false(clawt_automation_load(fixture.automation, fixture.pods,
                                         &again));
    g_assert_error(again, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    /* And the one problem the first load found is still there. */
    problems = clawt_automation_get_problems(fixture.automation);
    g_assert_nonnull(problems);
    g_assert_cmpuint(problems->len, ==, 1);
    g_assert_nonnull(strstr(g_ptr_array_index(problems, 0), "broken.pod"));

    fixture_teardown(&fixture);
}

/*
 * A directory that is not there is not a failure and not a problem.
 *
 * Most fleets have no automation at all, and creating the directory to
 * hold nothing would be a file appearing in somebody's home for a
 * feature they never used.
 */
static void
test_a_missing_directory_is_not_a_problem(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *absent = NULL;

    fixture_setup(&fixture);
    absent = g_build_filename(fixture.dir, "no-such-directory", NULL);

    g_assert_true(clawt_automation_load(fixture.automation, absent,
                                        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(
        clawt_automation_get_problems(fixture.automation)->len, ==, 0);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/automation/unparsable-pod-names-its-file",
                    test_a_pod_that_does_not_parse_is_a_problem_naming_the_file);
    g_test_add_func("/automation/good-pod-is-not-a-problem",
                    test_a_pod_that_parses_is_not_a_problem);
    g_test_add_func("/automation/second-load-refused",
                    test_a_second_load_is_refused_and_keeps_the_first_report);
    g_test_add_func("/automation/missing-directory",
                    test_a_missing_directory_is_not_a_problem);

    return g_test_run();
}
