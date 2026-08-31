/*
 * test-orchestration.c - Rooms, tasks and the limits on agent chatter
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The loop-guard tests matter most.  Agent-to-agent messaging is the point
 * of clawtilla and also the thing most able to run away, and every limit
 * here exists because one of the others does not catch that case.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

/* ── Loop safety ─────────────────────────────────────────────────── */

static ClawtMessage *
message_at_depth(const gchar *sender, const gchar *room,
                 const gchar *body, gint depth)
{
    ClawtMessage *message = clawt_message_new(room, sender, body);

    clawt_message_set_depth(message, depth);

    return message;
}

/* A chain that grows: A asks B asks C asks D. */
static void
test_hop_limit_stops_a_growing_chain(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(ClawtMessage) shallow = NULL;
    g_autoptr(ClawtMessage) deep = NULL;
    g_autoptr(GError) error = NULL;

    clawt_loop_guard_set_limits(guard, 3, 0, 0);

    shallow = message_at_depth("a", "room", "still fine", 2);
    g_assert_true(clawt_loop_guard_check(guard, shallow, &error));

    deep = message_at_depth("a", "room", "too far", 3);
    g_assert_false(clawt_loop_guard_check(guard, deep, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);

    /* The refusal names the limit, so the agent can say something useful. */
    g_assert_nonnull(strstr(error->message, "hops"));
}

/*
 * The case the hop limit misses entirely: two agents alternating the same
 * two replies, every message a fresh chain with a depth of one.
 */
static void
test_cycle_detection_catches_alternating_replies(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    guint round;

    clawt_loop_guard_set_limits(guard, 100, 0, 10);

    /* The first exchange is fine. */
    for (round = 0; round < 1; round++) {
        g_autoptr(ClawtMessage) a = message_at_depth("alice", "room",
                                                     "any update?", 1);
        g_autoptr(ClawtMessage) b = message_at_depth("bob", "room",
                                                     "nothing yet", 1);

        g_assert_true(clawt_loop_guard_check(guard, a, &error));
        g_assert_true(clawt_loop_guard_check(guard, b, &error));
    }

    /* The identical exchange again is a loop, however shallow. */
    {
        g_autoptr(ClawtMessage) again = message_at_depth("alice", "room",
                                                         "any update?", 1);

        g_assert_false(clawt_loop_guard_check(guard, again, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
        g_assert_nonnull(strstr(error->message, "circles"));
    }
}

/* Two agents saying the same thing is normal; one agent repeating is not. */
static void
test_cycle_detection_is_per_sender(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(ClawtMessage) from_alice = NULL;
    g_autoptr(ClawtMessage) from_bob = NULL;
    g_autoptr(GError) error = NULL;

    clawt_loop_guard_set_limits(guard, 100, 0, 10);

    from_alice = message_at_depth("alice", "room", "on it", 1);
    from_bob = message_at_depth("bob", "room", "on it", 1);

    g_assert_true(clawt_loop_guard_check(guard, from_alice, &error));
    g_assert_true(clawt_loop_guard_check(guard, from_bob, &error));
}

/* And the window forgets, so a repeat much later is allowed again. */
static void
test_cycle_window_forgets(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    guint i;

    clawt_loop_guard_set_limits(guard, 100, 0, 3);

    {
        g_autoptr(ClawtMessage) first = message_at_depth("a", "room",
                                                          "hello", 1);
        g_assert_true(clawt_loop_guard_check(guard, first, &error));
    }

    /* Push it out of the window. */
    for (i = 0; i < 4; i++) {
        g_autofree gchar *body = g_strdup_printf("filler %u", i);
        g_autoptr(ClawtMessage) filler = message_at_depth("a", "room",
                                                           body, 1);
        g_assert_true(clawt_loop_guard_check(guard, filler, &error));
    }

    {
        g_autoptr(ClawtMessage) again = message_at_depth("a", "room",
                                                          "hello", 1);
        g_assert_true(clawt_loop_guard_check(guard, again, &error));
    }
}

/*
 * "Recently" is a duration, and a repeat after it has passed is
 * delivered.
 *
 * It was not.  The history queue held bare fingerprints and was trimmed
 * by count alone, so orchestration.cycle_window -- ten messages -- was
 * however long ten messages take.  In a quiet room that is hours.
 *
 * What that cost: an agent hit an unrelated spawn failure and emitted a
 * byte-identical error string every turn.  The first reached the
 * operator at 00:43.  The 06:45 routine and the 07:15 routine were both
 * refused as cycles and produced no output at all, and the operator
 * typed "Fai?" at 11:03 after ten hours of silence.  Identical repeated
 * output is the signature of a stuck system, and the guard's answer to a
 * stuck system was to hide its only symptom.
 *
 * check_rate() in the same file had always done this properly -- a
 * timestamp per entry and a real cutoff.  The check that is time-bounded
 * did not claim to be; the check that claimed to be was not.
 *
 * The window is one second here so the wait is one second.  Both halves
 * are in this test on purpose: without the "still refused inside the
 * window" half it would pass in a build where the cycle check had simply
 * been deleted.
 */
static void
test_the_cycle_window_expires(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;

    clawt_loop_guard_set_limits(guard, 100, 0, 10);
    clawt_loop_guard_set_cycle_seconds(guard, 1);

    {
        g_autoptr(ClawtMessage) first = message_at_depth("fai", "dm:fai:user",
                                                         "Error: E2BIG", 1);

        g_assert_true(clawt_loop_guard_check(guard, first, &error));
    }

    /* Inside the window it is still a loop. This is the control. */
    {
        g_autoptr(ClawtMessage) inside = message_at_depth("fai", "dm:fai:user",
                                                          "Error: E2BIG", 1);

        g_assert_false(clawt_loop_guard_check(guard, inside, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
        g_clear_error(&error);
    }

    /*
     * Past it, the same body is delivered.  A real wait rather than a
     * seam that moves the clock: the thing being fixed is what the guard
     * does as time passes, and a test that sets the timestamps by hand
     * would be on the wrong side of the window it is about.
     */
    g_usleep(1200 * 1000);

    {
        g_autoptr(ClawtMessage) after = message_at_depth("fai", "dm:fai:user",
                                                         "Error: E2BIG", 1);

        g_assert_true(clawt_loop_guard_check(guard, after, &error));
        g_assert_no_error(error);
    }
}

/*
 * And the refusal says the duration it actually enforces.
 *
 * The old text said "recently" against a check with no clock in it. A
 * refusal an agent cannot act on is a refusal that gets retried.
 */
static void
test_the_cycle_refusal_names_its_window(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtMessage) first = NULL;
    g_autoptr(ClawtMessage) again = NULL;

    clawt_loop_guard_set_limits(guard, 100, 0, 10);
    clawt_loop_guard_set_cycle_seconds(guard, 45);

    first = message_at_depth("a", "room", "same", 1);
    again = message_at_depth("a", "room", "same", 1);

    g_assert_true(clawt_loop_guard_check(guard, first, &error));
    g_assert_false(clawt_loop_guard_check(guard, again, &error));
    g_assert_nonnull(strstr(error->message, "45 seconds"));
}

/*
 * A cycle window of zero seconds turns the check off, the way a
 * cycle_window of zero messages already does.
 */
static void
test_a_zero_cycle_window_disables_the_check(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtMessage) first = NULL;
    g_autoptr(ClawtMessage) again = NULL;

    clawt_loop_guard_set_limits(guard, 100, 0, 10);
    clawt_loop_guard_set_cycle_seconds(guard, 0);

    first = message_at_depth("a", "room", "same", 1);
    again = message_at_depth("a", "room", "same", 1);

    g_assert_true(clawt_loop_guard_check(guard, first, &error));
    g_assert_true(clawt_loop_guard_check(guard, again, &error));
}

/* One agent flooding, however shallow each message. */
static void
test_rate_limit_stops_a_flood(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    guint i;

    /* Cycle detection off, so this tests only the rate. */
    clawt_loop_guard_set_limits(guard, 100, 3, 0);

    for (i = 0; i < 3; i++) {
        g_autofree gchar *body = g_strdup_printf("message %u", i);
        g_autoptr(ClawtMessage) message = message_at_depth("chatty", "room",
                                                            body, 1);

        g_assert_true(clawt_loop_guard_check(guard, message, &error));
    }

    {
        g_autoptr(ClawtMessage) one_too_many =
            message_at_depth("chatty", "room", "and another", 1);

        g_assert_false(clawt_loop_guard_check(guard, one_too_many, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
    }

    /* A different agent is unaffected. */
    {
        g_autoptr(ClawtMessage) other = message_at_depth("quiet", "room",
                                                          "hello", 1);
        g_autoptr(GError) other_error = NULL;

        g_assert_true(clawt_loop_guard_check(guard, other, &other_error));
    }
}

/*
 * A message refused on hops must not have consumed part of its sender's
 * rate allowance -- otherwise a deep chain silently spends the budget of
 * whatever tries next.
 */
static void
test_refused_message_does_not_consume_the_rate_allowance(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    guint i;

    clawt_loop_guard_set_limits(guard, 2, 3, 0);

    /* Three refusals on depth. */
    for (i = 0; i < 3; i++) {
        g_autofree gchar *body = g_strdup_printf("too deep %u", i);
        g_autoptr(ClawtMessage) deep = message_at_depth("a", "room", body, 5);
        g_autoptr(GError) deep_error = NULL;

        g_assert_false(clawt_loop_guard_check(guard, deep, &deep_error));
    }

    /* The allowance is untouched. */
    for (i = 0; i < 3; i++) {
        g_autofree gchar *body = g_strdup_printf("fine %u", i);
        g_autoptr(ClawtMessage) shallow = message_at_depth("a", "room",
                                                            body, 0);

        g_assert_true(clawt_loop_guard_check(guard, shallow, &error));
    }
}

/* An expensive loop short enough to pass the other checks. */
static void
test_budget_stops_expensive_work(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(ClawtMessage) message = NULL;
    g_autoptr(GError) error = NULL;

    clawt_loop_guard_set_limits(guard, 100, 0, 0);
    clawt_loop_guard_set_task_budget(guard, 1.0);

    message = message_at_depth("a", "room", "carry on", 1);
    clawt_message_set_task_id(message, "task-1");

    g_assert_true(clawt_loop_guard_check(guard, message, &error));

    clawt_loop_guard_record_spend(guard, "task-1", 0.75);
    g_assert_true(clawt_loop_guard_check(guard, message, &error));

    clawt_loop_guard_record_spend(guard, "task-1", 0.50);
    g_assert_cmpfloat(clawt_loop_guard_get_task_spend(guard, "task-1"),
                      >, 1.0);

    {
        g_autoptr(ClawtMessage) over = message_at_depth("a", "room",
                                                         "more please", 1);
        g_autoptr(GError) over_error = NULL;

        clawt_message_set_task_id(over, "task-1");
        g_assert_false(clawt_loop_guard_check(guard, over, &over_error));
        g_assert_nonnull(strstr(over_error->message, "budget"));
    }

    /* A different task has its own budget. */
    {
        g_autoptr(ClawtMessage) other = message_at_depth("a", "room",
                                                          "unrelated", 1);
        g_autoptr(GError) other_error = NULL;

        clawt_message_set_task_id(other, "task-2");
        g_assert_true(clawt_loop_guard_check(guard, other, &other_error));
    }
}

static void
test_limits_can_be_disabled(void)
{
    g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
    g_autoptr(GError) error = NULL;
    guint i;

    clawt_loop_guard_set_limits(guard, 0, 0, 0);
    clawt_loop_guard_set_task_budget(guard, 0.0);

    for (i = 0; i < 50; i++) {
        g_autoptr(ClawtMessage) message = message_at_depth("a", "room",
                                                            "same thing", 99);

        g_assert_true(clawt_loop_guard_check(guard, message, &error));
    }
}

/* ── Rooms ───────────────────────────────────────────────────────── */

static void
test_room_membership(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("standup", NULL);

    clawt_room_add_member(room, "chief");
    clawt_room_add_member(room, "researcher");
    clawt_room_add_member(room, "chief");   /* twice is once */

    g_assert_cmpuint(clawt_room_get_members(room)->len, ==, 2);
    g_assert_true(clawt_room_has_member(room, "chief"));
    g_assert_false(clawt_room_has_member(room, "nobody"));

    g_assert_true(clawt_room_remove_member(room, "chief"));
    g_assert_false(clawt_room_remove_member(room, "chief"));
    g_assert_cmpuint(clawt_room_get_members(room)->len, ==, 1);
}

/* An agent never receives its own message; that alone is an infinite loop. */
static void
test_message_never_goes_back_to_its_sender(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("standup", NULL);
    g_autoptr(ClawtMessage) message = NULL;

    clawt_room_add_member(room, "chief");
    clawt_room_add_member(room, "researcher");

    message = clawt_message_new("standup", "chief", "anyone free?");

    g_assert_false(clawt_room_message_is_for(room, message, "chief"));
    g_assert_true(clawt_room_message_is_for(room, message, "researcher"));
}

/*
 * With require_mention on, every agent taking a turn on every message is
 * expensive and rarely wanted.
 */
static void
test_require_mention_narrows_delivery(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("standup", NULL);
    g_autoptr(ClawtMessage) addressed = NULL;
    g_autoptr(ClawtMessage) general = NULL;

    clawt_room_add_member(room, "chief");
    clawt_room_add_member(room, "researcher");
    clawt_room_set_require_mention(room, TRUE);

    addressed = clawt_message_new("standup", "chief",
                                  "@researcher can you look at this?");
    general = clawt_message_new("standup", "chief", "morning everyone");

    g_assert_true(clawt_room_message_is_for(room, addressed, "researcher"));
    g_assert_false(clawt_room_message_is_for(room, general, "researcher"));

    /* A bare name counts too: people write both. */
    {
        g_autoptr(ClawtMessage) bare =
            clawt_message_new("standup", "chief", "researcher, take a look");

        g_assert_true(clawt_room_message_is_for(room, bare, "researcher"));
    }
}

static void
test_non_members_never_receive(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("standup", NULL);
    g_autoptr(ClawtMessage) message = NULL;

    clawt_room_add_member(room, "chief");
    message = clawt_message_new("standup", "chief", "hello");

    g_assert_false(clawt_room_message_is_for(room, message, "outsider"));
}

static void
test_room_history_is_kept_and_bounded(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("standup", NULL);
    g_autoptr(GPtrArray) all = NULL;
    g_autoptr(GPtrArray) recent = NULL;
    guint i;

    for (i = 0; i < 10; i++) {
        g_autofree gchar *body = g_strdup_printf("message %u", i);
        g_autoptr(ClawtMessage) message =
            clawt_message_new("standup", "chief", body);

        g_assert_true(clawt_room_append(room, message, NULL));
    }

    all = clawt_room_get_history(room, 0);
    g_assert_cmpuint(all->len, ==, 10);

    recent = clawt_room_get_history(room, 3);
    g_assert_cmpuint(recent->len, ==, 3);

    /* The most recent, and in order. */
    g_assert_cmpstr(
        clawt_message_get_body(g_ptr_array_index(recent, 2)), ==, "message 9");
}

/*
 * A transcript is replayed into every context rebuild, so a key that
 * reached the file would be handed back to the model for ever.
 */
static void
test_transcript_redacts_on_write(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-room-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "standup.jsonl", NULL);
    g_autoptr(ClawtRoom) room = clawt_room_new("standup", path);
    g_autoptr(ClawtMessage) message = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) error = NULL;

    message = clawt_message_new("standup", "chief",
                                "use api_key=sk-ant-abcdefghijklmnopqrstuvwx");
    g_assert_true(clawt_room_append(room, message, &error));
    g_assert_no_error(error);

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_null(strstr(contents, "abcdefghijklmnopqrstuvwx"));
    g_assert_nonnull(strstr(contents, "REDACTED"));

    g_unlink(path);
    clawt_test_remove_tree(dir);
}

/* ── Tasks ───────────────────────────────────────────────────────── */

static void
test_task_lifecycle(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    g_autoptr(GError) error = NULL;
    ClawtTask *task;
    const gchar *task_id;

    task = clawt_task_manager_create(manager, "chief", "researcher",
                                     "summarise the week", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(task);
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);

    task_id = clawt_task_get_id(task);

    g_assert_true(clawt_task_manager_start(manager, task_id));
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);

    g_assert_true(clawt_task_manager_complete(manager, task_id, "here it is"));
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
    g_assert_cmpstr(clawt_task_get_result(task), ==, "here it is");
    g_assert_true(clawt_task_is_finished(task));
    g_assert_cmpint(clawt_task_get_finished_at(task), >, 0);
}

/* Each task gets its own session, so one job never contaminates the next. */
static void
test_tasks_have_distinct_sessions(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    ClawtTask *first;
    ClawtTask *second;

    first = clawt_task_manager_create(manager, "chief", "worker", "a", NULL,
                                      NULL);
    second = clawt_task_manager_create(manager, "chief", "worker", "b", NULL,
                                       NULL);

    g_assert_cmpstr(clawt_task_get_session_key(first), !=,
                    clawt_task_get_session_key(second));
}

/*
 * A late result must not un-cancel a task somebody stopped on purpose.
 */
static void
test_finished_task_stays_finished(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    ClawtTask *task;
    const gchar *task_id;

    task = clawt_task_manager_create(manager, "chief", "worker", "do it",
                                     NULL, NULL);
    task_id = clawt_task_get_id(task);

    clawt_task_manager_cancel(manager, task_id, "changed my mind", "chief");
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_CANCELLED);

    g_assert_false(clawt_task_manager_complete(manager, task_id, "too late"));
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_CANCELLED);
}

/*
 * Cancelling only the parent would leave children running and reporting
 * into a task nobody is waiting for -- the runaway cancellation exists to
 * stop.
 */
static void
test_cancel_reaches_child_tasks(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    ClawtTask *parent;
    ClawtTask *child;
    ClawtTask *grandchild;
    guint cancelled;

    parent = clawt_task_manager_create(manager, "chief", "worker",
                                       "big job", NULL, NULL);
    child = clawt_task_manager_create(manager, "worker", "helper",
                                      "part one",
                                      clawt_task_get_id(parent), NULL);
    grandchild = clawt_task_manager_create(manager, "helper", "specialist",
                                           "a detail",
                                           clawt_task_get_id(child), NULL);

    cancelled = clawt_task_manager_cancel(manager,
                                          clawt_task_get_id(parent),
                                          "no longer needed", "chief");

    g_assert_cmpuint(cancelled, ==, 3);
    g_assert_cmpint(clawt_task_get_state(child), ==, CLAWT_TASK_CANCELLED);
    g_assert_cmpint(clawt_task_get_state(grandchild), ==,
                    CLAWT_TASK_CANCELLED);
}

/* Delegation can nest without any message hop, so depth is bounded here too. */
static void
test_delegation_depth_is_bounded(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    g_autoptr(GError) error = NULL;
    ClawtTask *task;
    const gchar *parent_id = NULL;
    guint i;

    clawt_task_manager_set_max_depth(manager, 3);

    for (i = 0; i < 3; i++) {
        task = clawt_task_manager_create(manager, "a", "b", "deeper",
                                         parent_id, &error);
        g_assert_no_error(error);
        g_assert_nonnull(task);
        parent_id = clawt_task_get_id(task);
    }

    task = clawt_task_manager_create(manager, "a", "b", "too deep",
                                     parent_id, &error);
    g_assert_null(task);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
}

/*
 * A delegator waiting on an agent that stopped would wait for ever, so its
 * tasks fail with a reason.
 */
static void
test_stopped_agent_orphans_its_tasks(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    ClawtTask *theirs;
    ClawtTask *someone_elses;
    guint failed;

    theirs = clawt_task_manager_create(manager, "chief", "researcher",
                                       "one", NULL, NULL);
    someone_elses = clawt_task_manager_create(manager, "chief", "writer",
                                              "two", NULL, NULL);

    clawt_task_manager_start(manager, clawt_task_get_id(theirs));

    failed = clawt_task_manager_orphan_agent_tasks(manager, "researcher");

    g_assert_cmpuint(failed, ==, 1);
    g_assert_cmpint(clawt_task_get_state(theirs), ==, CLAWT_TASK_FAILED);
    g_assert_nonnull(strstr(clawt_task_get_reason(theirs), "stopped"));

    /* Another agent's work is untouched. */
    g_assert_cmpint(clawt_task_get_state(someone_elses), ==,
                    CLAWT_TASK_PENDING);
}

static void
test_task_listing_filters(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    g_autoptr(GPtrArray) live = NULL;
    g_autoptr(GPtrArray) theirs = NULL;
    g_autoptr(GPtrArray) everything = NULL;
    ClawtTask *done;

    clawt_task_manager_create(manager, "chief", "a", "one", NULL, NULL);
    clawt_task_manager_create(manager, "chief", "b", "two", NULL, NULL);
    done = clawt_task_manager_create(manager, "chief", "a", "three", NULL,
                                     NULL);

    clawt_task_manager_complete(manager, clawt_task_get_id(done), "ok");

    live = clawt_task_manager_list(manager, NULL, FALSE);
    g_assert_cmpuint(live->len, ==, 2);

    everything = clawt_task_manager_list(manager, NULL, TRUE);
    g_assert_cmpuint(everything->len, ==, 3);

    theirs = clawt_task_manager_list(manager, "a", TRUE);
    g_assert_cmpuint(theirs->len, ==, 2);
}

/*
 * The listing an agent asks for is not the one the operator asks for.
 * A chief that delegated everything is the assignee of nothing, so the
 * assignee filter -- the only one there was -- answered it with an
 * empty list.
 */
static void
test_task_listing_by_involvement(void)
{
    g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
    g_autoptr(GPtrArray) chiefs = NULL;
    g_autoptr(GPtrArray) theirs = NULL;
    g_autoptr(GPtrArray) live = NULL;
    g_autoptr(GPtrArray) stranger = NULL;
    ClawtTask *done;

    clawt_task_manager_create(manager, "chief", "a", "one", NULL, NULL);
    clawt_task_manager_create(manager, "chief", "b", "two", NULL, NULL);
    /* Work handed *to* the chief, so it is in the listing for a second reason. */
    clawt_task_manager_create(manager, "operator", "chief", "three", NULL, NULL);
    done = clawt_task_manager_create(manager, "chief", "a", "four", NULL, NULL);
    clawt_task_manager_complete(manager, clawt_task_get_id(done), "ok");

    /* Three delegated plus one received; the assignee filter sees one. */
    chiefs = clawt_task_manager_list_involving(manager, "chief", TRUE);
    g_assert_cmpuint(chiefs->len, ==, 4);

    theirs = clawt_task_manager_list(manager, "chief", TRUE);
    g_assert_cmpuint(theirs->len, ==, 1);

    /* A task appears once, however many of its roles the agent fills. */
    live = clawt_task_manager_list_involving(manager, "chief", FALSE);
    g_assert_cmpuint(live->len, ==, 3);

    /* An agent with no part in any of them gets nothing, not everything. */
    stranger = clawt_task_manager_list_involving(manager, "nobody", TRUE);
    g_assert_cmpuint(stranger->len, ==, 0);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/loop/hops", test_hop_limit_stops_a_growing_chain);
    g_test_add_func("/loop/cycle", test_cycle_detection_catches_alternating_replies);
    g_test_add_func("/loop/cycle-per-sender", test_cycle_detection_is_per_sender);
    g_test_add_func("/loop/cycle-forgets", test_cycle_window_forgets);
    g_test_add_func("/loop/cycle-window-expires", test_the_cycle_window_expires);
    g_test_add_func("/loop/cycle-refusal-names-its-window",
                    test_the_cycle_refusal_names_its_window);
    g_test_add_func("/loop/cycle-seconds-zero-disables",
                    test_a_zero_cycle_window_disables_the_check);
    g_test_add_func("/loop/rate", test_rate_limit_stops_a_flood);
    g_test_add_func("/loop/refusal-does-not-charge",
                    test_refused_message_does_not_consume_the_rate_allowance);
    g_test_add_func("/loop/budget", test_budget_stops_expensive_work);
    g_test_add_func("/loop/disabled", test_limits_can_be_disabled);

    g_test_add_func("/room/membership", test_room_membership);
    g_test_add_func("/room/never-back-to-sender",
                    test_message_never_goes_back_to_its_sender);
    g_test_add_func("/room/require-mention", test_require_mention_narrows_delivery);
    g_test_add_func("/room/non-members", test_non_members_never_receive);
    g_test_add_func("/room/history", test_room_history_is_kept_and_bounded);
    g_test_add_func("/room/transcript-redacts", test_transcript_redacts_on_write);

    g_test_add_func("/task/lifecycle", test_task_lifecycle);
    g_test_add_func("/task/distinct-sessions", test_tasks_have_distinct_sessions);
    g_test_add_func("/task/finished-stays-finished",
                    test_finished_task_stays_finished);
    g_test_add_func("/task/cancel-cascades", test_cancel_reaches_child_tasks);
    g_test_add_func("/task/depth-bounded", test_delegation_depth_is_bounded);
    g_test_add_func("/task/orphans", test_stopped_agent_orphans_its_tasks);
    g_test_add_func("/task/listing", test_task_listing_filters);
    g_test_add_func("/task/listing-by-involvement",
                    test_task_listing_by_involvement);

    return g_test_run();
}
