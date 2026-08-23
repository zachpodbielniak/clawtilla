/*
 * test-cron.c - When a routine is next due
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A scheduler tested by watching a clock is a scheduler tested once.
 * Everything here is a pure function of an expression and a time, so
 * every one of these asserts on a firing that would otherwise take a
 * week to observe -- including the two that are only wrong once a
 * month and once every four years.
 */

#include <clawtilla.h>

#include <string.h>

static GDateTime *
at(gint year, gint month, gint day, gint hour, gint minute)
{
    return g_date_time_new_local(year, month, day, hour, minute, 0);
}

static void
check_next(const gchar *expression, GDateTime *from, const gchar *expected)
{
    g_autoptr(ClawtCron) cron = NULL;
    g_autoptr(GDateTime) next = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *formatted = NULL;

    cron = clawt_cron_parse(expression, &error);
    g_assert_no_error(error);
    g_assert_nonnull(cron);

    next = clawt_cron_next(cron, from);

    if (expected == NULL) {
        g_assert_null(next);
        return;
    }

    g_assert_nonnull(next);
    formatted = g_date_time_format(next, "%Y-%m-%d %H:%M");
    g_assert_cmpstr(formatted, ==, expected);
}

static void
refuse(const gchar *expression)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_cron_parse(expression, &error));
    g_assert_nonnull(error);
    g_assert_cmpuint(strlen(error->message), >, 0);
}

/* ── Parsing ─────────────────────────────────────────────────────── */

static void
test_the_ordinary_forms(void)
{
    g_autoptr(GDateTime) monday = at(2026, 8, 24, 8, 30);   /* a Monday */

    check_next("0 9 * * *", monday, "2026-08-24 09:00");
    check_next("*/15 * * * *", monday, "2026-08-24 08:45");
    check_next("0 */6 * * *", monday, "2026-08-24 12:00");
    check_next("30 8 * * *", monday, "2026-08-25 08:30");   /* just missed */
    check_next("0 0 1 * *", monday, "2026-09-01 00:00");
    check_next("15,45 * * * *", monday, "2026-08-24 08:45");
}

static void
test_names_are_accepted(void)
{
    g_autoptr(GDateTime) monday = at(2026, 8, 24, 8, 30);

    /* Refusing `mon` after accepting `1` helps nobody. */
    check_next("0 9 * * mon", monday, "2026-08-24 09:00");
    check_next("0 9 * * FRI", monday, "2026-08-28 09:00");
    check_next("0 0 1 jan *", monday, "2027-01-01 00:00");
}

/*
 * Sunday is 0 and also 7, and both are in the wild.
 */
static void
test_sunday_has_two_numbers(void)
{
    g_autoptr(GDateTime) monday = at(2026, 8, 24, 8, 30);

    check_next("0 9 * * 0", monday, "2026-08-30 09:00");
    check_next("0 9 * * 7", monday, "2026-08-30 09:00");
}

static void
test_nonsense_is_refused(void)
{
    refuse(NULL);
    refuse("");
    refuse("0 9 * *");            /* four fields */
    refuse("0 9 * * * *");        /* six -- a different dialect */
    refuse("60 9 * * *");         /* no minute 60 */
    refuse("0 24 * * *");         /* no hour 24 */
    refuse("0 9 32 * *");
    refuse("0 9 * 13 *");
    refuse("0 9 * * 8");
    refuse("0 9 * * mo");         /* not a name */
    refuse("0 9 * * ,");
    refuse("*/0 * * * *");        /* a step of nothing */
}

/* ── The one everybody gets wrong ────────────────────────────────── */

/*
 * When both day fields are restricted the match is day-of-month OR
 * day-of-week.  `0 0 13 * 5` is the thirteenth *and* every Friday, not
 * Friday the thirteenth -- and getting it backwards is wrong in a way
 * nobody notices until the month a routine did not run.
 */
static void
test_both_day_fields_are_an_or(void)
{
    g_autoptr(GDateTime) start = at(2026, 11, 2, 0, 0);   /* a Monday */

    /* The next Friday comes before the next thirteenth. */
    check_next("0 0 13 * 5", start, "2026-11-06 00:00");

    /* With one of them unrestricted, the other decides. */
    check_next("0 0 13 * *", start, "2026-11-13 00:00");
    check_next("0 0 * * 5", start, "2026-11-06 00:00");
}

/*
 * A date that exists only every fourth year has to be found, which is
 * why the search runs for four of them rather than one.
 */
static void
test_the_twenty_ninth_of_february(void)
{
    g_autoptr(GDateTime) start = at(2026, 3, 1, 0, 0);

    check_next("0 0 29 2 *", start, "2028-02-29 00:00");
}

/*
 * A date that never exists is %NULL rather than a lie or a hang.
 */
static void
test_a_date_that_cannot_happen(void)
{
    g_autoptr(GDateTime) start = at(2026, 1, 1, 0, 0);

    check_next("0 0 30 2 *", start, NULL);
    check_next("0 0 31 4 *", start, NULL);
}

/*
 * A range that wraps means what it looks like.  Vixie cron refuses
 * these; refusing something whose meaning is obvious helps nobody.
 */
static void
test_a_wrapping_range(void)
{
    g_autoptr(GDateTime) evening = at(2026, 8, 24, 21, 30);
    g_autoptr(GDateTime) small_hours = at(2026, 8, 24, 1, 30);

    check_next("0 22-2 * * *", evening, "2026-08-24 22:00");
    check_next("0 22-2 * * *", small_hours, "2026-08-24 02:00");
}

/*
 * Strictly after, and truncated to the minute.  Without both, a routine
 * that fired at 09:00:30 is handed 09:00 again and runs twice.
 */
static void
test_the_next_one_is_never_this_one(void)
{
    g_autoptr(ClawtCron) cron = NULL;
    g_autoptr(GDateTime) exactly = at(2026, 8, 24, 9, 0);
    g_autoptr(GDateTime) next = NULL;
    g_autofree gchar *formatted = NULL;

    cron = clawt_cron_parse("0 9 * * *", NULL);
    next = clawt_cron_next(cron, exactly);

    g_assert_nonnull(next);
    formatted = g_date_time_format(next, "%Y-%m-%d %H:%M");
    g_assert_cmpstr(formatted, ==, "2026-08-25 09:00");
}

static void
test_matches_answers_for_one_minute(void)
{
    g_autoptr(ClawtCron) cron = clawt_cron_parse("30 9 * * mon", NULL);
    g_autoptr(GDateTime) monday = at(2026, 8, 24, 9, 30);
    g_autoptr(GDateTime) minute_later = at(2026, 8, 24, 9, 31);
    g_autoptr(GDateTime) tuesday = at(2026, 8, 25, 9, 30);

    g_assert_true(clawt_cron_matches(cron, monday));
    g_assert_false(clawt_cron_matches(cron, minute_later));
    g_assert_false(clawt_cron_matches(cron, tuesday));
}

/* ── Presets ─────────────────────────────────────────────────────── */

static void
check_preset(const gchar *preset, const gchar *when, const gchar *weekday,
             const gchar *expected)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *expression = NULL;

    expression = clawt_cron_from_preset(preset, when, weekday, NULL, &error);
    g_assert_no_error(error);

    if (expected == NULL) {
        g_assert_null(expression);
        return;
    }

    g_assert_cmpstr(expression, ==, expected);
}

/*
 * The presets are sugar over cron rather than a second scheduler, so
 * there is one implementation of "when next" and it is the one every
 * test above exercises.
 */
static void
test_presets_are_cron(void)
{
    check_preset("hourly", NULL, NULL, "0 * * * *");
    check_preset("hourly", "00:20", NULL, "20 * * * *");
    check_preset("daily", "09:00", NULL, "0 9 * * *");
    check_preset("daily", "17:45", NULL, "45 17 * * *");
    check_preset("weekdays", "09:00", NULL, "0 9 * * 1-5");
    check_preset("weekly", "09:00", "monday", "0 9 * * 1");
    check_preset("weekly", "09:00", "sun", "0 9 * * 0");

    /* Manual has no next time, which is an answer rather than a failure. */
    check_preset("manual", NULL, NULL, NULL);
}

static void
test_a_custom_schedule_is_checked_when_it_is_typed(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *good = NULL;

    good = clawt_cron_from_preset("custom", NULL, NULL, "0 9 * * 1-5",
                                  &error);
    g_assert_no_error(error);
    g_assert_cmpstr(good, ==, "0 9 * * 1-5");

    /* Refused while somebody is still looking at what they typed. */
    g_assert_null(clawt_cron_from_preset("custom", NULL, NULL, "0 9 * *",
                                         &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    g_assert_null(clawt_cron_from_preset("custom", NULL, NULL, NULL, &error));
    g_assert_nonnull(error);
}

static void
test_a_bad_preset_says_what_the_choices_are(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_cron_from_preset("fortnightly", NULL, NULL, NULL,
                                         &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "weekdays"));
    g_clear_error(&error);

    g_assert_null(clawt_cron_from_preset("daily", "9 o'clock", NULL, NULL,
                                         &error));
    g_assert_nonnull(error);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/cron/ordinary", test_the_ordinary_forms);
    g_test_add_func("/cron/names", test_names_are_accepted);
    g_test_add_func("/cron/sunday", test_sunday_has_two_numbers);
    g_test_add_func("/cron/nonsense", test_nonsense_is_refused);
    g_test_add_func("/cron/both-day-fields", test_both_day_fields_are_an_or);
    g_test_add_func("/cron/leap-day", test_the_twenty_ninth_of_february);
    g_test_add_func("/cron/impossible", test_a_date_that_cannot_happen);
    g_test_add_func("/cron/wrapping-range", test_a_wrapping_range);
    g_test_add_func("/cron/strictly-after",
                    test_the_next_one_is_never_this_one);
    g_test_add_func("/cron/matches", test_matches_answers_for_one_minute);
    g_test_add_func("/cron/presets", test_presets_are_cron);
    g_test_add_func("/cron/custom",
                    test_a_custom_schedule_is_checked_when_it_is_typed);
    g_test_add_func("/cron/bad-preset",
                    test_a_bad_preset_says_what_the_choices_are);

    return g_test_run();
}
