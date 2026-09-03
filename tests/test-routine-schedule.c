/*
 * test-routine-schedule.c - A routine that will never run, said once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A schedule that cannot be parsed used to be re-discovered on every
 * tick and re-announced every time.  One routine whose `at:` could not
 * be read produced roughly a line a minute for as long as the daemon was
 * up -- 1,368 a day on a daemon nobody restarts -- and it was the
 * dominant line in the unit's log while somebody was diagnosing an
 * unrelated fault on the same machine.
 *
 * Two separate problems in that, and this file holds both to account:
 *
 *   1. A value that cannot be parsed is a load-time fact.  Re-announcing
 *      it every minute tells the operator nothing they did not know
 *      after the first time.
 *   2. It was only a warning, so the routine silently never ran while
 *      every listing reported it as configured.
 *
 * The colon-losing defect this was reported alongside is *not* tested
 * here, because it does not reproduce: a quoted `at:` keeps its colon
 * through the YAML parser, through ClawtConfig, and through a
 * save-and-reload round trip.  The first two cases below are the control
 * that says so, and they would have caught it.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

typedef struct {
    gchar              *dir;
    gchar              *state_path;
    ClawtConfig        *config;
    ClawtRoutineRunner *runner;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *routines)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-rsched-XXXXXX", NULL);
    fixture->state_path = g_build_filename(fixture->dir, "routines.yaml",
                                           NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "agents:\n"
        "  - id: researcher\n"
        "%s",
        fixture->dir, routines);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->runner = clawt_routine_runner_new(fixture->config,
                                               fixture->state_path);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->runner);
    g_clear_object(&fixture->config);
    g_clear_pointer(&fixture->state_path, g_free);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

/* ── Counting what reaches the log ──────────────────────────────── */

/*
 * The schedule complaint is the thing under test, so this binary counts
 * it rather than letting it abort the run.
 *
 * g_log_set_default_handler() rather than g_log_set_writer_func(): the
 * writer is the modern path and it is *not* reached here, because
 * g_test_init() installs a legacy handler and the legacy path wins when
 * one is set.  A capture built on the writer counted zero while the
 * message it was counting printed one line above the failure.
 * set_default_handler is also restorable, which set_writer_func is not
 * -- calling that twice is fatal.
 *
 * The fatal mask comes off only while a capture is active.  g_test_init()
 * makes a warning fatal, which is right for a suite where a warning is a
 * surprise and wrong here, where the warning is the measurement.
 */
typedef struct {
    guint          count;
    gchar         *last;
    GLogLevelFlags fatal;
    GLogFunc       previous;
} LogCapture;

static LogCapture capture;

static void
count_warnings(const gchar   *domain,
               GLogLevelFlags level,
               const gchar   *message,
               gpointer       user_data)
{
    /*
     * Only the schedule complaint is counted, and everything else is
     * passed on.  A test that counted every warning would pass or fail
     * on whatever else the runner happened to say, which is how a
     * counting test stops measuring the thing it was written for.
     */
    if ((level & G_LOG_LEVEL_MASK) == G_LOG_LEVEL_WARNING &&
        message != NULL &&
        strstr(message, "will never run until this is fixed") != NULL) {
        capture.count++;
        g_free(capture.last);
        capture.last = g_strdup(message);
        return;
    }

    if (capture.previous != NULL)
        capture.previous(domain, level, message, user_data);
}

static void
capture_start(void)
{
    capture.count = 0;
    g_clear_pointer(&capture.last, g_free);
    capture.fatal = g_log_set_always_fatal(0);
    capture.previous = g_log_set_default_handler(count_warnings, NULL);
}

static void
capture_stop(void)
{
    g_log_set_default_handler(capture.previous, NULL);
    capture.previous = NULL;
    g_log_set_always_fatal(capture.fatal);
}

/*
 * A quoted time keeps its colon.
 *
 * This is the control for the defect the once-only rule was reported
 * alongside -- `at: "14:25"` arriving at the parser as `1425`.  It does
 * not reproduce, and quoting is the natural thing to write here, so the
 * case is pinned rather than left to be re-reported.
 */
static void
test_a_quoted_time_keeps_its_colon(void)
{
    Fixture fixture = { 0 };
    ClawtRoutine *routine;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *expression = NULL;

    fixture_setup(&fixture,
                  "routines:\n"
                  "  - id: overnight\n"
                  "    agent: researcher\n"
                  "    instructions: \"Summarise.\"\n"
                  "    schedule: daily\n"
                  "    at: \"14:25\"\n");

    routine = clawt_config_get_routine(fixture.config, "overnight");
    g_assert_nonnull(routine);

    g_assert_cmpstr(clawt_routine_get_string(routine, "at"), ==, "14:25");

    expression = clawt_routine_get_cron(routine, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(expression, ==, "25 14 * * *");

    /* And the fleet has nothing to complain about. */
    g_assert_cmpuint(clawt_config_get_warnings(fixture.config)->len, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * Every quoting style, and a save-and-reload round trip.
 *
 * Written as a walk rather than one example because the report's control
 * was that the two working routines were unquoted and the broken one was
 * quoted, so the interesting question is whether the three forms can
 * ever disagree -- including after clawtilla has written the file back
 * itself, which is the path a client edit takes.
 */
static void
test_every_quoting_style_survives_a_round_trip(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *rendered = NULL;
    g_autoptr(ClawtConfig) reloaded = NULL;
    g_autoptr(GError) error = NULL;
    static const gchar *const ids[] = { "plain", "dquoted", "squoted" };
    static const gchar *const expected[] = {
        "45 6 * * *", "25 14 * * *", "5 9 * * *"
    };
    gsize i;

    fixture_setup(&fixture,
                  "routines:\n"
                  "  - id: plain\n"
                  "    agent: researcher\n"
                  "    instructions: \"x\"\n"
                  "    schedule: daily\n"
                  "    at: 06:45\n"
                  "  - id: dquoted\n"
                  "    agent: researcher\n"
                  "    instructions: \"x\"\n"
                  "    schedule: daily\n"
                  "    at: \"14:25\"\n"
                  "  - id: squoted\n"
                  "    agent: researcher\n"
                  "    instructions: \"x\"\n"
                  "    schedule: daily\n"
                  "    at: '09:05'\n");

    rendered = clawt_config_to_string(fixture.config);
    reloaded = clawt_config_load_from_string(rendered, &error);
    g_assert_no_error(error);

    for (i = 0; i < G_N_ELEMENTS(ids); i++) {
        ClawtRoutine *before =
            clawt_config_get_routine(fixture.config, ids[i]);
        ClawtRoutine *after = clawt_config_get_routine(reloaded, ids[i]);
        g_autofree gchar *first = NULL;
        g_autofree gchar *second = NULL;

        g_assert_nonnull(before);
        g_assert_nonnull(after);

        first = clawt_routine_get_cron(before, NULL);
        second = clawt_routine_get_cron(after, NULL);

        g_assert_cmpstr(first, ==, expected[i]);
        g_assert_cmpstr(second, ==, expected[i]);
    }

    fixture_teardown(&fixture);
}

/*
 * A schedule that cannot be read is a fleet warning, once, at load.
 *
 * The warnings array is where every other fleet-level mistake goes -- a
 * routine with no id, two routines sharing one, a selector matching
 * nothing -- and it is read out by the daemon at load and by
 * `config.check`.  Putting this there is what makes it a thing somebody
 * is told rather than a thing they have to notice.
 */
static void
test_an_unreadable_schedule_warns_at_load(void)
{
    Fixture fixture = { 0 };
    GPtrArray *warnings;

    fixture_setup(&fixture,
                  "routines:\n"
                  "  - id: overnight-log\n"
                  "    agent: researcher\n"
                  "    instructions: \"x\"\n"
                  "    schedule: daily\n"
                  "    at: 1425\n");

    warnings = clawt_config_get_warnings(fixture.config);
    g_assert_cmpuint(warnings->len, ==, 1);

    /* It names the routine and the value, which is what makes it fixable. */
    g_assert_nonnull(strstr(g_ptr_array_index(warnings, 0), "overnight-log"));
    g_assert_nonnull(strstr(g_ptr_array_index(warnings, 0), "1425"));

    fixture_teardown(&fixture);
}

/*
 * And the tick says it once, however many times it runs.
 *
 * The limit needs a test that reaches it: this drives twenty ticks, which
 * is twenty minutes of a real daemon, and asserts on the *count* of
 * warnings rather than on any one of them.  Before the fix that number
 * was twenty.
 */
static void
test_the_tick_says_it_once(void)
{
    Fixture fixture = { 0 };
    guint i;

    fixture_setup(&fixture,
                  "routines:\n"
                  "  - id: overnight-log\n"
                  "    agent: researcher\n"
                  "    instructions: \"x\"\n"
                  "    schedule: daily\n"
                  "    at: 1425\n");

    capture_start();

    for (i = 0; i < 20; i++)
        clawt_routine_runner_tick(fixture.runner);

    capture_stop();

    g_assert_cmpuint(capture.count, ==, 1);
    g_assert_nonnull(capture.last);
    g_assert_nonnull(strstr(capture.last, "overnight-log"));

    fixture_teardown(&fixture);
}

/*
 * A routine whose schedule changed to a *different* failure is announced
 * again.
 *
 * The suppression compares the message rather than setting a flag, for
 * exactly this: "already said something about this one" would swallow
 * news.  Somebody fixing the time and getting the weekday wrong must
 * hear about the weekday.
 */
static void
test_a_different_failure_is_announced_again(void)
{
    Fixture fixture = { 0 };
    ClawtRoutine *routine;

    fixture_setup(&fixture,
                  "routines:\n"
                  "  - id: overnight-log\n"
                  "    agent: researcher\n"
                  "    instructions: \"x\"\n"
                  "    schedule: daily\n"
                  "    at: 1425\n");

    capture_start();

    clawt_routine_runner_tick(fixture.runner);
    clawt_routine_runner_tick(fixture.runner);
    g_assert_cmpuint(capture.count, ==, 1);

    /* The time is fixed and the weekday is now the problem. */
    routine = clawt_config_get_routine(fixture.config, "overnight-log");
    clawt_routine_set_string(routine, "schedule", "weekly");
    clawt_routine_set_string(routine, "at", "14:25");
    clawt_routine_set_string(routine, "weekday", "caturday");

    clawt_routine_runner_tick(fixture.runner);
    clawt_routine_runner_tick(fixture.runner);

    capture_stop();

    g_assert_cmpuint(capture.count, ==, 2);
    g_assert_nonnull(strstr(capture.last, "caturday"));

    fixture_teardown(&fixture);
}

/*
 * A routine that can never fire has no next run, which is what the
 * clients draw -- so the reason has to travel beside it.
 *
 * `next_run` being NULL is the same answer a `manual` routine gives, and
 * the GTK client rendered both as "only when you ask": a wrong answer
 * that looks like a working configuration, which is why one sat broken
 * long enough to fill a log.
 */
static void
test_a_broken_routine_has_no_next_run(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GDateTime) next = NULL;
    ClawtRoutine *routine;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *expression = NULL;

    fixture_setup(&fixture,
                  "routines:\n"
                  "  - id: overnight-log\n"
                  "    agent: researcher\n"
                  "    instructions: \"x\"\n"
                  "    schedule: daily\n"
                  "    at: 1425\n");

    next = clawt_routine_runner_next_run(fixture.runner, "overnight-log");
    g_assert_null(next);

    /*
     * And the reason is available to whoever renders that NULL, which is
     * what routine.list reports as `problem`.
     */
    routine = clawt_config_get_routine(fixture.config, "overnight-log");
    expression = clawt_routine_get_cron(routine, &error);

    g_assert_null(expression);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "1425"));

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/routine-schedule/a-quoted-time-keeps-its-colon",
                    test_a_quoted_time_keeps_its_colon);
    g_test_add_func("/routine-schedule/every-quoting-style-round-trips",
                    test_every_quoting_style_survives_a_round_trip);
    g_test_add_func("/routine-schedule/unreadable-schedule-warns-at-load",
                    test_an_unreadable_schedule_warns_at_load);
    g_test_add_func("/routine-schedule/the-tick-says-it-once",
                    test_the_tick_says_it_once);
    g_test_add_func("/routine-schedule/a-different-failure-is-said-again",
                    test_a_different_failure_is_announced_again);
    g_test_add_func("/routine-schedule/a-broken-routine-has-no-next-run",
                    test_a_broken_routine_has_no_next_run);

    return g_test_run();
}
