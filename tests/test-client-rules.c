/*
 * test-client-rules.c - The two rules both graphical clients apply
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An unread count and an alert tier are decided identically by the GTK
 * client and the web client, and were implemented twice before this --
 * which is the shape of drift `make parity` exists to catch and cannot
 * see, because neither rule sends a frame or answers a command.  Pure
 * functions, so every branch is exercised without a window, a browser or
 * a daemon.
 */

#include "clawtilla.h"

#include <glib.h>

/* Anything after this instant is live rather than replayed. */
#define CONNECTED_AT G_GINT64_CONSTANT(1000000)
#define LIVE         (CONNECTED_AT + 1000)
#define REPLAYED     (CONNECTED_AT - 1000)

static void
test_a_message_elsewhere_counts(void)
{
    g_assert_true(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                            "beta", LIVE, CONNECTED_AT));
}

/*
 * A conversation on screen never accrues one, whatever the scroll
 * position: that case belongs to the transcript's "New messages" rule,
 * which deliberately carries no number.  The two must never fire for the
 * same message.
 */
static void
test_the_room_on_screen_does_not(void)
{
    g_assert_false(clawt_unread_should_count("dm:alpha:user",
                                             "dm:alpha:user", "alpha", LIVE,
                                             CONNECTED_AT));
}

/* Nothing on screen at all -- a settings page -- still counts. */
static void
test_no_room_on_screen_still_counts(void)
{
    g_assert_true(clawt_unread_should_count("dm:alpha:user", NULL, "alpha",
                                            LIVE, CONNECTED_AT));
}

static void
test_your_own_message_does_not(void)
{
    g_assert_false(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                             "user", LIVE, CONNECTED_AT));
}

/*
 * A client subscribes from cursor 0 and the daemon replays its recent
 * events, so the first thing a fresh window receives is everything that
 * just happened -- possibly read in the previous session.  Counting
 * those opens a window already showing a number for a conversation
 * nobody has touched.
 */
static void
test_a_replayed_message_does_not(void)
{
    g_assert_false(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                             "beta", REPLAYED,
                                             CONNECTED_AT));
}

/* An event with no timestamp is taken at face value rather than dropped. */
static void
test_an_undated_message_counts(void)
{
    g_assert_true(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                            "beta", 0, CONNECTED_AT));
}

static void
test_nothing_without_a_room_or_a_sender(void)
{
    g_assert_false(clawt_unread_should_count(NULL, NULL, "beta", LIVE,
                                             CONNECTED_AT));
    g_assert_false(clawt_unread_should_count("dm:beta:user", NULL, NULL,
                                             LIVE, CONNECTED_AT));
}

/* ── Following the live edge ─────────────────────────────────────── */

/*
 * The predicate the whole follow behaviour turns on.
 *
 * A client refuses to move the view when it is false, and both unread
 * affordances -- the pill and the rule in the transcript -- are driven by
 * the edge where it changes.  It was thirty-two pixels of arithmetic
 * inside a signal handler and could not be tested at all; it is a pure
 * function now, and these are the cases nobody drives by hand.
 */
static void
test_at_the_bottom_follows(void)
{
    /* Exactly there. */
    g_assert_true(clawt_transcript_is_at_bottom(1000.0, 1600.0, 600.0));

    /* Within the tolerance, which exists because a scrolled window
     * rarely lands on an exact value. */
    g_assert_true(clawt_transcript_is_at_bottom(969.0, 1600.0, 600.0));

    /* Past the end, which GTK clamps but which must not read as away. */
    g_assert_true(clawt_transcript_is_at_bottom(1200.0, 1600.0, 600.0));
}

static void
test_away_from_the_bottom_does_not(void)
{
    g_assert_false(clawt_transcript_is_at_bottom(0.0, 1600.0, 600.0));
    g_assert_false(clawt_transcript_is_at_bottom(900.0, 1600.0, 600.0));
}

/*
 * The boundary itself, asserted on both sides.
 *
 * A tolerance nobody tests at its edge is a tolerance that quietly
 * becomes off-by-one, and every pixel of it is a pixel of message a new
 * arrival can push off the bottom without the client noticing it stopped
 * following.
 */
static void
test_the_tolerance_boundary(void)
{
    gdouble bottom = 1000.0;

    /* One pixel inside. */
    g_assert_true(clawt_transcript_is_at_bottom(
        bottom - (CLAWT_TRANSCRIPT_FOLLOW_TOLERANCE - 1.0), 1600.0, 600.0));

    /* Exactly the tolerance is *not* inside: the test is strictly less. */
    g_assert_false(clawt_transcript_is_at_bottom(
        bottom - CLAWT_TRANSCRIPT_FOLLOW_TOLERANCE, 1600.0, 600.0));
}

/*
 * A transcript shorter than its viewport is at the bottom by definition:
 * there is nowhere else to be.
 *
 * The arithmetic happened to give the right answer here already -- a
 * negative bottom is still less than the tolerance -- so the explicit
 * guard is not a fix.  It is here because "shorter than the viewport"
 * being correct by accident is the kind of thing a later edit breaks
 * without noticing, and these three cases are what would notice.
 */
static void
test_a_short_transcript_is_always_at_the_bottom(void)
{
    g_assert_true(clawt_transcript_is_at_bottom(0.0, 200.0, 600.0));
    g_assert_true(clawt_transcript_is_at_bottom(0.0, 0.0, 0.0));
    g_assert_true(clawt_transcript_is_at_bottom(0.0, 600.0, 600.0));
}

/* ── Alert tiers ─────────────────────────────────────────────────── */

static ClawtAlertTier
tier_of(const gchar *kind, const gchar *detail_key, const gchar *detail)
{
    g_autoptr(ClawtEvent) event = clawt_event_new(kind, "alpha");

    if (detail_key != NULL)
        clawt_event_set_detail(event, detail_key, detail);

    return clawt_alert_tier_for_event(event);
}

/*
 * The two that arrive on their own.  Everywhere else a client says
 * something, it is answering a question somebody is holding right now.
 */
static void
test_the_two_loud_kinds(void)
{
    g_assert_cmpint(tier_of("message.refused", "reason", "a limit"), ==,
                    CLAWT_ALERT_ERROR);
    g_assert_cmpint(tier_of("image.finished", "error", "404"), ==,
                    CLAWT_ALERT_ERROR);
}

/*
 * A download that *succeeded* is routine.  Only the failure arrived on
 * its own with nobody watching, so classifying on the kind alone would
 * put every completed download in the loud list.
 */
static void
test_a_successful_download_is_routine(void)
{
    g_assert_cmpint(tier_of("image.finished", NULL, NULL), ==,
                    CLAWT_ALERT_ROUTINE);
}

/*
 * An agent that stopped when nobody asked it to is the one routine event
 * that is not routine -- and a notice rather than an error, because it
 * may well have been asked to.
 */
static void
test_a_bad_agent_state_is_a_notice(void)
{
    g_assert_cmpint(tier_of("agent.state", "state", "error"), ==,
                    CLAWT_ALERT_NOTICE);
    g_assert_cmpint(tier_of("agent.state", "state", "degraded"), ==,
                    CLAWT_ALERT_NOTICE);
    g_assert_cmpint(tier_of("agent.state", "state", "running"), ==,
                    CLAWT_ALERT_ROUTINE);
}

/*
 * One entry per percent would fill the whole list with one file, and
 * typing is a spinner rather than something that happened.
 */
static void
test_the_noisy_kinds_are_skipped(void)
{
    g_assert_cmpint(tier_of("image.progress", "total", "100"), ==,
                    CLAWT_ALERT_SKIP);
    g_assert_cmpint(tier_of("agent.typing", "typing", "true"), ==,
                    CLAWT_ALERT_SKIP);
}

/* Anything the daemon grows later is routine rather than dropped. */
static void
test_an_unknown_kind_is_routine(void)
{
    g_assert_cmpint(tier_of("something.new", NULL, NULL), ==,
                    CLAWT_ALERT_ROUTINE);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/client-rules/unread/elsewhere",
                    test_a_message_elsewhere_counts);
    g_test_add_func("/client-rules/unread/on-screen",
                    test_the_room_on_screen_does_not);
    g_test_add_func("/client-rules/unread/no-room-on-screen",
                    test_no_room_on_screen_still_counts);
    g_test_add_func("/client-rules/unread/your-own",
                    test_your_own_message_does_not);
    g_test_add_func("/client-rules/unread/replayed",
                    test_a_replayed_message_does_not);
    g_test_add_func("/client-rules/unread/undated",
                    test_an_undated_message_counts);
    g_test_add_func("/client-rules/unread/incomplete",
                    test_nothing_without_a_room_or_a_sender);

    g_test_add_func("/client-rules/follow/at-the-bottom",
                    test_at_the_bottom_follows);
    g_test_add_func("/client-rules/follow/away",
                    test_away_from_the_bottom_does_not);
    g_test_add_func("/client-rules/follow/boundary",
                    test_the_tolerance_boundary);
    g_test_add_func("/client-rules/follow/short-transcript",
                    test_a_short_transcript_is_always_at_the_bottom);

    g_test_add_func("/client-rules/tier/loud", test_the_two_loud_kinds);
    g_test_add_func("/client-rules/tier/download-ok",
                    test_a_successful_download_is_routine);
    g_test_add_func("/client-rules/tier/bad-state",
                    test_a_bad_agent_state_is_a_notice);
    g_test_add_func("/client-rules/tier/skipped",
                    test_the_noisy_kinds_are_skipped);
    g_test_add_func("/client-rules/tier/unknown",
                    test_an_unknown_kind_is_routine);

    return g_test_run();
}
