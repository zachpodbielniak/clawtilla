/*
 * test-chat-run.c - Grouping consecutive messages into runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The rule is a pure function precisely so it can be tested without a
 * transcript, a window or a daemon -- both clients ask it the same
 * question, and two implementations of "is this a new run" would differ
 * exactly once, on the case nobody looked at.
 */

#include "clawtilla.h"

#include <glib.h>

static void
test_the_first_message_starts_a_run(void)
{
    gboolean new_day = FALSE;

    /*
     * Nothing before it, so it is both a run start and a new day -- and
     * the new day is what puts a divider at the top of a loaded history
     * rather than leaving the first block undated.
     */
    g_assert_true(clawt_chat_run_is_start(NULL, NULL, "alpha", "2026-08-25",
                                          &new_day));
    g_assert_true(new_day);
}

static void
test_the_same_sender_continues(void)
{
    gboolean new_day = TRUE;

    g_assert_false(clawt_chat_run_is_start("alpha", "2026-08-25", "alpha",
                                           "2026-08-25", &new_day));
    g_assert_false(new_day);
}

static void
test_a_different_sender_starts_a_run(void)
{
    gboolean new_day = TRUE;

    g_assert_true(clawt_chat_run_is_start("alpha", "2026-08-25", "user",
                                          "2026-08-25", &new_day));

    /* A speaker change is not a date change. */
    g_assert_false(new_day);
}

/*
 * A day boundary ends a run even when the sender has not changed.
 *
 * Without it a conversation that ran past midnight would have one header
 * for both days, and the divider between them would sit inside a block
 * it does not divide.
 */
static void
test_a_new_day_starts_a_run(void)
{
    gboolean new_day = FALSE;

    g_assert_true(clawt_chat_run_is_start("alpha", "2026-08-25", "alpha",
                                          "2026-08-26", &new_day));
    g_assert_true(new_day);
}

/*
 * The relative labels, against a fixed "now" rather than the real clock.
 *
 * Testing these against g_date_time_new_now_local() would pass all day
 * and fail for whoever ran the suite across midnight.
 */
static void
test_day_labels(void)
{
    g_autoptr(GDateTime) now = g_date_time_new_local(2026, 8, 25, 14, 0, 0);
    g_autoptr(GDateTime) yesterday = g_date_time_new_local(2026, 8, 24, 9, 0,
                                                           0);
    g_autoptr(GDateTime) older = g_date_time_new_local(2026, 8, 5, 9, 0, 0);
    g_autofree gchar *a = clawt_chat_day_label(now, now);
    g_autofree gchar *b = clawt_chat_day_label(yesterday, now);
    g_autofree gchar *c = clawt_chat_day_label(older, now);

    g_assert_cmpstr(a, ==, "Today");
    g_assert_cmpstr(b, ==, "Yesterday");

    /*
     * A single-digit day, asserted rather than assumed.  GLib pads "%e"
     * with U+2007 FIGURE SPACE, which g_strstrip() cannot remove because
     * it is in the middle of the string -- so "%A %e %B" renders
     * "Wednesday" then two spaces then "5", on the nine days a month it
     * comes up.  This test found that on its first run.
     */
    g_assert_cmpstr(c, ==, "Wednesday 5 August");
}

/*
 * A year boundary is a new day even though the day-of-year goes back to
 * 1, which a comparison on day-of-year alone would read as a jump
 * backwards rather than as a change.
 */
static void
test_new_year_is_a_new_day(void)
{
    g_autoptr(GDateTime) now = g_date_time_new_local(2027, 1, 1, 0, 30, 0);
    g_autoptr(GDateTime) last_year = g_date_time_new_local(2026, 1, 1, 0, 30,
                                                           0);
    g_autofree gchar *label = clawt_chat_day_label(last_year, now);

    g_assert_cmpstr(label, ==, "Thursday 1 January");
    g_assert_true(clawt_chat_run_is_start("alpha", "2026-12-31", "alpha",
                                          "2027-01-01", NULL));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/chat-run/first-message", test_the_first_message_starts_a_run);
    g_test_add_func("/chat-run/same-sender", test_the_same_sender_continues);
    g_test_add_func("/chat-run/different-sender",
                    test_a_different_sender_starts_a_run);
    g_test_add_func("/chat-run/new-day", test_a_new_day_starts_a_run);
    g_test_add_func("/chat-run/day-labels", test_day_labels);
    g_test_add_func("/chat-run/new-year", test_new_year_is_a_new_day);

    return g_test_run();
}
