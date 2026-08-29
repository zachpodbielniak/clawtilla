/*
 * test-routine.c - Standing work: what runs, what is missed, what is not
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The runner decides *that* a routine is due and hands it to a callback,
 * which is what makes these answerable without a fleet.  The cases worth
 * holding it to are the ones that only happen at a boundary: a routine
 * added this afternoon must not fire for this morning's slot, and a
 * machine that was asleep must not deliver a stack of good mornings when
 * it wakes.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar              *dir;
    gchar              *state_path;
    ClawtConfig        *config;
    ClawtRoutineRunner *runner;
    GPtrArray          *started;   /* routine ids, in order */
    gboolean            refuse;
} Fixture;

static const gchar *
on_run(const gchar *routine_id, const gchar *agent_id, const gchar *prompt,
       gpointer user_data, GError **error)
{
    Fixture *fixture = user_data;

    (void)agent_id;

    g_assert_nonnull(prompt);

    if (fixture->refuse) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "not today");
        return NULL;
    }

    g_ptr_array_add(fixture->started, g_strdup(routine_id));

    return "task-1";
}

static void
fixture_setup(Fixture *fixture, const gchar *routines)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-routine-XXXXXX", NULL);
    fixture->state_path = g_build_filename(fixture->dir, "routines.yaml",
                                           NULL);
    fixture->started = g_ptr_array_new_with_free_func(g_free);

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
    clawt_routine_runner_set_run_func(fixture->runner, on_run, fixture);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->runner);
    g_clear_object(&fixture->config);
    g_clear_pointer(&fixture->started, g_ptr_array_unref);
    g_clear_pointer(&fixture->state_path, g_free);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

static const gchar DAILY[] =
    "routines:\n"
    "  - id: standup\n"
    "    agent: researcher\n"
    "    instructions: \"Summarise the commits.\"\n"
    "    schedule: daily\n"
    "    at: \"09:00\"\n";

/* ── Running by hand ─────────────────────────────────────────────── */

/*
 * Neither the schedule nor `enabled` is consulted.  Running a disabled
 * routine by hand is the point: it is how somebody tries one before
 * trusting it with a schedule.
 */
static void
test_run_now_ignores_the_schedule(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture,
        "routines:\n"
        "  - id: parked\n"
        "    agent: researcher\n"
        "    instructions: \"Do the thing.\"\n"
        "    schedule: manual\n"
        "    enabled: false\n");

    g_assert_nonnull(clawt_routine_runner_run_now(fixture.runner, "parked",
                                                  &error));
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.started->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.started, 0), ==, "parked");

    fixture_teardown(&fixture);
}

static void
test_running_something_that_is_not_there(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, DAILY);

    g_assert_null(clawt_routine_runner_run_now(fixture.runner, "nope",
                                               &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);

    fixture_teardown(&fixture);
}

/*
 * A failure is remembered as a failure, so the list can say so rather
 * than showing a routine that looks as though it ran.
 */
static void
test_a_failed_run_is_recorded(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtRunState state = CLAWT_RUN_NEVER;
    const gchar *detail = NULL;

    fixture_setup(&fixture, DAILY);
    fixture.refuse = TRUE;

    g_assert_null(clawt_routine_runner_run_now(fixture.runner, "standup",
                                               &error));
    g_assert_nonnull(error);

    g_assert_cmpint(clawt_routine_runner_last_run(fixture.runner, "standup",
                                                  &state, &detail), >, 0);
    g_assert_cmpint(state, ==, CLAWT_RUN_FAILED);
    g_assert_nonnull(detail);
    g_assert_nonnull(strstr(detail, "not today"));

    fixture_teardown(&fixture);
}

/* ── When next ───────────────────────────────────────────────────── */

static void
test_the_next_run_follows_the_preset(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GDateTime) next = NULL;
    g_autofree gchar *formatted = NULL;

    fixture_setup(&fixture, DAILY);

    next = clawt_routine_runner_next_run(fixture.runner, "standup");
    g_assert_nonnull(next);

    formatted = g_date_time_format(next, "%H:%M");
    g_assert_cmpstr(formatted, ==, "09:00");

    fixture_teardown(&fixture);
}

/*
 * A manual or disabled routine has no next time, which is an answer
 * rather than a failure to compute one.
 */
static void
test_manual_and_disabled_have_no_next_run(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
        "routines:\n"
        "  - id: by-hand\n"
        "    agent: researcher\n"
        "    instructions: \"x\"\n"
        "    schedule: manual\n"
        "  - id: paused\n"
        "    agent: researcher\n"
        "    instructions: \"x\"\n"
        "    schedule: daily\n"
        "    enabled: false\n");

    g_assert_null(clawt_routine_runner_next_run(fixture.runner, "by-hand"));
    g_assert_null(clawt_routine_runner_next_run(fixture.runner, "paused"));
    g_assert_null(clawt_routine_runner_next_run(fixture.runner, "absent"));

    fixture_teardown(&fixture);
}

/*
 * A routine whose schedule cannot be parsed disables itself with a
 * warning naming it, rather than taking the rest of them down.
 */
static void
test_a_broken_schedule_is_named_and_skipped(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
        "routines:\n"
        "  - id: broken\n"
        "    agent: researcher\n"
        "    instructions: \"x\"\n"
        "    schedule: custom\n"
        "    cron: \"0 9 * *\"\n");

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*routine 'broken'*");
    g_assert_null(clawt_routine_runner_next_run(fixture.runner, "broken"));
    g_test_assert_expected_messages();

    fixture_teardown(&fixture);
}

/* ── Catching up ─────────────────────────────────────────────────── */

/*
 * A routine that has never run has not missed anything.
 *
 * Without this, adding one fires it immediately -- which is exactly the
 * surprise somebody setting a 09:00 schedule at four in the afternoon
 * does not want.
 */
static void
test_a_new_routine_does_not_fire_at_once(void)
{
    Fixture fixture = { 0 };
    ClawtRunState state = CLAWT_RUN_FAILED;

    fixture_setup(&fixture, DAILY);

    clawt_routine_runner_catch_up(fixture.runner);

    g_assert_cmpuint(fixture.started->len, ==, 0);

    clawt_routine_runner_last_run(fixture.runner, "standup", &state, NULL);
    g_assert_cmpint(state, ==, CLAWT_RUN_NEVER);

    fixture_teardown(&fixture);
}

/*
 * Writes a last-run time far enough back that every schedule has passed
 * since, which is what "the daemon was down" looks like on disk.
 */
static void
seed_last_run(Fixture *fixture, const gchar *id, gint days_ago)
{
    g_autofree gchar *text = NULL;
    gint64 when = (g_get_real_time() / G_USEC_PER_SEC) -
                  ((gint64)days_ago * 24 * 60 * 60);

    text = g_strdup_printf("%s:\n  last_run: %" G_GINT64_FORMAT
                           "\n  state: ok\n", id, when);
    g_assert_true(g_file_set_contents(fixture->state_path, text, -1, NULL));

    /* Reload, since the runner read the file when it was built. */
    g_clear_object(&fixture->runner);
    fixture->runner = clawt_routine_runner_new(fixture->config,
                                               fixture->state_path);
    clawt_routine_runner_set_run_func(fixture->runner, on_run, fixture);
}

/*
 * Missed is deliberately not failed.  A routine that did not run because
 * the machine was asleep is not broken, and showing it as broken would
 * train somebody to ignore the one that is.
 */
static void
test_a_missed_run_is_missed_and_not_failed(void)
{
    Fixture fixture = { 0 };
    ClawtRunState state = CLAWT_RUN_NEVER;
    const gchar *detail = NULL;

    fixture_setup(&fixture, DAILY);
    seed_last_run(&fixture, "standup", 3);

    clawt_routine_runner_catch_up(fixture.runner);

    g_assert_cmpuint(fixture.started->len, ==, 0);

    clawt_routine_runner_last_run(fixture.runner, "standup", &state, &detail);
    g_assert_cmpint(state, ==, CLAWT_RUN_MISSED);
    g_assert_nonnull(detail);
    g_assert_nonnull(strstr(detail, "not running"));

    fixture_teardown(&fixture);
}

/*
 * Once, however many were missed.  A laptop opened after a long weekend
 * should not deliver a stack of good mornings at once.
 */
static void
test_catch_up_runs_exactly_once(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
        "routines:\n"
        "  - id: standup\n"
        "    agent: researcher\n"
        "    instructions: \"Summarise the commits.\"\n"
        "    schedule: daily\n"
        "    at: \"09:00\"\n"
        "    catch_up: true\n");

    /* Three days down is three missed nine o'clocks. */
    seed_last_run(&fixture, "standup", 3);

    clawt_routine_runner_catch_up(fixture.runner);

    g_assert_cmpuint(fixture.started->len, ==, 1);

    fixture_teardown(&fixture);
}

static void
test_a_disabled_routine_never_catches_up(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
        "routines:\n"
        "  - id: standup\n"
        "    agent: researcher\n"
        "    instructions: \"x\"\n"
        "    schedule: daily\n"
        "    catch_up: true\n"
        "    enabled: false\n");

    seed_last_run(&fixture, "standup", 3);
    clawt_routine_runner_catch_up(fixture.runner);

    g_assert_cmpuint(fixture.started->len, ==, 0);

    fixture_teardown(&fixture);
}

/* ── Remembering ─────────────────────────────────────────────────── */

/*
 * Run state lives beside the config, not in it: a clawtilla.yaml that
 * rewrote itself every time a routine fired is one people stop keeping
 * in git.
 */
static void
test_run_state_survives_a_restart(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *config_text = NULL;
    ClawtRunState state = CLAWT_RUN_NEVER;
    gint64 first;

    fixture_setup(&fixture, DAILY);

    g_assert_nonnull(clawt_routine_runner_run_now(fixture.runner, "standup",
                                                  NULL));
    first = clawt_routine_runner_last_run(fixture.runner, "standup", NULL,
                                          NULL);
    g_assert_cmpint(first, >, 0);

    /* A new runner over the same file agrees with the old one. */
    g_clear_object(&fixture.runner);
    fixture.runner = clawt_routine_runner_new(fixture.config,
                                              fixture.state_path);

    g_assert_cmpint(clawt_routine_runner_last_run(fixture.runner, "standup",
                                                  &state, NULL), ==, first);
    g_assert_cmpint(state, ==, CLAWT_RUN_OK);

    /* And nothing was written into the configuration. */
    config_text = clawt_config_to_string(fixture.config);
    g_assert_null(strstr(config_text, "last_run"));

    fixture_teardown(&fixture);
}

/*
 * A state file nobody can parse is a warning and an empty slate.  It is
 * a convenience, and refusing to schedule anything because a timestamp
 * was lost would turn that into a stopped fleet.
 */
static void
test_a_corrupt_state_file_is_survivable(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture, DAILY);

    g_assert_true(g_file_set_contents(fixture.state_path,
                                      "this: is: not: yaml: [", -1, NULL));

    g_clear_object(&fixture.runner);

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*could not be read*");
    fixture.runner = clawt_routine_runner_new(fixture.config,
                                              fixture.state_path);
    g_test_assert_expected_messages();

    g_assert_cmpint(clawt_routine_runner_last_run(fixture.runner, "standup",
                                                  NULL, NULL), ==, 0);

    fixture_teardown(&fixture);
}

/* ── The prompt ──────────────────────────────────────────────────── */

/*
 * An agent that asks a clarifying question at three in the morning has
 * not done the work, and it has no way to know from the instructions
 * alone that this run is different from a person typing.
 */
static void
test_the_prompt_says_nobody_is_watching(void)
{
    Fixture fixture = { 0 };
    ClawtRoutine *routine;
    g_autofree gchar *expression = NULL;

    fixture_setup(&fixture, DAILY);

    routine = clawt_config_get_routine(fixture.config, "standup");
    g_assert_nonnull(routine);
    g_assert_cmpstr(clawt_routine_get_string(routine, "instructions"), ==,
                    "Summarise the commits.");

    /* The preset is the expression, so there is one answer to "when". */
    expression = clawt_routine_get_cron(routine, NULL);
    g_assert_cmpstr(expression, ==, "0 9 * * *");

    fixture_teardown(&fixture);
}

/* ── Jitter ──────────────────────────────────────────────────────── */

/*
 * A cron that is due on every tick, so "is it held back" is the only
 * variable.  A preset schedule would make each of these tests depend on
 * what time the suite happens to run at.
 */
static const gchar EVERY_MINUTE[] =
    "routines:\n"
    "  - id: sweep\n"
    "    agent: researcher\n"
    "    instructions: \"Check the queue.\"\n"
    "    schedule: custom\n"
    "    cron: \"* * * * *\"\n";

static const gchar EVERY_MINUTE_JITTERED[] =
    "routines:\n"
    "  - id: sweep\n"
    "    agent: researcher\n"
    "    instructions: \"Check the queue.\"\n"
    "    schedule: custom\n"
    "    cron: \"* * * * *\"\n"
    "    jitter_seconds: 1\n";

/*
 * Waits for a jittered run, with a watchdog.
 *
 * A test that can hang is worse than one that fails: without the
 * deadline, a jitter that was never armed would park the suite here for
 * ever rather than reporting which assertion did not hold.
 */
static gboolean
wait_for_a_run(Fixture *fixture, guint wanted)
{
    gint64 deadline = g_get_monotonic_time() + (10 * G_USEC_PER_SEC);

    while (fixture->started->len < wanted) {
        if (g_get_monotonic_time() > deadline)
            return FALSE;

        g_main_context_iteration(NULL, FALSE);
        g_usleep(5000);
    }

    return TRUE;
}

/*
 * Without jitter, a due routine starts on the tick that finds it.
 *
 * The control for everything below: if this did not hold, "it was held
 * back" and "it was never due" would be the same observation.
 */
static void
test_a_due_routine_starts_on_the_tick(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture, EVERY_MINUTE);

    clawt_routine_runner_tick(fixture.runner);

    g_assert_cmpuint(fixture.started->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.started, 0), ==, "sweep");

    fixture_teardown(&fixture);
}

/*
 * With jitter, the tick arms it and starts nothing.
 *
 * This is the whole feature, and it is also the half that is invisible:
 * an implementation that read `jitter_seconds` and then started the run
 * anyway would pass every assertion about the run happening. So the
 * assertion is that nothing has happened *yet*, and only then that it
 * eventually does.
 */
static void
test_a_jittered_routine_is_held_and_then_runs(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture, EVERY_MINUTE_JITTERED);

    clawt_routine_runner_tick(fixture.runner);

    /* Held: the tick has been and gone, and nothing started. */
    g_assert_cmpuint(fixture.started->len, ==, 0);

    g_assert_true(wait_for_a_run(&fixture, 1));
    g_assert_cmpuint(fixture.started->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.started, 0), ==, "sweep");

    fixture_teardown(&fixture);
}

/*
 * A second tick inside the jitter window does not arm a second run.
 *
 * The tick is once a minute and `last_run` is only stamped when the run
 * actually starts, so without the armed flag every tick inside a
 * five-minute jitter would find the routine due all over again -- and
 * `jitter_seconds: 300` would fire five runs rather than delaying one.
 */
static void
test_ticking_twice_inside_the_jitter_arms_one_run(void)
{
    Fixture fixture = { 0 };
    guint i;

    fixture_setup(&fixture, EVERY_MINUTE_JITTERED);

    for (i = 0; i < 5; i++)
        clawt_routine_runner_tick(fixture.runner);

    g_assert_cmpuint(fixture.started->len, ==, 0);

    g_assert_true(wait_for_a_run(&fixture, 1));

    /*
     * Settle: anything else that was armed would have fired by now, and
     * five ticks arming five one-second timers would show up here.
     */
    {
        gint64 until = g_get_monotonic_time() + (2 * G_USEC_PER_SEC);

        while (g_get_monotonic_time() < until) {
            g_main_context_iteration(NULL, FALSE);
            g_usleep(5000);
        }
    }

    g_assert_cmpuint(fixture.started->len, ==, 1);

    fixture_teardown(&fixture);
}

/*
 * Running one by hand runs it now, and retires the jitter that was
 * waiting.
 *
 * "Run now" has to mean now -- the IPC reply hands back a task id
 * synchronously, and there is nothing to return if the run is deferred.
 * And the delayed copy has to go, or pressing the button inside the
 * window would produce two runs from one intention.
 */
static void
test_running_by_hand_ignores_and_cancels_the_jitter(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, EVERY_MINUTE_JITTERED);

    clawt_routine_runner_tick(fixture.runner);
    g_assert_cmpuint(fixture.started->len, ==, 0);

    g_assert_nonnull(clawt_routine_runner_run_now(fixture.runner, "sweep",
                                                  &error));
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.started->len, ==, 1);

    /* And the armed one does not arrive on top of it. */
    {
        gint64 until = g_get_monotonic_time() + (3 * G_USEC_PER_SEC);

        while (g_get_monotonic_time() < until) {
            g_main_context_iteration(NULL, FALSE);
            g_usleep(5000);
        }
    }

    g_assert_cmpuint(fixture.started->len, ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A catch-up starts at once, whatever the jitter says.
 *
 * A run that was already missed while the machine was asleep does not
 * need to be later still, and spreading load is not the problem at
 * startup -- there is nobody else on the rate-limited service yet.
 */
static void
test_catch_up_does_not_jitter(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
                  "routines:\n"
                  "  - id: sweep\n"
                  "    agent: researcher\n"
                  "    instructions: \"Check the queue.\"\n"
                  "    schedule: daily\n"
                  "    at: \"09:00\"\n"
                  "    catch_up: true\n"
                  "    jitter_seconds: 600\n");

    seed_last_run(&fixture, "sweep", 3);
    clawt_routine_runner_catch_up(fixture.runner);

    g_assert_cmpuint(fixture.started->len, ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A stopped runner does not still have a run coming.
 *
 * "A stop that only sends a signal is not a stop" applies here too: a
 * timer left armed on a runner somebody asked to stop starts a routine
 * after the answer was no.
 */
static void
test_stopping_disarms_a_pending_jitter(void)
{
    Fixture fixture = { 0 };
    gint64 until;

    fixture_setup(&fixture, EVERY_MINUTE_JITTERED);

    clawt_routine_runner_tick(fixture.runner);
    g_assert_cmpuint(fixture.started->len, ==, 0);

    clawt_routine_runner_stop(fixture.runner);

    until = g_get_monotonic_time() + (3 * G_USEC_PER_SEC);

    while (g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(5000);
    }

    g_assert_cmpuint(fixture.started->len, ==, 0);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/routine/run-now", test_run_now_ignores_the_schedule);
    g_test_add_func("/routine/run-absent",
                    test_running_something_that_is_not_there);
    g_test_add_func("/routine/run-failed", test_a_failed_run_is_recorded);
    g_test_add_func("/routine/next-run",
                    test_the_next_run_follows_the_preset);
    g_test_add_func("/routine/no-next-run",
                    test_manual_and_disabled_have_no_next_run);
    g_test_add_func("/routine/broken-schedule",
                    test_a_broken_schedule_is_named_and_skipped);
    g_test_add_func("/routine/new-does-not-fire",
                    test_a_new_routine_does_not_fire_at_once);
    g_test_add_func("/routine/missed", test_a_missed_run_is_missed_and_not_failed);
    g_test_add_func("/routine/catch-up-once",
                    test_catch_up_runs_exactly_once);
    g_test_add_func("/routine/catch-up-disabled",
                    test_a_disabled_routine_never_catches_up);
    g_test_add_func("/routine/state-survives",
                    test_run_state_survives_a_restart);
    g_test_add_func("/routine/state-corrupt",
                    test_a_corrupt_state_file_is_survivable);
    g_test_add_func("/routine/prompt", test_the_prompt_says_nobody_is_watching);

    g_test_add_func("/routine/due-starts-on-the-tick",
                    test_a_due_routine_starts_on_the_tick);
    g_test_add_func("/routine/jitter-holds-then-runs",
                    test_a_jittered_routine_is_held_and_then_runs);
    g_test_add_func("/routine/jitter-arms-once",
                    test_ticking_twice_inside_the_jitter_arms_one_run);
    g_test_add_func("/routine/jitter-not-for-run-now",
                    test_running_by_hand_ignores_and_cancels_the_jitter);
    g_test_add_func("/routine/jitter-not-for-catch-up",
                    test_catch_up_does_not_jitter);
    g_test_add_func("/routine/jitter-disarmed-by-stop",
                    test_stopping_disarms_a_pending_jitter);

    return g_test_run();
}
