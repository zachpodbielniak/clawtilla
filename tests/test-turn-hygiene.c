/*
 * test-turn-hygiene.c - Budgets, repeat counters and steering
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three limits and one queue, and the thing they have in common is that
 * each of them is easy to write in a way that never fires.  So every
 * test here **reaches** its limit rather than asserting that the code
 * exists: a threshold is checked by landing exactly on it, an eviction
 * by filling the table past its bound, a grace timer by letting it
 * elapse.
 *
 * The daemon half includes core/clawt-daemon-private.h directly, because
 * that header *is* the interface of src/core/daemon-turn.c.  Reaching it
 * through anything else would be testing a different thing -- there is
 * no way in through the IPC surface, since settling a turn is something
 * libreclaw's typing frame does and a hermetic test has no libreclaw.
 * The build already defines CLAWT_COMPILATION for every test, which is
 * what lets the include through the header's own guard.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

/*
 * Every nested wait in this file is bounded.  A test that can hang is
 * worse than one that fails: it takes the whole suite with it and the
 * failure says nothing about what went wrong.
 */
#define WAIT_ITERATIONS (2000)

/* ── Repeat detection ────────────────────────────────────────────── */

typedef struct {
    guint   fired;
    gchar  *key;
    guint   count;
} ThresholdReport;

static void
on_threshold(ClawtRepeatWatch *watch, const gchar *turn_id, const gchar *key,
             guint count, gpointer user_data)
{
    ThresholdReport *report = user_data;

    (void)watch;
    (void)turn_id;

    report->fired++;
    g_free(report->key);
    report->key = g_strdup(key);
    report->count = count;
}

/*
 * A bare tool name is not a call worth counting.
 *
 * Five `bash` calls may be five different commands, and reporting them
 * as a loop is the false positive that teaches somebody to stop reading
 * these reports at all.
 */
static void
test_a_bare_tool_name_is_not_a_key(void)
{
    g_autofree gchar *bare = clawt_repeat_key("bash", NULL);
    g_autofree gchar *blank = clawt_repeat_key("bash", "   ");
    g_autofree gchar *real = clawt_repeat_key("bash", "ls -l");

    g_assert_null(bare);
    g_assert_null(blank);
    g_assert_nonnull(real);
}

/*
 * Runs of whitespace collapse, and leading and trailing whitespace goes.
 *
 * Pretty-printed arguments and their one-line form are the same call.
 * Runs rather than *all* whitespace, deliberately: `ls -l` and `ls-l`
 * are different commands, and a key that could not tell them apart would
 * report a loop that is not there.
 */
static void
test_the_key_collapses_whitespace(void)
{
    g_autofree gchar *tight = clawt_repeat_key("exec", "{\"cmd\": \"ls\"}");
    g_autofree gchar *loose = clawt_repeat_key("exec",
                                               "  {\"cmd\":\n\t\"ls\"}  ");
    g_autofree gchar *spaced = clawt_repeat_key("exec", "ls -l");
    g_autofree gchar *joined = clawt_repeat_key("exec", "ls-l");

    g_assert_cmpstr(tight, ==, loose);
    g_assert_cmpstr(spaced, !=, joined);
}

/*
 * The thresholds fire on the count that lands on them and on no other.
 *
 * Reporting every repeat past a floor turns one signal into a hundred,
 * and a hundred is noise. The 6th and the 21st are the assertions that
 * matter -- a "greater than" would pass everything else in this test.
 */
static void
test_thresholds_fire_exactly_on_5_10_and_20(void)
{
    g_autoptr(ClawtRepeatWatch) watch = clawt_repeat_watch_new();
    ThresholdReport report = { 0 };
    guint call;

    g_signal_connect(watch, "threshold", G_CALLBACK(on_threshold), &report);

    for (call = 1; call <= 21; call++) {
        guint landed = clawt_repeat_watch_note(watch, "chief", "exec",
                                               "{\"cmd\":\"ls\"}");

        switch (call) {
        case 5:
        case 10:
        case 20:
            g_assert_cmpuint(landed, ==, call);
            break;

        default:
            g_assert_cmpuint(landed, ==, 0);
            break;
        }
    }

    g_assert_cmpuint(report.fired, ==, 3);
    g_assert_cmpuint(report.count, ==, 20);
    g_assert_cmpstr(report.key, ==, "exec:{\"cmd\":\"ls\"}");

    g_free(report.key);
}

/*
 * The table is an LRU, and a key it dropped starts again from one.
 *
 * Unbounded, one pathological turn making a million distinct calls would
 * grow the daemon for as long as it ran -- and the count restarting is
 * what makes the bound honest rather than a silent hole in the counting.
 */
static void
test_the_lru_evicts_the_least_recently_seen(void)
{
    g_autoptr(ClawtRepeatWatch) watch = clawt_repeat_watch_new();
    guint i;

    clawt_repeat_watch_set_max_keys(watch, 4);

    /* Seen twice, then left alone while four others go past. */
    clawt_repeat_watch_note(watch, "chief", "exec", "old");
    clawt_repeat_watch_note(watch, "chief", "exec", "old");
    g_assert_cmpuint(clawt_repeat_watch_count(watch, "chief", "exec", "old"),
                     ==, 2);

    for (i = 0; i < 4; i++) {
        g_autofree gchar *args = g_strdup_printf("fresh-%u", i);

        clawt_repeat_watch_note(watch, "chief", "exec", args);
    }

    g_assert_cmpuint(clawt_repeat_watch_count(watch, "chief", "exec", "old"),
                     ==, 0);

    clawt_repeat_watch_note(watch, "chief", "exec", "old");
    g_assert_cmpuint(clawt_repeat_watch_count(watch, "chief", "exec", "old"),
                     ==, 1);
}

/*
 * And a key that keeps being used is not the one dropped.
 *
 * Without the recency half this is a fixed-size cache that evicts by
 * insertion order, and the call an agent is looping on -- the one thing
 * this table exists to hold -- is exactly the oldest entry.
 */
static void
test_a_key_in_use_survives_eviction(void)
{
    g_autoptr(ClawtRepeatWatch) watch = clawt_repeat_watch_new();
    guint i;

    clawt_repeat_watch_set_max_keys(watch, 3);

    for (i = 0; i < 6; i++) {
        g_autofree gchar *args = g_strdup_printf("fresh-%u", i);

        clawt_repeat_watch_note(watch, "chief", "exec", "hot");
        clawt_repeat_watch_note(watch, "chief", "exec", args);
    }

    g_assert_cmpuint(clawt_repeat_watch_count(watch, "chief", "exec", "hot"),
                     ==, 6);
}

/*
 * Counters are per turn.
 *
 * The same call made once in each of twenty turns is twenty pieces of
 * work; only the same call twenty times inside one turn is a loop.
 */
static void
test_counters_drop_at_turn_end(void)
{
    g_autoptr(ClawtRepeatWatch) watch = clawt_repeat_watch_new();

    clawt_repeat_watch_note(watch, "chief", "exec", "ls");
    clawt_repeat_watch_note(watch, "chief", "exec", "ls");
    g_assert_cmpuint(clawt_repeat_watch_count(watch, "chief", "exec", "ls"),
                     ==, 2);

    clawt_repeat_watch_end_turn(watch, "chief");

    g_assert_cmpuint(clawt_repeat_watch_count(watch, "chief", "exec", "ls"),
                     ==, 0);
}

/* Turns are independent of each other, which is the other half of that. */
static void
test_two_agents_are_counted_apart(void)
{
    g_autoptr(ClawtRepeatWatch) watch = clawt_repeat_watch_new();

    clawt_repeat_watch_note(watch, "chief", "exec", "ls");
    clawt_repeat_watch_note(watch, "worker", "exec", "ls");

    g_assert_cmpuint(clawt_repeat_watch_count(watch, "chief", "exec", "ls"),
                     ==, 1);
    g_assert_cmpuint(clawt_repeat_watch_count(watch, "worker", "exec", "ls"),
                     ==, 1);
}

/* ── The turn budget ─────────────────────────────────────────────── */

static gint64 fake_now;

static gint64
fake_clock(gpointer user_data)
{
    (void)user_data;

    return fake_now;
}

static ClawtTurnWatch *
activity_watch(guint budget)
{
    ClawtTurnWatch *watch = clawt_turn_watch_new_activity();

    fake_now = 1000 * G_USEC_PER_SEC;
    clawt_turn_watch_set_clock(watch, fake_clock, NULL, NULL);
    clawt_turn_watch_set_budget(watch, budget);

    return watch;
}

static void
advance(guint seconds)
{
    fake_now += (gint64)seconds * G_USEC_PER_SEC;
}

/*
 * Activity, not duration.
 *
 * A turn may legitimately run for an hour while events keep arriving; a
 * turn that has emitted nothing at all for its budget is wedged. This is
 * the difference, and a duration watchdog passes every other test here.
 */
static void
test_a_sign_of_life_pushes_the_deadline_out(void)
{
    g_autoptr(ClawtTurnWatch) watch = activity_watch(60);
    g_autoptr(GPtrArray) expired = NULL;
    guint round;

    clawt_turn_watch_begin(watch, "chief");

    /* Ten minutes of work, in fifty-second steps. */
    for (round = 0; round < 12; round++) {
        advance(50);
        clawt_turn_watch_note_activity(watch, "chief");
    }

    expired = clawt_turn_watch_collect_expired(watch);
    g_assert_cmpuint(expired->len, ==, 0);

    /* And then it goes quiet. */
    advance(61);

    {
        g_autoptr(GPtrArray) wedged = clawt_turn_watch_collect_expired(watch);

        g_assert_cmpuint(wedged->len, ==, 1);
        g_assert_cmpstr(g_ptr_array_index(wedged, 0), ==, "chief");
    }
}

/*
 * A work budget is not pushed out by anything.
 *
 * `rooms.turn_timeout_seconds` bounds how long one member may hold a
 * room's turn however chatty it is being, which is the opposite question
 * from the one above.
 */
static void
test_a_work_budget_ignores_activity(void)
{
    g_autoptr(ClawtTurnWatch) watch = clawt_turn_watch_new_work();
    g_autoptr(GPtrArray) expired = NULL;
    guint round;

    fake_now = 1000 * G_USEC_PER_SEC;
    clawt_turn_watch_set_clock(watch, fake_clock, NULL, NULL);
    clawt_turn_watch_set_budget(watch, 60);
    clawt_turn_watch_begin(watch, "standup");

    for (round = 0; round < 3; round++) {
        advance(30);
        clawt_turn_watch_note_activity(watch, "standup");
    }

    expired = clawt_turn_watch_collect_expired(watch);
    g_assert_cmpuint(expired->len, ==, 1);
}

/*
 * The clock holds while a turn is parked on an open decision, and
 * resumes with the remainder.
 *
 * Stopping a turn under a question nobody has answered manufactures a
 * stranded decision, which the daemon then has to repair -- and the
 * remainder is the part that is easy to get wrong: restarting the whole
 * budget would let a turn hold a room for ever by asking a question
 * every four minutes.
 */
static void
test_a_hold_parks_the_clock_and_resumes_with_the_remainder(void)
{
    g_autoptr(ClawtTurnWatch) watch = clawt_turn_watch_new_work();

    fake_now = 1000 * G_USEC_PER_SEC;
    clawt_turn_watch_set_clock(watch, fake_clock, NULL, NULL);
    clawt_turn_watch_set_budget(watch, 300);
    clawt_turn_watch_begin(watch, "standup");

    advance(200);
    clawt_turn_watch_hold(watch, "standup");

    /* An hour of somebody being away from their desk. */
    advance(3600);

    {
        g_autoptr(GPtrArray) expired = clawt_turn_watch_collect_expired(watch);

        g_assert_cmpuint(expired->len, ==, 0);
    }

    clawt_turn_watch_release(watch, "standup");

    /* A hundred seconds left, not three hundred. */
    g_assert_cmpint(clawt_turn_watch_remaining(watch, "standup"), ==,
                    100 * G_USEC_PER_SEC);

    advance(99);

    {
        g_autoptr(GPtrArray) expired = clawt_turn_watch_collect_expired(watch);

        g_assert_cmpuint(expired->len, ==, 0);
    }

    advance(2);

    {
        g_autoptr(GPtrArray) expired = clawt_turn_watch_collect_expired(watch);

        g_assert_cmpuint(expired->len, ==, 1);
    }
}

/*
 * A release for a hold that was never taken does nothing.
 *
 * A resolve can arrive for a card this turn never opened -- the stale
 * cleanup after an interrupt does exactly that -- and a counter allowed
 * below zero needs as many spurious holds to climb back, during which
 * the budget is not running at all.
 */
static void
test_a_release_without_a_hold_clamps_at_zero(void)
{
    g_autoptr(ClawtTurnWatch) watch = clawt_turn_watch_new_work();
    guint i;

    fake_now = 1000 * G_USEC_PER_SEC;
    clawt_turn_watch_set_clock(watch, fake_clock, NULL, NULL);
    clawt_turn_watch_set_budget(watch, 60);
    clawt_turn_watch_begin(watch, "standup");

    /* Three resolves for cards this turn never opened. */
    for (i = 0; i < 3; i++)
        clawt_turn_watch_release(watch, "standup");

    g_assert_cmpuint(clawt_turn_watch_get_holds(watch, "standup"), ==, 0);

    /* One real hold still parks it, and one release still frees it. */
    clawt_turn_watch_hold(watch, "standup");
    g_assert_cmpuint(clawt_turn_watch_get_holds(watch, "standup"), ==, 1);

    clawt_turn_watch_release(watch, "standup");
    g_assert_cmpuint(clawt_turn_watch_get_holds(watch, "standup"), ==, 0);

    advance(61);

    {
        g_autoptr(GPtrArray) expired = clawt_turn_watch_collect_expired(watch);

        g_assert_cmpuint(expired->len, ==, 1);
    }
}

/*
 * A hold that finds the budget already spent expires the turn at once.
 *
 * The main loop can be late -- decision events are delivered from an
 * idle -- so the card can arrive after the deadline has passed. Taking
 * the hold then would hand the turn an unbounded extension for a budget
 * that was gone before anybody asked anything, and the expiry would
 * never be reported at all.
 */
static void
test_a_hold_on_a_spent_budget_expires_immediately(void)
{
    g_autoptr(ClawtTurnWatch) watch = clawt_turn_watch_new_work();
    g_autoptr(GPtrArray) expired = NULL;

    fake_now = 1000 * G_USEC_PER_SEC;
    clawt_turn_watch_set_clock(watch, fake_clock, NULL, NULL);
    clawt_turn_watch_set_budget(watch, 60);
    clawt_turn_watch_begin(watch, "standup");

    advance(90);
    clawt_turn_watch_hold(watch, "standup");

    g_assert_cmpuint(clawt_turn_watch_get_holds(watch, "standup"), ==, 0);

    expired = clawt_turn_watch_collect_expired(watch);
    g_assert_cmpuint(expired->len, ==, 1);
}

/*
 * An expired turn is reported once.
 *
 * The caller is about to interrupt it, and a second report while that is
 * in flight would interrupt it twice -- which for a turn that is being
 * killed is how a second turn ends up overlapping the first.
 */
static void
test_an_expired_turn_is_reported_once(void)
{
    g_autoptr(ClawtTurnWatch) watch = activity_watch(60);

    clawt_turn_watch_begin(watch, "chief");
    advance(61);

    {
        g_autoptr(GPtrArray) first = clawt_turn_watch_collect_expired(watch);
        g_autoptr(GPtrArray) second = clawt_turn_watch_collect_expired(watch);

        g_assert_cmpuint(first->len, ==, 1);
        g_assert_cmpuint(second->len, ==, 0);
    }

    /* And it is no longer watched at all, which is a different answer. */
    g_assert_cmpint(clawt_turn_watch_remaining(watch, "chief"), ==, -1);
}

/* A budget of zero watches nothing rather than expiring everything. */
static void
test_a_budget_of_zero_watches_nothing(void)
{
    g_autoptr(ClawtTurnWatch) watch = activity_watch(0);
    g_autoptr(GPtrArray) expired = NULL;

    clawt_turn_watch_begin(watch, "chief");
    advance(100000);

    g_assert_false(clawt_turn_watch_is_watching(watch, "chief"));

    expired = clawt_turn_watch_collect_expired(watch);
    g_assert_cmpuint(expired->len, ==, 0);
}

/* ── The steer queue ─────────────────────────────────────────────── */

/*
 * Several corrections typed while one turn runs become one follow-up
 * turn, in the order they were typed.
 *
 * Two turns for two sentences typed three seconds apart costs twice and
 * answers the first one blind.
 */
static void
test_messages_in_one_thread_are_joined(void)
{
    g_autoptr(ClawtSteerQueue) queue = clawt_steer_queue_new();
    g_autofree gchar *thread = NULL;
    g_autofree gchar *drained = NULL;

    clawt_steer_queue_add(queue, "dm:chief:user", "chief", "wait");
    clawt_steer_queue_add(queue, "dm:chief:user", "chief", "use the other one");

    g_assert_cmpuint(clawt_steer_queue_pending(queue, "chief"), ==, 2);

    drained = clawt_steer_queue_drain(queue, "chief", &thread);

    g_assert_cmpstr(drained, ==, "wait\nuse the other one");
    g_assert_cmpstr(thread, ==, "dm:chief:user");
}

/*
 * A drain takes the entry out before it reads a byte of it.
 *
 * Two settles can arrive together -- the link lowering its typing
 * indicator and the daemon's own interrupt both free the same agent --
 * and building the text first would let both find the entry and deliver
 * the correction twice.
 */
static void
test_two_settles_drain_one_queue_once(void)
{
    g_autoptr(ClawtSteerQueue) queue = clawt_steer_queue_new();
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;

    clawt_steer_queue_add(queue, "dm:chief:user", "chief", "actually, stop");

    first = clawt_steer_queue_drain(queue, "chief", NULL);
    second = clawt_steer_queue_drain(queue, "chief", NULL);

    g_assert_cmpstr(first, ==, "actually, stop");
    g_assert_null(second);
    g_assert_cmpuint(clawt_steer_queue_pending(queue, NULL), ==, 0);
}

/*
 * Keyed by thread, found by agent.
 *
 * The settle that frees an agent can happen on a different thread from
 * the one the correction was typed into, so draining has to be
 * answerable from the agent alone -- and each thread is its own turn.
 */
static void
test_two_threads_drain_as_two_turns(void)
{
    g_autoptr(ClawtSteerQueue) queue = clawt_steer_queue_new();
    g_autofree gchar *one = NULL;
    g_autofree gchar *two = NULL;
    g_autofree gchar *three = NULL;
    g_autofree gchar *first_thread = NULL;

    clawt_steer_queue_add(queue, "dm:chief:user", "chief", "from the operator");
    clawt_steer_queue_add(queue, "room:standup", "chief", "from the room");

    one = clawt_steer_queue_drain(queue, "chief", &first_thread);
    two = clawt_steer_queue_drain(queue, "chief", NULL);
    three = clawt_steer_queue_drain(queue, "chief", NULL);

    /* Oldest first, so a correction is not overtaken by a later one. */
    g_assert_cmpstr(first_thread, ==, "dm:chief:user");
    g_assert_cmpstr(one, ==, "from the operator");
    g_assert_cmpstr(two, ==, "from the room");
    g_assert_null(three);
}

/* ── The rendered watchdog ───────────────────────────────────────── */

/*
 * `agents.runtime.turn_timeout_seconds` reaches libreclaw's own
 * watchdog.
 *
 * libreclaw has had `lc_session_set_watchdog_timeout()` since 0.23.3
 * with a default of 1200 seconds, and clawtilla never set it -- so the
 * one control an operator had over a wedged turn reached nothing at all.
 * This is the wire; grep for the caller, not the implementation.
 */
static void
test_the_watchdog_reaches_the_agents_config(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: patient\n"
        "    runtime: {turn_timeout_seconds: 5400}\n"
        "  - id: unbounded\n"
        "    runtime: {turn_timeout_seconds: 0}\n"
        "  - id: ordinary\n",
        &error);
    GPtrArray *agents;
    g_autofree gchar *patient = NULL;
    g_autofree gchar *unbounded = NULL;
    g_autofree gchar *ordinary = NULL;

    g_assert_no_error(error);
    agents = clawt_config_get_agents(config);
    g_assert_cmpuint(agents->len, ==, 3);

    patient = clawt_config_render_agent(config,
                                        g_ptr_array_index(agents, 0),
                                        "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_nonnull(strstr(patient, "watchdog_timeout_seconds: 5400"));

    /*
     * Zero is written too. It is what turns the watchdog off, and
     * leaving the key out would restore libreclaw's default instead --
     * which is the opposite of what was asked for.
     */
    unbounded = clawt_config_render_agent(config,
                                          g_ptr_array_index(agents, 1),
                                          "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_nonnull(strstr(unbounded, "watchdog_timeout_seconds: 0"));

    /* And an agent that says nothing gets the schema's default. */
    ordinary = clawt_config_render_agent(config,
                                         g_ptr_array_index(agents, 2),
                                         "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_nonnull(strstr(ordinary, "watchdog_timeout_seconds: 1200"));
}

/* ── Drafts ──────────────────────────────────────────────────────── */

/*
 * A draft with newlines in it comes back with the newlines in it.
 *
 * This is the whole reason the text is C-escaped before it goes into a
 * single-quoted YAML scalar: YAML *folds* a real newline inside a quoted
 * scalar into a space, so the obvious spelling silently reflows what
 * somebody wrote and nothing reports it.
 */
static void
test_a_multi_line_draft_round_trips(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-draft-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "drafts.yaml", NULL);
    g_autofree gchar *back = NULL;
    const gchar *text =
        "first line\n\nthird line, with 'quotes' and a backslash \\";

    g_assert_true(clawt_draft_store_set(path, "local/chief", text, NULL));

    back = clawt_draft_store_get(path, "local/chief");
    g_assert_cmpstr(back, ==, text);

    clawt_test_remove_tree(dir);
}

/* Clearing removes the entry rather than storing a blank one. */
static void
test_an_empty_draft_is_forgotten(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-draft-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "drafts.yaml", NULL);
    g_autoptr(GHashTable) drafts = NULL;

    g_assert_true(clawt_draft_store_set(path, "local/chief", "half a thought",
                                        NULL));
    g_assert_true(clawt_draft_store_set(path, "local/chief", "", NULL));

    drafts = clawt_draft_store_load(path, NULL);
    g_assert_nonnull(drafts);
    g_assert_cmpuint(g_hash_table_size(drafts), ==, 0);

    clawt_test_remove_tree(dir);
}

/* A file that is not there is an empty set of drafts, not a failure. */
static void
test_a_missing_draft_file_reads_as_empty(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-draft-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "nothing-here.yaml", NULL);
    g_autoptr(GHashTable) drafts = NULL;
    g_autoptr(GError) error = NULL;

    drafts = clawt_draft_store_load(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(drafts);
    g_assert_cmpuint(g_hash_table_size(drafts), ==, 0);

    clawt_test_remove_tree(dir);
}

/*
 * The key carries the connection profile.
 *
 * A client switches daemons at runtime and two fleets can each hold an
 * agent called `chief`; keying on the agent alone would show one
 * machine's half-written message in the other machine's composer.
 */
static void
test_two_daemons_do_not_share_a_draft(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-draft-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "drafts.yaml", NULL);
    g_autofree gchar *here = clawt_draft_key(NULL, "chief");
    g_autofree gchar *there = clawt_draft_key("workstation", "chief");
    g_autofree gchar *read_here = NULL;
    g_autofree gchar *read_there = NULL;

    g_assert_cmpstr(here, !=, there);

    g_assert_true(clawt_draft_store_set(path, here, "on the laptop", NULL));
    g_assert_true(clawt_draft_store_set(path, there, "on the desk", NULL));

    read_here = clawt_draft_store_get(path, here);
    read_there = clawt_draft_store_get(path, there);

    g_assert_cmpstr(read_here, ==, "on the laptop");
    g_assert_cmpstr(read_there, ==, "on the desk");

    clawt_test_remove_tree(dir);
}

/* ── The daemon ──────────────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *extra_yaml)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-turn-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);

    /*
     * Five things pinned, every one of which otherwise escapes into the
     * developer's own fleet or onto the network: the state directory,
     * the socket, the automation directory, the workspace root and the
     * tailnet listener.
     */
    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "%s",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        extra_yaml != NULL ? extra_yaml : "");

    g_file_set_contents(fixture->config_path, yaml, -1, &error);
    g_assert_no_error(error);

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);

    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

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

/*
 * Drops a warning on the floor.
 *
 * g_test_expect_message() makes every *other* message fatal too, which
 * turns the assertion into one about the order clawtilla happens to log
 * in -- and the daemon logs several lines while it starts.  The grace
 * timer warns on purpose, because an agent whose runtime never reported
 * the end of its turn is an anomaly worth a line in the log.
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

static JsonObject *
payload_of(JsonNode *reply)
{
    JsonObject *root = json_node_get_object(reply);

    g_assert_true(json_object_has_member(root, "payload"));

    return json_object_get_object_member(root, "payload");
}

/* How many messages the agent's operator room is holding. */
static guint
history_length(Fixture *fixture, const gchar *agent_id)
{
    g_autofree gchar *payload =
        g_strdup_printf("{\"room\":\"%s\"}", agent_id);
    g_autoptr(JsonNode) reply = request(fixture, "room.history", payload);
    JsonArray *messages;

    g_assert_nonnull(reply);
    messages = json_object_get_array_member(payload_of(reply), "messages");

    return (guint)json_array_get_length(messages);
}

static void
mark_busy(Fixture *fixture, const gchar *agent_id, gboolean busy)
{
    ClawtAgent *agent = clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture->daemon), agent_id);

    g_assert_nonnull(agent);
    clawt_agent_set_activity(agent, busy, NULL);
}

/*
 * A correction typed at a working agent is held, and does not enter the
 * transcript.
 *
 * Appending it straight away would make the queued line the active leaf,
 * so the rest of the turn in flight would hang off a line the model was
 * never shown -- and the transcript would read as though the agent had
 * answered something nobody had said.
 */
static void
test_a_steer_is_held_out_of_the_transcript(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    mark_busy(&fixture, "chief", TRUE);

    reply = request(&fixture, "msg.send",
                    "{\"target\":\"chief\",\"body\":\"actually, stop\"}");

    g_assert_nonnull(reply);
    g_assert_true(json_object_get_boolean_member(payload_of(reply),
                                                 "steered"));
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "queued"),
                    ==, 0);

    g_assert_cmpuint(history_length(&fixture, "chief"), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * And it arrives exactly once when the turn settles.
 *
 * Exactly once is the assertion: the queue is drained from three places
 * -- the typing frame, the interrupt verb and the grace timer -- and any
 * two of them can arrive together.
 */
static void
test_a_settle_delivers_a_steer_exactly_once(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    mark_busy(&fixture, "chief", TRUE);

    reply = request(&fixture, "msg.send",
                    "{\"target\":\"chief\",\"body\":\"actually, stop\"}");
    g_assert_nonnull(reply);

    mark_busy(&fixture, "chief", FALSE);

    /*
     * Twice on purpose. clawt_daemon_interrupt_agent() and the link's
     * own typing frame both settle the same turn, and a queue drained
     * twice would deliver the correction twice.
     */
    clawt_daemon_turn_settle(fixture.daemon, "chief");
    clawt_daemon_turn_settle(fixture.daemon, "chief");

    g_assert_cmpuint(history_length(&fixture, "chief"), ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A steer survives being stopped.
 *
 * Queue a correction, press stop, the correction runs -- that is the
 * feature rather than a leak. Pressing stop is how somebody says "not
 * that, this", and dropping the "this" leaves them having only
 * cancelled. clawt_daemon_interrupt_agent() settles through the same
 * function this calls, which is the wire.
 */
static void
test_a_steer_survives_a_stop(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    guint killed = 0;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    mark_busy(&fixture, "chief", TRUE);

    reply = request(&fixture, "msg.send",
                    "{\"target\":\"chief\",\"body\":\"not that\"}");
    g_assert_nonnull(reply);

    /*
     * The interrupt refuses -- there is no runtime in a hermetic test --
     * and the assertion that matters is that refusing did not throw the
     * correction away.
     */
    g_assert_false(clawt_daemon_interrupt_agent(fixture.daemon, "chief",
                                                &killed, &error));

    mark_busy(&fixture, "chief", FALSE);
    clawt_daemon_turn_settle(fixture.daemon, "chief");

    g_assert_cmpuint(history_length(&fixture, "chief"), ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A message to an agent that is not working goes straight to the
 * mailbox, which is what a durable queue is for.
 *
 * Holding it here instead would lose it: nothing settles a turn that
 * never started.
 */
static void
test_a_message_to_an_idle_agent_is_not_steered(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    reply = request(&fixture, "msg.send",
                    "{\"target\":\"chief\",\"body\":\"when you can\"}");

    g_assert_nonnull(reply);
    g_assert_false(json_object_get_boolean_member(payload_of(reply),
                                                  "steered"));
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "queued"),
                    ==, 1);

    g_assert_cmpuint(history_length(&fixture, "chief"), ==, 1);

    fixture_teardown(&fixture);
}

/*
 * An agent's own message to a busy peer is never steered.
 *
 * Ordinary traffic between agents belongs in the mailbox, which is
 * durable and ordered precisely so that a busy recipient is not a
 * special case. Steering it would hold a delegation until the peer
 * happened to finish something unrelated.
 */
static void
test_a_peer_message_is_never_steered(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture,
                  "agents:\n  - id: chief\n  - id: worker\n");
    mark_busy(&fixture, "worker", TRUE);

    reply = request(&fixture, "msg.send",
                    "{\"from\":\"chief\",\"target\":\"worker\","
                    "\"body\":\"have a look at this\"}");

    g_assert_nonnull(reply);
    g_assert_false(json_object_get_boolean_member(payload_of(reply),
                                                  "steered"));

    fixture_teardown(&fixture);
}

/*
 * The grace timer releases an agent whose runtime never reported the end
 * of its turn.
 *
 * A stop that only signals is not a stop -- `agent.restart` learned that
 * already. Without this the agent stays marked busy for ever, the next
 * delivery overlaps a turn nobody is running, and the watch never begins
 * again.
 */
static void
test_the_grace_timer_releases_a_stuck_agent(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *agent;
    GLogLevelFlags fatal;
    guint handler;
    guint waited;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    /*
     * One second rather than fifteen. A test that waits for the real
     * grace period is a test people start skipping, and this is the one
     * mechanism that catches a stop that did not stop.
     */
    clawt_daemon_turn_set_grace_seconds(fixture.daemon, 1);

    agent = clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture.daemon), "chief");
    g_assert_nonnull(agent);

    clawt_agent_set_activity(agent, TRUE, NULL);

    /*
     * A turn that timed out, interrupted, whose runtime says nothing
     * afterwards. clawt_daemon_turn_settle() cancels the grace timer, so
     * this arms it the way an expiry does and then leaves the agent busy.
     */
    fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK);
    handler = g_log_set_handler("Clawtilla", G_LOG_LEVEL_WARNING,
                                swallow_warnings, NULL);

    /*
     * Armed the way an expiry arms it. Nothing else here will ever mark
     * this agent idle -- there is no runtime in a hermetic test, which
     * is exactly the shape of the failure the timer exists for.
     */
    clawt_daemon_turn_arm_grace(fixture.daemon, "chief");

    /*
     * Bounded. A test that can hang is worse than one that fails, and a
     * timer that never fires is precisely the bug being guarded against.
     */
    for (waited = 0; waited < WAIT_ITERATIONS; waited++) {
        if (!clawt_agent_get_busy(agent))
            break;

        g_main_context_iteration(fixture.context, FALSE);
        g_usleep(2000);
    }

    g_log_remove_handler("Clawtilla", handler);
    g_log_set_always_fatal(fatal);

    g_assert_false(clawt_agent_get_busy(agent));

    fixture_teardown(&fixture);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/turn-hygiene/a-bare-tool-name-is-not-a-key",
                    test_a_bare_tool_name_is_not_a_key);
    g_test_add_func("/turn-hygiene/the-key-collapses-whitespace",
                    test_the_key_collapses_whitespace);
    g_test_add_func("/turn-hygiene/thresholds-fire-exactly-on-5-10-20",
                    test_thresholds_fire_exactly_on_5_10_and_20);
    g_test_add_func("/turn-hygiene/the-lru-evicts-the-least-recently-seen",
                    test_the_lru_evicts_the_least_recently_seen);
    g_test_add_func("/turn-hygiene/a-key-in-use-survives-eviction",
                    test_a_key_in_use_survives_eviction);
    g_test_add_func("/turn-hygiene/counters-drop-at-turn-end",
                    test_counters_drop_at_turn_end);
    g_test_add_func("/turn-hygiene/two-agents-are-counted-apart",
                    test_two_agents_are_counted_apart);

    g_test_add_func("/turn-hygiene/a-sign-of-life-pushes-the-deadline-out",
                    test_a_sign_of_life_pushes_the_deadline_out);
    g_test_add_func("/turn-hygiene/a-work-budget-ignores-activity",
                    test_a_work_budget_ignores_activity);
    g_test_add_func("/turn-hygiene/a-hold-resumes-with-the-remainder",
                    test_a_hold_parks_the_clock_and_resumes_with_the_remainder);
    g_test_add_func("/turn-hygiene/a-release-without-a-hold-clamps",
                    test_a_release_without_a_hold_clamps_at_zero);
    g_test_add_func("/turn-hygiene/a-hold-on-a-spent-budget-expires",
                    test_a_hold_on_a_spent_budget_expires_immediately);
    g_test_add_func("/turn-hygiene/an-expired-turn-is-reported-once",
                    test_an_expired_turn_is_reported_once);
    g_test_add_func("/turn-hygiene/a-budget-of-zero-watches-nothing",
                    test_a_budget_of_zero_watches_nothing);

    g_test_add_func("/turn-hygiene/messages-in-one-thread-are-joined",
                    test_messages_in_one_thread_are_joined);
    g_test_add_func("/turn-hygiene/two-settles-drain-one-queue-once",
                    test_two_settles_drain_one_queue_once);
    g_test_add_func("/turn-hygiene/two-threads-drain-as-two-turns",
                    test_two_threads_drain_as_two_turns);

    g_test_add_func("/turn-hygiene/the-watchdog-reaches-the-agents-config",
                    test_the_watchdog_reaches_the_agents_config);

    g_test_add_func("/turn-hygiene/a-multi-line-draft-round-trips",
                    test_a_multi_line_draft_round_trips);
    g_test_add_func("/turn-hygiene/an-empty-draft-is-forgotten",
                    test_an_empty_draft_is_forgotten);
    g_test_add_func("/turn-hygiene/a-missing-draft-file-reads-as-empty",
                    test_a_missing_draft_file_reads_as_empty);
    g_test_add_func("/turn-hygiene/two-daemons-do-not-share-a-draft",
                    test_two_daemons_do_not_share_a_draft);

    g_test_add_func("/turn-hygiene/a-steer-is-held-out-of-the-transcript",
                    test_a_steer_is_held_out_of_the_transcript);
    g_test_add_func("/turn-hygiene/a-settle-delivers-a-steer-once",
                    test_a_settle_delivers_a_steer_exactly_once);
    g_test_add_func("/turn-hygiene/a-steer-survives-a-stop",
                    test_a_steer_survives_a_stop);
    g_test_add_func("/turn-hygiene/an-idle-agent-is-not-steered",
                    test_a_message_to_an_idle_agent_is_not_steered);
    g_test_add_func("/turn-hygiene/a-peer-message-is-never-steered",
                    test_a_peer_message_is_never_steered);
    g_test_add_func("/turn-hygiene/the-grace-timer-releases-a-stuck-agent",
                    test_the_grace_timer_releases_a_stuck_agent);

    return g_test_run();
}
