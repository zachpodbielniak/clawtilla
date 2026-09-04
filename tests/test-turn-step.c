/*
 * test-turn-step.c - The steps of a running turn
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two things are being tested and only one of them is behaviour.
 *
 * The first is that the reader and the producer agree.  A member name
 * is checked by nothing, so a reader asking for `kind` where the
 * producer wrote `step_kind` gets its fallback on every call and
 * reports nothing -- which this project has shipped six times.  The
 * constants make that a compile error; these tests make the *values*
 * they carry a test failure.
 *
 * The second is a negative: a step must have no path to delivery.  That
 * is the whole safety property of the feature and the one thing a
 * future edit could quietly take away, so it is asserted rather than
 * described.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

/* ── The event reader ────────────────────────────────────────────── */

static ClawtEvent *
tool_event(void)
{
    ClawtEvent *event = clawt_event_new("turn.step", "scribe");

    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_KIND, "tool");
    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_ROOM, "dm:user:scribe");
    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_TOOL, "Bash");
    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_DETAIL, "make test");
    clawt_event_set_detail_int(event, CLAWT_STEP_MEMBER_FAILED, 1);

    return event;
}

static void
test_every_member_survives_the_event(void)
{
    g_autoptr(ClawtEvent) event = tool_event();
    g_autoptr(ClawtTurnStep) step = clawt_turn_step_new_from_event(event);

    g_assert_nonnull(step);
    g_assert_cmpint(clawt_turn_step_get_kind(step), ==, CLAWT_STEP_TOOL);
    g_assert_cmpstr(clawt_turn_step_get_agent_id(step), ==, "scribe");
    g_assert_cmpstr(clawt_turn_step_get_room_id(step), ==, "dm:user:scribe");
    g_assert_cmpstr(clawt_turn_step_get_tool_name(step), ==, "Bash");
    g_assert_cmpstr(clawt_turn_step_get_detail(step), ==, "make test");
    g_assert_true(clawt_turn_step_get_failed(step));
}

/*
 * The one that would have caught the whole class of bug this file is
 * about: every getter is asserted against a value that differs from
 * every other, so a reader wired to the wrong member fails rather than
 * passing on a coincidence.
 */
static void
test_a_wrong_member_would_be_visible(void)
{
    g_autoptr(ClawtEvent) event = tool_event();
    g_autoptr(ClawtTurnStep) step = clawt_turn_step_new_from_event(event);

    g_assert_cmpstr(clawt_turn_step_get_tool_name(step), !=,
                    clawt_turn_step_get_detail(step));
    g_assert_cmpstr(clawt_turn_step_get_room_id(step), !=,
                    clawt_turn_step_get_agent_id(step));
    g_assert_null(clawt_turn_step_get_text(step));
}

static void
test_another_kind_of_event_is_not_a_step(void)
{
    g_autoptr(ClawtEvent) event = clawt_event_new("agent.typing", "scribe");

    g_assert_null(clawt_turn_step_new_from_event(event));
}

/*
 * Zero is a real member of the enum, so an unrecognised kind must not
 * land on it.  `text` is the kind a client renders in the agent's own
 * voice; a step this build does not understand being drawn as the
 * agent's prose is the wrong direction to be wrong in.
 */
static void
test_an_unknown_kind_reads_as_status(void)
{
    g_autoptr(ClawtEvent) event = clawt_event_new("turn.step", "scribe");
    g_autoptr(ClawtTurnStep) step = NULL;

    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_KIND, "telepathy");
    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_ROOM, "r");

    step = clawt_turn_step_new_from_event(event);

    g_assert_nonnull(step);
    g_assert_cmpint(clawt_turn_step_get_kind(step), ==, CLAWT_STEP_STATUS);
    g_assert_cmpint(clawt_turn_step_get_kind(step), !=, CLAWT_STEP_TEXT);
}

/* ── The array reader ────────────────────────────────────────────── */

static void
test_the_array_reader_agrees_with_the_event_reader(void)
{
    g_autoptr(JsonObject) object = json_object_new();
    g_autoptr(ClawtTurnStep) step = NULL;

    json_object_set_string_member(object, CLAWT_STEP_MEMBER_KIND, "tool");
    json_object_set_string_member(object, CLAWT_STEP_MEMBER_ROOM, "r1");
    json_object_set_string_member(object, CLAWT_STEP_MEMBER_TOOL, "Read");
    json_object_set_string_member(object, CLAWT_STEP_MEMBER_DETAIL, "/etc/hosts");

    /*
     * A JSON *boolean*, which is what the daemon writes.  Read with the
     * string reader it would come back as the fallback and every failed
     * tool would draw as a successful one -- the same failure as a
     * misspelled member, with nothing misspelled in it to notice.
     */
    json_object_set_boolean_member(object, CLAWT_STEP_MEMBER_FAILED, TRUE);

    step = clawt_turn_step_new_from_object(object, "kudu");

    g_assert_nonnull(step);
    g_assert_cmpint(clawt_turn_step_get_kind(step), ==, CLAWT_STEP_TOOL);
    g_assert_cmpstr(clawt_turn_step_get_agent_id(step), ==, "kudu");
    g_assert_cmpstr(clawt_turn_step_get_room_id(step), ==, "r1");
    g_assert_cmpstr(clawt_turn_step_get_tool_name(step), ==, "Read");
    g_assert_cmpstr(clawt_turn_step_get_detail(step), ==, "/etc/hosts");
    g_assert_true(clawt_turn_step_get_failed(step));
}

static void
test_a_missing_failed_member_is_not_a_failure(void)
{
    g_autoptr(JsonObject) object = json_object_new();
    g_autoptr(ClawtTurnStep) step = NULL;

    json_object_set_string_member(object, CLAWT_STEP_MEMBER_KIND, "tool");
    json_object_set_string_member(object, CLAWT_STEP_MEMBER_TOOL, "Read");

    step = clawt_turn_step_new_from_object(object, NULL);

    g_assert_nonnull(step);
    g_assert_false(clawt_turn_step_get_failed(step));
}

/* ── The rules both clients share ────────────────────────────────── */

/*
 * Walked rather than listed.  clawt_task_state_tone() was written by
 * comparing a state against nicknames the enum does not produce, so
 * every completed task was drawn grey for a year and a wrong colour
 * reports itself to nobody.  The switch has no `default:`, so a kind
 * added to the enum is a -Wswitch warning; this is what makes it a
 * failure as well.
 */
static void
test_every_kind_has_a_tone(void)
{
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_STEP_KIND);
    static const gchar * const painted[] = {
        "neutral", "good", "warn", "bad", "info", NULL
    };
    guint i;

    for (i = 0; i < klass->n_values; i++) {
        g_autoptr(ClawtTurnStep) step = clawt_turn_step_new(
            (ClawtStepKind)klass->values[i].value, "a", "r", "t", NULL,
            NULL, FALSE);
        const gchar *tone = clawt_turn_step_tone(step);

        g_assert_nonnull(tone);
        g_assert_true(g_strv_contains(painted, tone));
    }
}

static void
test_a_failed_tool_is_the_only_bad_tone(void)
{
    g_autoptr(ClawtTurnStep) ok = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "a", "r", NULL, "Bash", "ls", FALSE);
    g_autoptr(ClawtTurnStep) bad = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "a", "r", NULL, "Bash", "ls", TRUE);

    g_assert_cmpstr(clawt_turn_step_tone(ok), !=, "bad");
    g_assert_cmpstr(clawt_turn_step_tone(bad), ==, "bad");
}

static void
test_a_run_label_counts_and_says_what_failed(void)
{
    g_autofree gchar *one = clawt_turn_step_run_label(1, 0);
    g_autofree gchar *many = clawt_turn_step_run_label(6, 0);
    g_autofree gchar *broken = clawt_turn_step_run_label(13, 1);

    g_assert_cmpstr(one, ==, "Ran 1 command");
    g_assert_cmpstr(many, ==, "Ran 6 commands");
    g_assert_cmpstr(broken, ==, "Ran 13 commands (1 failed)");

    /*
     * A failure is never folded into the count.  A run that contains one
     * means something different from a run that does not, and a label
     * that hid it would make a struggling turn look productive.
     */
    g_assert_nonnull(strstr(broken, "failed"));
}

static void
test_only_a_tool_joins_a_run(void)
{
    g_autoptr(ClawtTurnStep) tool = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "a", "r", NULL, "Bash", NULL, FALSE);
    g_autoptr(ClawtTurnStep) text = clawt_turn_step_new(
        CLAWT_STEP_TEXT, "a", "r", "hello", NULL, NULL, FALSE);

    g_assert_true(clawt_turn_step_joins_run(tool));
    g_assert_false(clawt_turn_step_joins_run(text));
}

static void
test_a_summary_is_one_line(void)
{
    g_autoptr(ClawtTurnStep) prose = clawt_turn_step_new(
        CLAWT_STEP_TEXT, "a", "r", "first line\nsecond line", NULL, NULL,
        FALSE);
    g_autoptr(ClawtTurnStep) named = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "a", "r", NULL, "Bash", "make test", FALSE);
    g_autoptr(ClawtTurnStep) bare = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "a", "r", NULL, "Read", NULL, FALSE);
    g_autofree gchar *a = clawt_turn_step_summary(prose);
    g_autofree gchar *b = clawt_turn_step_summary(named);
    g_autofree gchar *c = clawt_turn_step_summary(bare);

    g_assert_cmpstr(a, ==, "first line");
    g_assert_null(strchr(a, '\n'));
    g_assert_cmpstr(b, ==, "Bash: make test");
    g_assert_cmpstr(c, ==, "Read");
}

/* ── Merging steps back into a transcript ────────────────────────── */

/*
 * The two stamps are in different units, and this is the function that
 * knows it.
 *
 * A ClawtTurnStep is stamped from ClawtEvent's clock -- g_get_real_time(),
 * microseconds -- and a ClawtMessage stamps itself in seconds.  Compared
 * raw, every step is "after" every message by a factor of a million, so
 * a conversation's whole tool history piles up at the bottom under the
 * answers it came before -- and that reads as an ordering preference
 * rather than a bug.
 */
static void
test_a_step_is_placed_by_seconds_not_microseconds(void)
{
    g_autoptr(ClawtTurnStep) step = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "a", "r", NULL, "Bash", "ls", FALSE);
    gint64 now_seconds = g_get_real_time() / G_USEC_PER_SEC;

    /*
     * The step was stamped just now, so it precedes a message stamped
     * now -- and, decisively, it does *not* precede one from an hour
     * ago.  A raw comparison would fail the second of these, because
     * the microsecond figure dwarfs any plausible second figure.
     */
    g_assert_true(clawt_turn_step_precedes(step, now_seconds));
    g_assert_false(clawt_turn_step_precedes(step, now_seconds - 3600));
}

/*
 * A step from a daemon too old to send a time sorts first rather than
 * being dropped into the middle of somebody else's turn.
 */
static void
test_an_unstamped_step_sorts_first(void)
{
    g_autoptr(JsonObject) object = json_object_new();
    g_autoptr(ClawtTurnStep) step = NULL;

    json_object_set_string_member(object, CLAWT_STEP_MEMBER_KIND, "tool");
    json_object_set_string_member(object, CLAWT_STEP_MEMBER_TOOL, "Bash");

    step = clawt_turn_step_new_from_object(object, NULL);

    g_assert_nonnull(step);
    g_assert_cmpint(clawt_turn_step_get_timestamp(step), ==, 0);
    g_assert_true(clawt_turn_step_precedes(step, 0));
}

/*
 * The timestamp survives the array form, in the unit it was sent in.
 */
static void
test_a_timestamp_round_trips(void)
{
    g_autoptr(JsonObject) object = json_object_new();
    g_autoptr(ClawtTurnStep) step = NULL;
    gint64 stamp = 1788000000LL * G_USEC_PER_SEC;

    json_object_set_string_member(object, CLAWT_STEP_MEMBER_KIND, "text");
    json_object_set_int_member(object, CLAWT_STEP_MEMBER_TS, stamp);

    step = clawt_turn_step_new_from_object(object, NULL);

    g_assert_nonnull(step);
    g_assert_cmpint(clawt_turn_step_get_timestamp(step), ==, stamp);
    g_assert_true(clawt_turn_step_precedes(step, 1788000000LL));
    g_assert_false(clawt_turn_step_precedes(step, 1787999999LL));
}

/* ── What a step must never reach ────────────────────────────────── */

/*
 * A step is not kept anywhere.
 *
 * The bus's replay ring is small and shared: a turn makes tens of tool
 * calls, so retaining steps would evict real events within one busy
 * turn and a client resuming from a cursor would be told its replay was
 * incomplete precisely because an agent had been working.
 */
static void
test_a_step_is_never_retained(void)
{
    g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(16);
    g_autoptr(ClawtEvent) step = tool_event();
    g_autoptr(ClawtEvent) real = clawt_event_new("agent.state", "scribe");
    g_autoptr(GPtrArray) replay = NULL;
    gboolean complete = FALSE;
    guint i;

    clawt_event_bus_publish(bus, step);
    clawt_event_bus_publish(bus, real);

    replay = clawt_event_bus_replay(bus, 0, &complete);

    for (i = 0; i < replay->len; i++) {
        ClawtEvent *held = g_ptr_array_index(replay, i);

        g_assert_cmpstr(clawt_event_get_kind(held), !=, "turn.step");
    }

    /*
     * And the real one is still there -- an exclusion that dropped
     * everything would pass the loop above and be useless.
     */
    g_assert_cmpuint(replay->len, ==, 1);
    g_assert_cmpstr(clawt_event_get_kind(g_ptr_array_index(replay, 0)), ==,
                    "agent.state");
}

static void
test_only_a_step_is_ephemeral(void)
{
    g_autoptr(ClawtEvent) step = clawt_event_new("turn.step", "a");
    g_autoptr(ClawtEvent) message = clawt_event_new("message", "a");
    g_autoptr(ClawtEvent) typing = clawt_event_new("agent.typing", "a");

    g_assert_true(clawt_event_is_ephemeral(step));
    g_assert_false(clawt_event_is_ephemeral(message));

    /*
     * Typing is skipped from the *alert list* but is still a thing that
     * happened and is still replayed, so a client reconnecting learns
     * an agent is busy.  The two questions are different and the
     * answers deliberately differ.
     */
    g_assert_false(clawt_event_is_ephemeral(typing));
}

/*
 * A step is never an alert.  A turn makes tens of them, and a list that
 * filled with one agent's tool calls would bury everything a person
 * keeps that list for.
 */
static void
test_a_step_is_not_an_alert(void)
{
    g_autoptr(ClawtEvent) step = clawt_event_new("turn.step", "a");

    g_assert_cmpint(clawt_alert_tier_for_event(step), ==, CLAWT_ALERT_SKIP);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/turn-step/every-member-survives",
                    test_every_member_survives_the_event);
    g_test_add_func("/turn-step/a-wrong-member-would-be-visible",
                    test_a_wrong_member_would_be_visible);
    g_test_add_func("/turn-step/not-a-step",
                    test_another_kind_of_event_is_not_a_step);
    g_test_add_func("/turn-step/unknown-kind-is-status",
                    test_an_unknown_kind_reads_as_status);
    g_test_add_func("/turn-step/array-reader-agrees",
                    test_the_array_reader_agrees_with_the_event_reader);
    g_test_add_func("/turn-step/missing-failed",
                    test_a_missing_failed_member_is_not_a_failure);
    g_test_add_func("/turn-step/every-kind-has-a-tone",
                    test_every_kind_has_a_tone);
    g_test_add_func("/turn-step/failed-is-the-only-bad",
                    test_a_failed_tool_is_the_only_bad_tone);
    g_test_add_func("/turn-step/run-label", test_a_run_label_counts_and_says_what_failed);
    g_test_add_func("/turn-step/only-tools-join-a-run",
                    test_only_a_tool_joins_a_run);
    g_test_add_func("/turn-step/summary-is-one-line",
                    test_a_summary_is_one_line);
    g_test_add_func("/turn-step/placed-by-seconds",
                    test_a_step_is_placed_by_seconds_not_microseconds);
    g_test_add_func("/turn-step/unstamped-sorts-first",
                    test_an_unstamped_step_sorts_first);
    g_test_add_func("/turn-step/timestamp-round-trips",
                    test_a_timestamp_round_trips);
    g_test_add_func("/turn-step/never-retained",
                    test_a_step_is_never_retained);
    g_test_add_func("/turn-step/only-a-step-is-ephemeral",
                    test_only_a_step_is_ephemeral);
    g_test_add_func("/turn-step/not-an-alert", test_a_step_is_not_an_alert);

    return g_test_run();
}
