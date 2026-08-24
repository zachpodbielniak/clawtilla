/*
 * test-usage.c - What the fleet has spent, and the budget that reads it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar *dir;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    fixture->dir = g_dir_make_tmp("clawt-usage-XXXXXX", NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

/*
 * Builds an agent state directory holding a libreclaw database with
 * @turns rows in it.
 *
 * Written through libreclaw's own API rather than by CREATE TABLE here,
 * because the whole point of the reader is that it does not carry a copy
 * of that schema -- a fixture that spelled the table itself would keep
 * passing after libreclaw changed it.
 */
static gchar *
make_agent_with_turns(Fixture *fixture, const gchar *agent,
                      guint turns, gint64 cost_micros_each,
                      gint64 recorded_at)
{
    gchar *state_dir = g_build_filename(fixture->dir, agent, NULL);
    g_autofree gchar *db_path = NULL;
    g_autofree gchar *sessions = NULL;
    g_autoptr(LcDatabase) db = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    sessions = g_build_filename(state_dir, "sessions", NULL);
    g_assert_cmpint(g_mkdir_with_parents(sessions, 0700), ==, 0);

    db_path = clawt_usage_database_path(state_dir);
    db = LC_DATABASE(lc_sqlite_database_new());
    g_assert_true(lc_database_open(db, db_path, &error));
    g_assert_no_error(error);

    for (i = 0; i < turns; i++) {
        g_assert_true(lc_database_add_token_usage(
            db, "session-key", "clawtilla", "dm:x:y", "sonnet", "sonnet",
            10, 100, cost_micros_each, recorded_at, &error));
        g_assert_no_error(error);
    }

    lc_database_close(db);

    return state_dir;
}

/*
 * libreclaw's sqlite backend builds its filename from
 * `session.persist_dir`, so the database is inside the sessions
 * directory and not beside it.
 *
 * This is pinned because clawtilla spelled it the other way for a long
 * time: `/reset` tested for `<state_dir>/libreclaw.db`, a file that has
 * never existed on any machine, so its session-clearing branch was
 * skipped every single time and reported nothing cleared.
 */
static void
test_database_path_is_inside_sessions(void)
{
    g_autofree gchar *path = clawt_usage_database_path("/state/agents/bob");

    g_assert_cmpstr(path, ==, "/state/agents/bob/sessions/libreclaw.db");
}

/*
 * An agent that has never run has spent nothing.
 *
 * Reported as a total rather than as a failure -- and without creating
 * the file, because opening a sqlite database brings it into existence
 * and asking what a stopped agent cost should not leave one behind.
 */
static void
test_a_missing_database_is_zero_not_an_error(void)
{
    Fixture fixture;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;
    g_autoptr(GError) error = NULL;
    ClawtUsageTotals totals;

    fixture_setup(&fixture);

    state_dir = g_build_filename(fixture.dir, "never-started", NULL);
    db_path = clawt_usage_database_path(state_dir);

    g_assert_true(clawt_usage_read_totals(db_path, 0, &totals, &error));
    g_assert_no_error(error);
    g_assert_cmpint(totals.turns, ==, 0);
    g_assert_cmpint(totals.cost_micros, ==, 0);
    g_assert_false(g_file_test(db_path, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

static void
test_totals_sum_every_recorded_turn(void)
{
    Fixture fixture;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;
    g_autoptr(GError) error = NULL;
    ClawtUsageTotals totals;

    fixture_setup(&fixture);

    state_dir = make_agent_with_turns(&fixture, "worker", 3, 25000,
                                      g_get_real_time() / G_USEC_PER_SEC);
    db_path = clawt_usage_database_path(state_dir);

    g_assert_true(clawt_usage_read_totals(db_path, 0, &totals, &error));
    g_assert_no_error(error);

    g_assert_cmpint(totals.turns, ==, 3);
    g_assert_cmpint(totals.input_tokens, ==, 30);
    g_assert_cmpint(totals.output_tokens, ==, 300);
    g_assert_cmpint(totals.cost_micros, ==, 75000);

    fixture_teardown(&fixture);
}

/*
 * The window excludes what happened before it.
 *
 * "What did this cost today" is the question somebody asks first, and a
 * filter that quietly returned everything would answer it with a number
 * that only ever grows.
 */
static void
test_since_excludes_older_turns(void)
{
    Fixture fixture;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;
    g_autoptr(LcDatabase) db = NULL;
    g_autoptr(GError) error = NULL;
    ClawtUsageTotals totals;

    fixture_setup(&fixture);

    /* One turn a week ago... */
    state_dir = make_agent_with_turns(&fixture, "worker", 1, 10000,
                                      now - 7 * 86400);

    /* ...and one just now, added to the same database. */
    db_path = clawt_usage_database_path(state_dir);
    db = LC_DATABASE(lc_sqlite_database_new());
    g_assert_true(lc_database_open(db, db_path, &error));
    g_assert_true(lc_database_add_token_usage(
        db, "k", "clawtilla", "r", "sonnet", "sonnet", 10, 100, 40000,
        now, &error));
    lc_database_close(db);

    g_assert_true(clawt_usage_read_totals(db_path, 0, &totals, &error));
    g_assert_cmpint(totals.turns, ==, 2);
    g_assert_cmpint(totals.cost_micros, ==, 50000);

    g_assert_true(clawt_usage_read_totals(db_path, now - 3600, &totals,
                                          &error));
    g_assert_cmpint(totals.turns, ==, 1);
    g_assert_cmpint(totals.cost_micros, ==, 40000);

    fixture_teardown(&fixture);
}

/*
 * The first drain charges nothing.
 *
 * A daemon restarted while a task is running must not bill that task for
 * every turn the agent has ever taken.  An agent with any history at all
 * would exhaust its budget on its first reply, over work that finished
 * days ago.
 */
static void
test_the_first_drain_only_sets_the_watermark(void)
{
    Fixture fixture;
    g_autoptr(ClawtUsage) usage = clawt_usage_new();
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;

    fixture_setup(&fixture);

    state_dir = make_agent_with_turns(&fixture, "worker", 5, 100000,
                                      g_get_real_time() / G_USEC_PER_SEC);
    db_path = clawt_usage_database_path(state_dir);

    g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A drain reports only what arrived since the last one, and reports each
 * turn exactly once.
 *
 * The rows are filtered by `recorded_at`, which is a whole second, so
 * two turns in the same second are indistinguishable by time -- without
 * the row id as a tiebreak the earlier one would be counted again on
 * every drain and a task's budget would drain itself.
 */
static void
test_a_drain_charges_each_turn_once(void)
{
    Fixture fixture;
    g_autoptr(ClawtUsage) usage = clawt_usage_new();
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    fixture_setup(&fixture);

    state_dir = make_agent_with_turns(&fixture, "worker", 1, 10000, now);
    db_path = clawt_usage_database_path(state_dir);

    /* Prime. */
    g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 0);

    /* Three more turns, all stamped the same second. */
    {
        g_autoptr(LcDatabase) db = LC_DATABASE(lc_sqlite_database_new());

        g_assert_true(lc_database_open(db, db_path, &error));

        for (i = 0; i < 3; i++) {
            g_assert_true(lc_database_add_token_usage(
                db, "k", "clawtilla", "r", "sonnet", "sonnet", 10, 100,
                20000, now, &error));
        }

        lc_database_close(db);
    }

    g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 60000);

    /* Nothing new: the same rows must not be charged again. */
    g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * After a reset the database is a different one, numbering from 1 again.
 *
 * A watermark carried over from the old one is above every id in the new
 * one, so the agent would appear to spend nothing for ever.
 */
static void
test_forget_lets_a_replaced_database_be_read(void)
{
    Fixture fixture;
    g_autoptr(ClawtUsage) usage = clawt_usage_new();
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;

    fixture_setup(&fixture);

    state_dir = make_agent_with_turns(&fixture, "worker", 4, 10000, now);
    db_path = clawt_usage_database_path(state_dir);

    g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 0);

    /* What /reset does: the whole sessions directory goes. */
    {
        g_autofree gchar *sessions = g_build_filename(state_dir, "sessions",
                                                      NULL);
        clawt_test_remove_tree(sessions);
    }

    clawt_usage_forget(usage, "worker");

    g_free(state_dir);
    state_dir = make_agent_with_turns(&fixture, "worker", 2, 30000, now);

    /* Priming again, so still nothing... */
    g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 0);

    /* ...but the new rows are visible, which they would not be if the
     * old watermark had survived. */
    {
        g_autoptr(GError) error = NULL;
        ClawtUsageTotals totals;

        g_assert_true(clawt_usage_read_totals(db_path, 0, &totals, &error));
        g_assert_cmpint(totals.turns, ==, 2);
    }

    fixture_teardown(&fixture);
}

/*
 * The budget refuses a message once the task has spent its cap.
 *
 * This is the limit `orchestration.task_budget_usd` has always described
 * and never enforced: the guard checked it correctly throughout, and
 * nothing outside a test had ever called record_spend(), so the counter
 * it reads was permanently zero.  Same shape as the hop limit before it.
 */
static void
test_the_budget_refuses_once_the_task_has_spent_it(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    guint i;

    clawt_loop_guard_set_task_budget(guard, 1.0);

    /*
     * Each message says something different.  The guard also detects a
     * sender repeating itself, so three identical bodies are refused
     * for that reason instead -- which would let this test pass while
     * proving nothing about the budget.
     */
    for (i = 0; i < 3; i++) {
        g_autofree gchar *body = g_strdup_printf("step %u", i);
        g_autoptr(ClawtMessage) message =
            clawt_message_new("worker", "chief", body);

        clawt_message_set_task_id(message, "task-1");

        if (i < 2) {
            g_assert_true(clawt_loop_guard_check(guard, message, &error));
            g_assert_no_error(error);

            /* Two turns at 60 cents: the cap is passed on the second. */
            clawt_loop_guard_record_spend(guard, "task-1", 0.60);
            continue;
        }

        g_assert_false(clawt_loop_guard_check(guard, message, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
    }

    /* The refusal has to say what was spent and what the cap was, or
     * whoever reads it cannot tell a budget from a rate limit. */
    g_assert_nonnull(strstr(error->message, "1.20"));
    g_assert_nonnull(strstr(error->message, "1.00"));
}

/*
 * A cost below a cent is shown as a number, not as $0.00.
 *
 * Per-turn costs here are routinely a fraction of a cent, and a column
 * of zeroes reads as "nothing is being recorded" -- which is the one
 * thing this report exists to disprove.
 */
static void
test_sub_cent_costs_keep_their_digits(void)
{
    g_autofree gchar *tiny = clawt_usage_format_cost(600);
    g_autofree gchar *real = clawt_usage_format_cost(1250000);
    g_autofree gchar *nothing = clawt_usage_format_cost(0);

    g_assert_cmpstr(tiny, ==, "$0.0006");
    g_assert_cmpstr(real, ==, "$1.25");

    /* Genuinely zero stays plain: there is no precision to show. */
    g_assert_cmpstr(nothing, ==, "$0.00");
}

static void
test_totals_add_accumulates_every_field(void)
{
    ClawtUsageTotals a = { 1, 10, 100, 1000 };
    ClawtUsageTotals b = { 2, 20, 200, 2000 };

    clawt_usage_totals_add(&a, &b);

    g_assert_cmpint(a.turns, ==, 3);
    g_assert_cmpint(a.input_tokens, ==, 30);
    g_assert_cmpint(a.output_tokens, ==, 300);
    g_assert_cmpint(a.cost_micros, ==, 3000);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/usage/database-path-is-inside-sessions",
                    test_database_path_is_inside_sessions);
    g_test_add_func("/usage/missing-database-is-zero",
                    test_a_missing_database_is_zero_not_an_error);
    g_test_add_func("/usage/totals-sum-every-turn",
                    test_totals_sum_every_recorded_turn);
    g_test_add_func("/usage/since-excludes-older",
                    test_since_excludes_older_turns);
    g_test_add_func("/usage/first-drain-primes",
                    test_the_first_drain_only_sets_the_watermark);
    g_test_add_func("/usage/drain-charges-each-turn-once",
                    test_a_drain_charges_each_turn_once);
    g_test_add_func("/usage/forget-allows-a-replaced-database",
                    test_forget_lets_a_replaced_database_be_read);
    g_test_add_func("/usage/budget-refuses-when-spent",
                    test_the_budget_refuses_once_the_task_has_spent_it);
    g_test_add_func("/usage/sub-cent-costs-keep-digits",
                    test_sub_cent_costs_keep_their_digits);
    g_test_add_func("/usage/totals-add", test_totals_add_accumulates_every_field);

    return g_test_run();
}
