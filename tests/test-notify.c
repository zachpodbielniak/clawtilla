/*
 * test-notify.c - What is worth interrupting somebody for, and how
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A notifier is the one thing in a fleet you cannot tell is working by
 * looking at it: it is correct precisely when nothing happens.  So the
 * parts that decide *whether* to fire are pure functions, and this is
 * where they are held to it -- including the one that is only wrong
 * between eleven at night and seven in the morning.
 */

#include <clawtilla.h>

#include <string.h>

/* ── Quiet hours ─────────────────────────────────────────────────── */

static gint
minutes(gint hour, gint minute)
{
    return hour * 60 + minute;
}

static void
test_a_range_is_parsed(void)
{
    gint start = 0;
    gint end = 0;

    g_assert_true(clawt_notify_parse_quiet_hours("23:00-07:00", &start,
                                                 &end));
    g_assert_cmpint(start, ==, minutes(23, 0));
    g_assert_cmpint(end, ==, minutes(7, 0));

    /* Spaces around the dash are what people type. */
    g_assert_true(clawt_notify_parse_quiet_hours("09:30 - 17:45", &start,
                                                 &end));
    g_assert_cmpint(start, ==, minutes(9, 30));
    g_assert_cmpint(end, ==, minutes(17, 45));
}

static void
test_nonsense_is_not_a_range(void)
{
    gint start = 0;
    gint end = 0;

    g_assert_false(clawt_notify_parse_quiet_hours(NULL, &start, &end));
    g_assert_false(clawt_notify_parse_quiet_hours("", &start, &end));
    g_assert_false(clawt_notify_parse_quiet_hours("23:00", &start, &end));
    g_assert_false(clawt_notify_parse_quiet_hours("25:00-07:00", &start,
                                                  &end));
    g_assert_false(clawt_notify_parse_quiet_hours("23:60-07:00", &start,
                                                  &end));
    g_assert_false(clawt_notify_parse_quiet_hours("night-morning", &start,
                                                  &end));
    g_assert_false(clawt_notify_parse_quiet_hours("23:00-07:00-09:00",
                                                  &start, &end));
}

/*
 * The one that matters.  People sleep across midnight, so the wrapping
 * range is the ordinary spelling -- and getting it backwards produces a
 * notifier that is silent all day and loud all night, which is the same
 * bug either way round and only noticed at 3am.
 */
static void
test_a_range_that_wraps_midnight(void)
{
    gint start = minutes(23, 0);
    gint end = minutes(7, 0);

    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(23, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(23, 59)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(0, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(3, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(6, 59)));

    /* And loud for the rest of it. */
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(7, 0)));
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(12, 0)));
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(22, 59)));
}

static void
test_a_range_that_does_not_wrap(void)
{
    gint start = minutes(9, 0);
    gint end = minutes(17, 0);

    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(8, 59)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(9, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(16, 59)));

    /* The end is exclusive, so 17:00 is audible again. */
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(17, 0)));
}

/* ── Which events ────────────────────────────────────────────────── */

static void
test_events_are_read_from_names(void)
{
    static const gchar *const both[] = { "question", "error", NULL };
    static const gchar *const one[] = { "done", NULL };
    static const gchar *const none[] = { NULL };
    g_autoptr(GError) error = NULL;

    g_assert_cmpuint(clawt_notify_events_from_strv(both, &error), ==,
                     CLAWT_NOTIFY_EVENTS_QUESTION |
                     CLAWT_NOTIFY_EVENTS_ERROR);
    g_assert_no_error(error);

    g_assert_cmpuint(clawt_notify_events_from_strv(one, &error), ==,
                     CLAWT_NOTIFY_EVENTS_DONE);
    g_assert_cmpuint(clawt_notify_events_from_strv(none, &error), ==,
                     CLAWT_NOTIFY_EVENTS_NONE);
    g_assert_cmpuint(clawt_notify_events_from_strv(NULL, &error), ==,
                     CLAWT_NOTIFY_EVENTS_NONE);
}

/*
 * A name nobody recognises is an error rather than a silent omission.
 * The failure it prevents is a notifier that looks configured and never
 * fires.
 */
static void
test_an_unknown_event_is_refused(void)
{
    static const gchar *const wrong[] = { "question", "shouting", NULL };
    g_autoptr(GError) error = NULL;

    g_assert_cmpuint(clawt_notify_events_from_strv(wrong, &error), ==,
                     CLAWT_NOTIFY_EVENTS_NONE);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "shouting"));
    g_assert_nonnull(strstr(error->message, "question"));
}

/*
 * These are flags, not an enum, and g_enum_get_value_by_nick() asserts
 * on a flags type rather than returning nothing.  That is how every
 * notifier came to be unable to parse its own event list while the test
 * button worked perfectly -- so the parser is exercised here directly.
 */
static void
test_the_flags_parser_is_the_flags_parser(void)
{
    guint value = 0;

    g_assert_true(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS,
                                        "question", &value));
    g_assert_cmpuint(value, ==, CLAWT_NOTIFY_EVENTS_QUESTION);

    /* Case-insensitively, so a config file is not a spelling test. */
    g_assert_true(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, "ERROR",
                                        &value));
    g_assert_cmpuint(value, ==, CLAWT_NOTIFY_EVENTS_ERROR);

    g_assert_false(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, "nope",
                                         &value));
    g_assert_false(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, NULL,
                                         &value));
}

/* ── Priorities ──────────────────────────────────────────────────── */

static void
test_priorities_map_onto_each_backend(void)
{
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("urgent"), ==, "urgent");
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("high"), ==, "high");
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("low"), ==, "low");

    /* Anything unrecognised is the middle, not a failure. */
    g_assert_cmpstr(clawt_notify_priority_for_ntfy(NULL), ==, "default");
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("shouty"), ==, "default");

    /*
     * gotify treats 8 and above as demanding attention -- that is what
     * turns off auto-dismiss on its Android client, so urgent has to
     * clear it or it means nothing.
     */
    g_assert_cmpint(clawt_notify_priority_for_gotify("urgent"), >=, 8);
    g_assert_cmpint(clawt_notify_priority_for_gotify("high"), <, 8);
    g_assert_cmpint(clawt_notify_priority_for_gotify("normal"), ==, 5);
    g_assert_cmpint(clawt_notify_priority_for_gotify("low"), <, 5);

    /* freedesktop urgency: 0 low, 1 normal, 2 critical. */
    g_assert_cmpuint(clawt_notify_priority_for_desktop("low"), ==, 0);
    g_assert_cmpuint(clawt_notify_priority_for_desktop("normal"), ==, 1);
    g_assert_cmpuint(clawt_notify_priority_for_desktop("urgent"), ==, 2);
}

/* ── The line a lock screen shows ────────────────────────────────── */

static void
test_a_summary_is_one_line(void)
{
    g_autofree gchar *wrapped =
        clawt_notify_summarize("First line.\n\nSecond   line.\n", 0);

    g_assert_cmpstr(wrapped, ==, "First line. Second line.");
}

/*
 * A model's answer often opens with a code fence, and the first hundred
 * characters of a shell script tell a person nothing about why they are
 * being interrupted.
 */
static void
test_code_fences_go_first(void)
{
    g_autofree gchar *summary = clawt_notify_summarize(
        "```bash\nrm -rf /very/important\n```\nShall I run this?", 0);

    g_assert_null(strstr(summary, "rm -rf"));
    g_assert_nonnull(strstr(summary, "Shall I run this?"));
}

static void
test_a_long_answer_is_cut_and_says_so(void)
{
    g_autofree gchar *long_text = g_strnfill(400, 'a');
    g_autofree gchar *summary = clawt_notify_summarize(long_text, 20);
    g_autofree gchar *short_summary =
        clawt_notify_summarize("Short.", 20);

    /* The ellipsis only appears when something was actually left out. */
    g_assert_nonnull(strstr(summary, "\342\200\246"));
    g_assert_null(strstr(short_summary, "\342\200\246"));

    {
        g_autofree gchar *nothing = clawt_notify_summarize(NULL, 0);

        g_assert_cmpstr(nothing, ==, "");
    }
}

/* ── The command backend ─────────────────────────────────────────── */

static void
test_a_program_with_no_placeholders_gets_the_text(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, "researcher", "Researcher",
        "is waiting", "on an answer");
    g_auto(GStrv) argv = NULL;

    /*
     * This is what makes `command: receipt-print` work with nothing else
     * written down, which is the whole reason the backend exists.
     */
    argv = clawt_notify_expand_argv("receipt-print", NULL, notification);

    g_assert_cmpstr(argv[0], ==, "receipt-print");
    g_assert_cmpstr(argv[1], ==, "is waiting");
    g_assert_cmpstr(argv[2], ==, "on an answer");
    g_assert_null(argv[3]);
}

static void
test_placeholders_are_substituted_where_they_appear(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_ERROR, "scribe", "Scribe", "broke", "badly");
    static const gchar *const args[] = {
        "--from", "{{agent}}", "--subject", "{{title}}", "{{body}}", NULL
    };
    g_auto(GStrv) argv = NULL;

    argv = clawt_notify_expand_argv("send", args, notification);

    g_assert_cmpstr(argv[0], ==, "send");
    g_assert_cmpstr(argv[1], ==, "--from");
    g_assert_cmpstr(argv[2], ==, "scribe");
    g_assert_cmpstr(argv[3], ==, "--subject");
    g_assert_cmpstr(argv[4], ==, "broke");
    g_assert_cmpstr(argv[5], ==, "badly");

    /* Nothing appended, because the placeholders said where it goes. */
    g_assert_null(argv[6]);
}

/*
 * A brace pair that is not one of ours is somebody's literal text, and
 * eating it would silently change the command they wrote.
 */
static void
test_an_unknown_placeholder_is_left_alone(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, "a", "A", "t", "b");
    static const gchar *const args[] = { "{{title}}", "{{elsewhere}}", NULL };
    g_auto(GStrv) argv = NULL;

    argv = clawt_notify_expand_argv("x", args, notification);

    g_assert_cmpstr(argv[1], ==, "t");
    g_assert_cmpstr(argv[2], ==, "{{elsewhere}}");
}

static void
test_an_empty_body_appends_nothing(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, "a", "A", "just a title", NULL);
    g_auto(GStrv) argv = NULL;

    argv = clawt_notify_expand_argv("say", NULL, notification);

    g_assert_cmpstr(argv[1], ==, "just a title");
    g_assert_null(argv[2]);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/notify/quiet-hours-parse", test_a_range_is_parsed);
    g_test_add_func("/notify/quiet-hours-nonsense",
                    test_nonsense_is_not_a_range);
    g_test_add_func("/notify/quiet-hours-wrapping",
                    test_a_range_that_wraps_midnight);
    g_test_add_func("/notify/quiet-hours-daytime",
                    test_a_range_that_does_not_wrap);
    g_test_add_func("/notify/events", test_events_are_read_from_names);
    g_test_add_func("/notify/events-unknown",
                    test_an_unknown_event_is_refused);
    g_test_add_func("/notify/flags-parser",
                    test_the_flags_parser_is_the_flags_parser);
    g_test_add_func("/notify/priorities",
                    test_priorities_map_onto_each_backend);
    g_test_add_func("/notify/summary", test_a_summary_is_one_line);
    g_test_add_func("/notify/summary-code", test_code_fences_go_first);
    g_test_add_func("/notify/summary-long",
                    test_a_long_answer_is_cut_and_says_so);
    g_test_add_func("/notify/command-plain",
                    test_a_program_with_no_placeholders_gets_the_text);
    g_test_add_func("/notify/command-placeholders",
                    test_placeholders_are_substituted_where_they_appear);
    g_test_add_func("/notify/command-unknown-placeholder",
                    test_an_unknown_placeholder_is_left_alone);
    g_test_add_func("/notify/command-empty-body",
                    test_an_empty_body_appends_nothing);

    return g_test_run();
}
