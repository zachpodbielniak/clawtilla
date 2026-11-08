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

/* A new fleet may have no database until its first turn finishes. */
static void
test_prime_missing_database_charges_first_turn(void)
{
	Fixture fixture;
	g_autoptr(ClawtUsage) usage = clawt_usage_new();
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *db_path = NULL;
	g_autofree gchar *created = NULL;
	g_autoptr(GError) error = NULL;

	fixture_setup(&fixture);
	state_dir = g_build_filename(fixture.dir, "worker", NULL);
	db_path = clawt_usage_database_path(state_dir);
	g_assert_true(clawt_usage_prime(usage, "worker", db_path, &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(db_path, G_FILE_TEST_EXISTS));
	created = make_agent_with_turns(&fixture, "worker", 1, 125000,
		g_get_real_time() / G_USEC_PER_SEC);
	g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 125000);
	g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 0);
	fixture_teardown(&fixture);
}

/* Re-priming on a reconnect must not advance the original historical baseline. */
static void
test_prime_preserves_pending_spend(void)
{
	Fixture fixture;
	g_autoptr(ClawtUsage) usage = clawt_usage_new();
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *db_path = NULL;
	g_autoptr(LcDatabase) db = LC_DATABASE(lc_sqlite_database_new());
	g_autoptr(GError) error = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	fixture_setup(&fixture);
	state_dir = make_agent_with_turns(&fixture, "worker", 3, 1000000, now);
	db_path = clawt_usage_database_path(state_dir);
	g_assert_true(clawt_usage_prime(usage, "worker", db_path, &error));
	g_assert_no_error(error);
	g_assert_true(lc_database_open(db, db_path, &error));
	g_assert_true(lc_database_add_token_usage(db, "new", "clawtilla", "room",
		"model", "model", 1, 1, 250000, now, &error));
	g_assert_no_error(error);
	lc_database_close(db);
	g_assert_true(clawt_usage_prime(usage, "worker", db_path, &error));
	g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 250000);
	g_assert_cmpint(clawt_usage_drain(usage, "worker", db_path), ==, 0);
	fixture_teardown(&fixture);
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

/*
 * The hop refusal has to fit what the agent was doing.
 *
 * "Answer directly rather than passing it on again" is right for a
 * delegation chain and useless for a conversation: three agents in a
 * room each reply one hop deeper, so an ordinary exchange reaches the
 * ceiling on its own -- and every one of them was already answering
 * directly.  An agent told to do the thing it is doing has nothing to
 * act on, which is how a refusal gets retried in a different shape.
 *
 * Found by running a real fleet: 111 refusals in one session, all of
 * them a standup room rather than a delegation.
 */
static void
test_a_room_refusal_says_it_is_a_room(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    /* clawt_message_new() takes the room first, then the sender. */
    g_autoptr(ClawtMessage) in_room = clawt_message_new("standup", "chief",
                                                        "hello");
    g_autoptr(ClawtMessage) delegated =
        clawt_message_new("dm:chief:worker", "chief", "do this");
    g_autoptr(GError) room_error = NULL;
    g_autoptr(GError) chain_error = NULL;

    clawt_loop_guard_set_limits(guard, 2, 0, 0);

    clawt_message_set_depth(in_room, 4);
    g_assert_false(clawt_loop_guard_check(guard, in_room, &room_error));
    g_assert_nonnull(room_error);
    g_assert_nonnull(strstr(room_error->message, "This is a room"));
    g_assert_null(strstr(room_error->message, "Delegation has gone"));

    /*
     * And a real delegation chain still gets the advice that fits it --
     * without this the fix would just be a different wrong answer.
     */
    clawt_message_set_depth(delegated, 4);
    g_assert_false(clawt_loop_guard_check(guard, delegated, &chain_error));
    g_assert_nonnull(chain_error);
    g_assert_nonnull(strstr(chain_error->message, "Delegation has gone"));
}

/*
 * The fleet view's read: the period's totals, the last turn ever (even
 * when it is older than the period -- that is the idle agent), and the
 * spend per local day.  Turns at three ages: 10 days ago, yesterday and
 * now; a 7-day window and a period of the last 2 days.
 */
static void
test_report_reads_last_turn_and_days(void)
{
    Fixture fixture;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;
    g_autofree gint64 *starts = NULL;
    g_autoptr(GError) error = NULL;
    ClawtUsageTotals totals;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 daily[7];
    gint64 last = -1;
    g_autoptr(LcDatabase) db = NULL;
    guint d;
    gint64 sum = 0;

    fixture_setup(&fixture);
    state_dir = make_agent_with_turns(&fixture, "worker", 1, 1000,
                                      now - 10 * 86400);
    db_path = clawt_usage_database_path(state_dir);

    /* more rows, into the same database */
    db = LC_DATABASE(lc_sqlite_database_new());
    g_assert_true(lc_database_open(db, db_path, &error));
    g_assert_true(lc_database_add_token_usage(db, "k", "clawtilla", "r", "p", "m",
        1, 1, 20000, now - 86400, &error));
    g_assert_true(lc_database_add_token_usage(db, "k", "clawtilla", "r", "p", "m",
        1, 1, 300000, now, &error));
    lc_database_close(db);

    starts = clawt_usage_day_starts(now, 7);
    g_assert_nonnull(starts);
    g_assert_cmpint(starts[0], <, starts[7]);
    g_assert_cmpint(starts[6], <=, now);
    g_assert_cmpint(now, <, starts[7]);

    g_assert_true(clawt_usage_read_report(db_path, now - 2 * 86400, starts, 7,
                                          &totals, &last, daily, &error));
    g_assert_no_error(error);

    /* the period: yesterday and today, not 10 days ago */
    g_assert_cmpint(totals.turns, ==, 2);
    g_assert_cmpint(totals.cost_micros, ==, 320000);
    /* the last turn is the newest ever */
    g_assert_cmpint(last, ==, now);
    /* today is the last day; the 10-day-old turn is outside the window */
    g_assert_cmpint(daily[6], ==, 300000);
    for (d = 0; d < 7; d++)
        sum += daily[d];
    g_assert_cmpint(sum, ==, 320000);

    fixture_teardown(&fixture);
}

/* A database that is not there: never used, nothing spent, nothing created. */
static void
test_report_of_a_missing_database(void)
{
    Fixture fixture;
    g_autofree gchar *db_path = NULL;
    g_autofree gint64 *starts = NULL;
    g_autoptr(GError) error = NULL;
    ClawtUsageTotals totals;
    gint64 daily[3] = { 9, 9, 9 };
    gint64 last = -1;

    fixture_setup(&fixture);
    db_path = clawt_usage_database_path(fixture.dir);
    starts = clawt_usage_day_starts(g_get_real_time() / G_USEC_PER_SEC, 3);

    g_assert_true(clawt_usage_read_report(db_path, 0, starts, 3, &totals, &last,
                                          daily, &error));
    g_assert_no_error(error);
    g_assert_cmpint(last, ==, 0);
    g_assert_cmpint(daily[0] + daily[1] + daily[2], ==, 0);
    g_assert_false(g_file_test(db_path, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

static void
test_sparkline_and_share(void)
{
    gint64 days[4] = { 0, 10, 80, 40 };
    g_autofree gchar *strip = clawt_usage_sparkline(days, 4);
    g_autofree gchar *none = clawt_usage_sparkline(days, 1);
    g_autofree gchar *half = clawt_usage_share_label(50, 100);
    g_autofree gchar *sliver = clawt_usage_share_label(1, 1000);
    g_autofree gchar *nothing = clawt_usage_share_label(0, 1000);

    /* a gap, the smallest bar (never a gap), the tallest, a middle one */
    g_assert_cmpstr(strip, ==, "\xc2\xb7\xe2\x96\x81\xe2\x96\x88\xe2\x96\x84");
    g_assert_cmpstr(none, ==, "\xc2\xb7");
    g_assert_cmpstr(half, ==, "50%");
    g_assert_cmpstr(sliver, ==, "<1%");
    g_assert_cmpstr(nothing, ==, "0%");
}

/* The Fleet page's lines, from a reply shaped as usage.summary sends it. */
static void
test_fleet_page_lines(void)
{
    static const gchar reply_text[] =
        "{\"agents\":["
        " {\"id\":\"busy\",\"turns\":12,\"cost_micros\":900000,"
        "  \"last_turn_at\":999000,\"idle\":\"active\",\"state\":\"running\"},"
        " {\"id\":\"quiet\",\"turns\":1,\"cost_micros\":1000,"
        "  \"last_turn_at\":740800,\"idle\":\"idle\",\"state\":\"running\","
        "  \"idle_stop_hours\":24},"
        " {\"id\":\"nap\",\"turns\":0,\"cost_micros\":0,\"last_turn_at\":0,"
        "  \"idle\":\"never used\",\"state\":\"stopped\",\"asleep\":true}],"
        "\"total\":{\"turns\":13,\"cost_micros\":901000},"
        "\"days\":[{\"start\":1,\"cost_micros\":0},{\"start\":2,\"cost_micros\":901000}],"
        "\"daily_fleet_budget_micros\":500000}";
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *reply;
    JsonArray *agents;
    g_autofree gchar *summary = NULL;
    g_autofree gchar *budget = NULL;
    g_autofree gchar *strip = NULL;
    g_autofree gchar *busy = NULL;
    g_autofree gchar *quiet = NULL;
    g_autofree gchar *nap = NULL;
    g_autoptr(JsonNode) payload = clawt_usage_fleet_payload(100, 14);

    g_assert_true(json_parser_load_from_data(parser, reply_text, -1, NULL));
    reply = json_node_get_object(json_parser_get_root(parser));
    agents = json_object_get_array_member(reply, "agents");

    summary = clawt_usage_fleet_summary(reply);
    g_assert_cmpstr(summary, ==, "$0.90 across 13 turns");
    budget = clawt_usage_fleet_budget_line(reply);
    g_assert_cmpstr(budget, ==,
                    "Today $0.90 -- the $0.50 daily cap is reached; new work waits");
    strip = clawt_usage_fleet_strip(reply);
    g_assert_cmpstr(strip, ==, "\xc2\xb7\xe2\x96\x88");

    busy = clawt_usage_fleet_row_subtitle(json_array_get_object_element(agents, 0),
                                          1000000);
    quiet = clawt_usage_fleet_row_subtitle(json_array_get_object_element(agents, 1),
                                           1000000);
    nap = clawt_usage_fleet_row_subtitle(json_array_get_object_element(agents, 2),
                                         1000000);
    g_assert_cmpstr(busy, ==, "12 turns \xc2\xb7 last turn 16m ago");
    g_assert_cmpstr(quiet, ==, "1 turn \xc2\xb7 last turn 3d ago \xc2\xb7 sleeps after 24h");
    g_assert_cmpstr(nap, ==, "0 turns \xc2\xb7 last turn never \xc2\xb7 asleep until a message");

    /* Stop on a running agent that is not doing anything, and only there */
    g_assert_false(clawt_usage_fleet_row_stoppable(json_array_get_object_element(agents, 0)));
    g_assert_true(clawt_usage_fleet_row_stoppable(json_array_get_object_element(agents, 1)));
    g_assert_false(clawt_usage_fleet_row_stoppable(json_array_get_object_element(agents, 2)));
    g_assert_cmpuint(clawt_usage_fleet_stoppable_count(reply), ==, 1);

    /* the request: the period, the days, sorted by cost */
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(payload), "sort"),
                    ==, "cost");
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(payload), "days"),
                    ==, 14);
}

static void
test_idle_rules(void)
{
    gint64 now = 1000000000;
    g_autofree gchar *never = clawt_usage_last_turn_label(0, now);
    g_autofree gchar *ago = clawt_usage_last_turn_label(now - 3 * 86400, now);

    g_assert_cmpint(clawt_usage_idle_state(0, now, 72), ==, CLAWT_IDLE_NEVER);
    g_assert_cmpint(clawt_usage_idle_state(now - 3600, now, 72), ==, CLAWT_IDLE_ACTIVE);
    g_assert_cmpint(clawt_usage_idle_state(now - 72 * 3600, now, 72), ==, CLAWT_IDLE_IDLE);
    /* 0 calls nothing idle */
    g_assert_cmpint(clawt_usage_idle_state(now - 400 * 86400, now, 0), ==, CLAWT_IDLE_ACTIVE);

    g_assert_cmpstr(never, ==, "never");
    g_assert_cmpstr(ago, ==, "3d ago");

    /* every state has a label and a tone the stylesheets paint */
    g_assert_cmpstr(clawt_usage_idle_label(CLAWT_IDLE_IDLE), ==, "idle");
    g_assert_cmpstr(clawt_usage_idle_tone(CLAWT_IDLE_IDLE), ==, "warn");
    g_assert_cmpstr(clawt_usage_idle_tone(CLAWT_IDLE_ACTIVE), ==, "good");
    g_assert_cmpstr(clawt_usage_idle_tone(CLAWT_IDLE_NEVER), ==, "neutral");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/usage/report-last-turn-and-days",
                    test_report_reads_last_turn_and_days);
    g_test_add_func("/usage/report-missing-database",
                    test_report_of_a_missing_database);
    g_test_add_func("/usage/idle-rules", test_idle_rules);
    g_test_add_func("/usage/sparkline-and-share", test_sparkline_and_share);
    g_test_add_func("/usage/fleet-page-lines", test_fleet_page_lines);
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
	g_test_add_func("/usage/prime-missing-database", test_prime_missing_database_charges_first_turn);
	g_test_add_func("/usage/prime-preserves-pending", test_prime_preserves_pending_spend);
    g_test_add_func("/usage/drain-charges-each-turn-once",
                    test_a_drain_charges_each_turn_once);
    g_test_add_func("/usage/forget-allows-a-replaced-database",
                    test_forget_lets_a_replaced_database_be_read);
    g_test_add_func("/usage/budget-refuses-when-spent",
                    test_the_budget_refuses_once_the_task_has_spent_it);
    g_test_add_func("/usage/sub-cent-costs-keep-digits",
                    test_sub_cent_costs_keep_their_digits);
    g_test_add_func("/usage/totals-add", test_totals_add_accumulates_every_field);
    g_test_add_func("/usage/room-refusal-says-it-is-a-room",
                    test_a_room_refusal_says_it_is_a_room);


    return g_test_run();
}
