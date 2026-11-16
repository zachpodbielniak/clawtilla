/*
 * test-loop-guard.c - Ending an exchange, not refusing one more message
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The four limits in #ClawtLoopGuard have always refused a message, and
 * tests/test-orchestration.c covers that they do.  What is here is the
 * part that came later: **a refusal is not an ending**.  A stuck pair
 * kept taking turns, each turn produced a message, each message was
 * refused, and the turn had already been paid for before the refusal
 * arrived.
 *
 * So every test in this file asserts on the *count* of messages that got
 * through, not on the final state.  A guard that refuses for ever passes
 * a test phrased as "the last one was refused" and fails this one, which
 * is the whole distinction being made.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

/*
 * The guard cannot tell a person from an agent on its own, and it must
 * not end a person's conversation.  Both are named here rather than
 * "anything that is not user", so a test can say which is which.
 */
static gboolean
alice_and_bob_are_agents(const gchar *sender_id, gpointer user_data)
{
    (void)user_data;

    return g_strcmp0(sender_id, "alice") == 0 ||
           g_strcmp0(sender_id, "bob") == 0;
}

static ClawtMessage *
note(const gchar *sender, const gchar *room, const gchar *body)
{
    ClawtMessage *message = clawt_message_new(room, sender, body);

    clawt_message_set_depth(message, 1);

    return message;
}

static ClawtLoopGuard *
guard_with_peers(void)
{
    ClawtLoopGuard *guard = clawt_loop_guard_new();

    clawt_loop_guard_set_limits(guard, 0, 0, 20);
    clawt_loop_guard_set_cycle_seconds(guard, 300);
    clawt_loop_guard_set_task_budget(guard, 0.0);
    clawt_loop_guard_set_peer_func(guard, alice_and_bob_are_agents, NULL,
                                   NULL);

    return guard;
}

/* ── The exchange ends ───────────────────────────────────────────── */

/*
 * Two agents alternating the same two replies, asserted on how many got
 * through rather than on the state at the end.
 *
 * Before the stall existed this loop ran for ever: every message after
 * the first pair was refused, and every refusal cost the sender a turn.
 * The bound is what the test is about -- twenty rounds of a real
 * exchange is twenty model calls nobody asked for.
 */
static void
test_an_alternating_pair_is_stopped_within_a_bound(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    guint sent = 0;
    guint round;

    for (round = 0; round < 20; round++) {
        g_autoptr(ClawtMessage) a = note("alice", "room", "shall we?");
        g_autoptr(ClawtMessage) b = note("bob", "room", "after you");

        if (clawt_loop_guard_check(guard, a, NULL))
            sent++;

        if (clawt_loop_guard_check(guard, b, NULL))
            sent++;
    }

    /*
     * One each.  The second time round alice repeats herself, which is
     * the exact repeat that ends it -- and bob's reply after that is
     * refused by the stall rather than by the cycle check, which is the
     * difference this whole file is about.
     */
    g_assert_cmpuint(sent, ==, 2);
    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_REPEATED_MESSAGE);
}

/*
 * And the refusal after the stall names the ending rather than the loop.
 *
 * An agent told "say something different" will say something different
 * and take another turn doing it.  It has to be told the exchange is
 * over.
 */
static void
test_a_stalled_room_refuses_without_advice_to_retry(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(ClawtMessage) first = note("alice", "room", "same");
    g_autoptr(ClawtMessage) second = note("alice", "room", "same");
    g_autoptr(ClawtMessage) third = note("bob", "room", "something else");
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_loop_guard_check(guard, first, NULL));
    g_assert_false(clawt_loop_guard_check(guard, second, NULL));

    g_assert_false(clawt_loop_guard_check(guard, third, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
    g_assert_nonnull(strstr(error->message, "ended"));
}

/*
 * The system's own notice passes a stalled room, and the stall stays.
 *
 * When the guard ends an exchange the daemon stalls the task behind it,
 * and the stall's notice has to reach the delegator through the very
 * room the guard just closed -- refused, the ending would be the one
 * thing nobody could be told about.  It passes without *clearing* the
 * stall: the refusal's text promises the room stays ended until a
 * person speaks, and a notice is not a person.
 */
static void
test_a_system_notice_passes_a_stalled_room(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(ClawtMessage) first = note("alice", "room", "same");
    g_autoptr(ClawtMessage) second = note("alice", "room", "same");
    g_autoptr(ClawtMessage) notice =
        note("clawtilla", "room", "[clawtilla] Task ta-1 was stopped.");
    g_autoptr(ClawtMessage) after = note("bob", "room", "so, anyway");

    g_assert_true(clawt_loop_guard_check(guard, first, NULL));
    g_assert_false(clawt_loop_guard_check(guard, second, NULL));

    g_assert_true(clawt_loop_guard_check(guard, notice, NULL));

    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_REPEATED_MESSAGE);
    g_assert_false(clawt_loop_guard_check(guard, after, NULL));
}

/*
 * A person saying something restarts it.
 *
 * Without this a stalled room is dead for the life of the daemon, and
 * the person the stall was raised for is the one who cannot do anything
 * about it.
 */
static void
test_a_person_reopens_a_stalled_exchange(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(ClawtMessage) first = note("alice", "room", "same");
    g_autoptr(ClawtMessage) second = note("alice", "room", "same");
    g_autoptr(ClawtMessage) operator_line = note("user", "room", "carry on");
    g_autoptr(ClawtMessage) after = note("alice", "room", "right, then");

    g_assert_true(clawt_loop_guard_check(guard, first, NULL));
    g_assert_false(clawt_loop_guard_check(guard, second, NULL));

    g_assert_true(clawt_loop_guard_check(guard, operator_line, NULL));
    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_NONE);

    g_assert_true(clawt_loop_guard_check(guard, after, NULL));
}

/*
 * clawtilla does not end a person's conversation.
 *
 * An operator repeating themselves -- pasting the same instruction
 * twice, saying "yes" again -- is refused by the cycle check exactly as
 * before and the room stays open.
 */
static void
test_a_person_repeating_themselves_does_not_stall_the_room(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(ClawtMessage) first = note("user", "room", "do the thing");
    g_autoptr(ClawtMessage) again = note("user", "room", "do the thing");

    g_assert_true(clawt_loop_guard_check(guard, first, NULL));
    g_assert_false(clawt_loop_guard_check(guard, again, NULL));

    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_NONE);
}

/*
 * With no peer function set, nothing is a peer and nothing ever stalls.
 *
 * That is the behaviour every caller had before stalls existed, and it
 * is what keeps a guard built by something that does not know about
 * agents -- a test, an embedding host -- from ending exchanges on its
 * own.
 */
static void
test_without_a_peer_function_nothing_stalls(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(ClawtMessage) first = note("alice", "room", "same");
    g_autoptr(ClawtMessage) second = note("alice", "room", "same");

    clawt_loop_guard_set_limits(guard, 0, 0, 20);
    clawt_loop_guard_set_cycle_seconds(guard, 300);
    clawt_loop_guard_set_task_budget(guard, 0.0);

    g_assert_true(clawt_loop_guard_check(guard, first, NULL));
    g_assert_false(clawt_loop_guard_check(guard, second, NULL));

    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_NONE);
}

/* ── The same message with the numbers changed ───────────────────── */

/*
 * A loop that looks different every time.
 *
 * "finished run 41", "finished run 42": an exact-match detector sees
 * three distinct messages and lets it run for ever.  Caught, and
 * deliberately *not* stalled -- a near match is a refusal that can be
 * waited out, because being wrong about a stall costs somebody a trip to
 * restart the conversation.
 */
static void
test_a_changing_run_id_is_still_caught(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(GError) error = NULL;
    guint sent = 0;
    guint run;

    for (run = 41; run < 80; run++) {
        g_autofree gchar *body = g_strdup_printf("finished run %u", run);
        g_autoptr(ClawtMessage) line = note("alice", "room", body);

        if (clawt_loop_guard_check(guard, line, &error))
            sent++;
        else
            break;
    }

    /*
     * Bounded, and asserted as a count: the point is that this stops,
     * not that any particular message was refused. Thirty-nine of them
     * would be thirty-nine model calls spent counting.
     */
    g_assert_cmpuint(sent, ==, 5);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
    g_assert_nonnull(strstr(error->message, "numbers"));

    /*
     * And not stalled. Being wrong about a near match costs a refusal
     * the sender can wait out; being wrong about a stall costs somebody
     * a trip to restart the conversation.
     */
    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_NONE);
}

/*
 * And a short run of numbered steps is **not** a loop.
 *
 * "step 1 done" through "step 4 done" is progress through a small task,
 * and refusing it would silence an agent that is working. This is the
 * boundary the limit is placed at, asserted from the safe side.
 */
static void
test_a_few_numbered_steps_get_through(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    guint sent = 0;
    guint step;

    for (step = 1; step <= 4; step++) {
        g_autofree gchar *body = g_strdup_printf("step %u done", step);
        g_autoptr(ClawtMessage) line = note("alice", "room", body);

        if (clawt_loop_guard_check(guard, line, NULL))
            sent++;
    }

    g_assert_cmpuint(sent, ==, 4);
}

/*
 * And the one that matters more than any of the others: real work that
 * repeats is **not** caught.
 *
 * A false positive here silences an agent that is doing its job, which
 * is worse than the loop -- the loop costs money and is visible, and a
 * silenced status line is neither.  What protects it is the *window*:
 * an hourly line is compared against nothing, because the previous one
 * fell out of the cycle window fifty-five minutes ago.
 */
static void
test_an_hourly_status_line_is_not_caught(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    guint sent = 0;
    guint hour;

    /* Five minutes of memory, and a line an hour. */
    clawt_loop_guard_set_cycle_seconds(guard, 300);

    for (hour = 0; hour < 8; hour++) {
        g_autoptr(ClawtMessage) line =
            note("alice", "room", "hourly: all quiet");

        if (clawt_loop_guard_check(guard, line, NULL))
            sent++;

        /*
         * An hour later, without waiting an hour.  Resetting the guard's
         * memory is what the window does on its own once the entries are
         * older than cycle_seconds -- and a test that slept for it would
         * be a test that hangs.
         */
        clawt_loop_guard_reset(guard);
    }

    g_assert_cmpuint(sent, ==, 8);
    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_NONE);
}

/*
 * A number that is part of the content, not part of a counter.
 *
 * "deploy 3 servers" and "deploy 5 servers" are a near match and are
 * refused -- which is the cost of catching the counting loop, and is
 * survivable precisely because a near match does not stall.
 */
static void
test_a_near_match_is_refused_but_leaves_the_room_open(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(ClawtMessage) different = note("alice", "room", "all deployed");
    guint count;

    for (count = 1; count <= 6; count++) {
        g_autofree gchar *body = g_strdup_printf("deploy %u servers", count);
        g_autoptr(ClawtMessage) line = note("alice", "room", body);

        if (count <= 5)
            g_assert_true(clawt_loop_guard_check(guard, line, NULL));
        else
            g_assert_false(clawt_loop_guard_check(guard, line, NULL));
    }

    /* And the sender is not cut off: saying something else works. */
    g_assert_true(clawt_loop_guard_check(guard, different, NULL));
}

/* ── Stalling from outside ───────────────────────────────────────── */

/*
 * The turn watchdog and the room budget both end an exchange, and the
 * first reason is the one kept.
 *
 * A room that stalled on a repeated message and then hit the room budget
 * while nobody was reading is still a room that stalled on a repeated
 * message; overwriting would leave the alert naming the consequence.
 */
static void
test_the_first_stall_reason_is_the_one_kept(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();

    g_assert_true(clawt_loop_guard_stall_room(guard, "room",
                                              CLAWT_STALL_TURN_TIMEOUT,
                                              "nothing for 20 minutes"));

    g_assert_false(clawt_loop_guard_stall_room(guard, "room",
                                               CLAWT_STALL_ROOM_TIMEOUT,
                                               "held its turn"));

    g_assert_cmpint(clawt_loop_guard_get_stall_reason(guard, "room"), ==,
                    CLAWT_STALL_TURN_TIMEOUT);
    g_assert_cmpstr(clawt_loop_guard_get_stall_detail(guard, "room"), ==,
                    "nothing for 20 minutes");
}

/*
 * Clearing a stall drops the fingerprints too.
 *
 * Leaving them would have the first message after a person restarts the
 * conversation matched against whatever was going round before it -- so
 * restarting an exchange would refuse the restart.
 */
static void
test_clearing_a_stall_forgets_what_was_repeating(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(ClawtMessage) first = note("alice", "room", "same");
    g_autoptr(ClawtMessage) second = note("alice", "room", "same");
    g_autoptr(ClawtMessage) third = note("alice", "room", "same");

    g_assert_true(clawt_loop_guard_check(guard, first, NULL));
    g_assert_false(clawt_loop_guard_check(guard, second, NULL));

    g_assert_true(clawt_loop_guard_clear_stall(guard, "room"));
    g_assert_true(clawt_loop_guard_check(guard, third, NULL));
}

/*
 * The signal is what the daemon hangs the alert, the task move and the
 * note in the thread off, so it has to carry all three of the things
 * those need.
 */
typedef struct {
    guint   fired;
    gchar  *room;
    guint   reason;
    gchar  *detail;
} StallReport;

static void
on_stalled(ClawtLoopGuard *guard, const gchar *room, guint reason,
           const gchar *detail, gpointer user_data)
{
    StallReport *report = user_data;

    (void)guard;

    report->fired++;
    g_free(report->room);
    g_free(report->detail);
    report->room = g_strdup(room);
    report->reason = reason;
    report->detail = g_strdup(detail);
}

static void
test_the_stall_signal_names_the_room_and_the_text(void)
{
    g_autoptr(ClawtLoopGuard) guard = guard_with_peers();
    g_autoptr(ClawtMessage) first = note("alice", "room", "round we go");
    g_autoptr(ClawtMessage) second = note("alice", "room", "round we go");
    g_autoptr(ClawtMessage) third = note("bob", "room", "and again");
    StallReport report = { 0 };

    g_signal_connect(guard, "stalled", G_CALLBACK(on_stalled), &report);

    g_assert_true(clawt_loop_guard_check(guard, first, NULL));
    g_assert_false(clawt_loop_guard_check(guard, second, NULL));

    /* And once only, however many more messages arrive. */
    g_assert_false(clawt_loop_guard_check(guard, third, NULL));

    g_assert_cmpuint(report.fired, ==, 1);
    g_assert_cmpstr(report.room, ==, "room");
    g_assert_cmpuint(report.reason, ==, CLAWT_STALL_REPEATED_MESSAGE);
    g_assert_cmpstr(report.detail, ==, "round we go");

    g_free(report.room);
    g_free(report.detail);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/loop-guard/an-alternating-pair-is-stopped",
                    test_an_alternating_pair_is_stopped_within_a_bound);
    g_test_add_func("/loop-guard/a-stalled-room-does-not-advise-a-retry",
                    test_a_stalled_room_refuses_without_advice_to_retry);
    g_test_add_func("/loop-guard/a-system-notice-passes-a-stalled-room",
                    test_a_system_notice_passes_a_stalled_room);
    g_test_add_func("/loop-guard/a-person-reopens-a-stalled-exchange",
                    test_a_person_reopens_a_stalled_exchange);
    g_test_add_func("/loop-guard/a-person-repeating-does-not-stall",
                    test_a_person_repeating_themselves_does_not_stall_the_room);
    g_test_add_func("/loop-guard/without-a-peer-function-nothing-stalls",
                    test_without_a_peer_function_nothing_stalls);
    g_test_add_func("/loop-guard/a-changing-run-id-is-still-caught",
                    test_a_changing_run_id_is_still_caught);
    g_test_add_func("/loop-guard/a-few-numbered-steps-get-through",
                    test_a_few_numbered_steps_get_through);
    g_test_add_func("/loop-guard/an-hourly-status-line-is-not-caught",
                    test_an_hourly_status_line_is_not_caught);
    g_test_add_func("/loop-guard/a-near-match-leaves-the-room-open",
                    test_a_near_match_is_refused_but_leaves_the_room_open);
    g_test_add_func("/loop-guard/the-first-stall-reason-is-kept",
                    test_the_first_stall_reason_is_the_one_kept);
    g_test_add_func("/loop-guard/clearing-forgets-what-was-repeating",
                    test_clearing_a_stall_forgets_what_was_repeating);
    g_test_add_func("/loop-guard/the-stall-signal-names-the-room",
                    test_the_stall_signal_names_the_room_and_the_text);

    return g_test_run();
}
