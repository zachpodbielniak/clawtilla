/*
 * clawt-cron.c - When a routine is next due
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-cron.h"

#include <string.h>

/*
 * How far ahead to look before giving up.
 *
 * Four years rather than one, because 29 February exists: `0 0 29 2 *`
 * is a legitimate schedule that fires once every four years, and a
 * one-year search would report it as impossible.
 */
#define SEARCH_DAYS (366 * 4)

struct _ClawtCron {
    gchar   *expression;

    guint64  minutes;    /* 0-59 */
    guint32  hours;      /* 0-23 */
    guint32  days;       /* 1-31 */
    guint16  months;     /* 1-12 */
    guint8   weekdays;   /* 0-6, Sunday first */

    /*
     * Whether each of the two day fields was restricted.
     *
     * Cron's oldest oddity: when *both* are restricted the match is
     * day-of-month OR day-of-week, not AND.  `0 0 13 * 5` is the
     * thirteenth and every Friday, not Friday the thirteenth -- and a
     * scheduler that gets this backwards is wrong in a way nobody
     * notices until the month a routine did not run.
     */
    gboolean day_restricted;
    gboolean weekday_restricted;
};

ClawtCron *
clawt_cron_copy(ClawtCron *self)
{
    ClawtCron *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtCron, 1);
    *copy = *self;
    copy->expression = g_strdup(self->expression);

    return copy;
}

void
clawt_cron_free(ClawtCron *self)
{
    if (self == NULL)
        return;

    g_free(self->expression);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtCron, clawt_cron, clawt_cron_copy, clawt_cron_free)

const gchar *
clawt_cron_describe(ClawtCron *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->expression;
}

/* ── Parsing ─────────────────────────────────────────────────────── */

static const gchar *const month_names[] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec"
};

static const gchar *const weekday_names[] = {
    "sun", "mon", "tue", "wed", "thu", "fri", "sat"
};

/*
 * A number, or the three-letter name of a month or a weekday.
 *
 * Names are accepted because people write them, and refusing `mon`
 * after accepting `1` is the kind of pedantry that sends somebody to
 * the documentation for something they already got right.
 */
static gboolean
parse_number(const gchar *text, gint min, gint max, gboolean names_are_months,
             gboolean names_are_weekdays, gint *out_value)
{
    gchar *end = NULL;
    gint64 value;
    gsize i;

    if (text == NULL || *text == '\0')
        return FALSE;

    if (names_are_months || names_are_weekdays) {
        const gchar *const *names = names_are_months ? month_names
                                                     : weekday_names;
        gsize count = names_are_months ? G_N_ELEMENTS(month_names)
                                       : G_N_ELEMENTS(weekday_names);

        for (i = 0; i < count; i++) {
            if (g_ascii_strncasecmp(text, names[i], 3) != 0)
                continue;

            if (strlen(text) != 3)
                continue;

            *out_value = names_are_months ? (gint)i + 1 : (gint)i;
            return TRUE;
        }
    }

    value = g_ascii_strtoll(text, &end, 10);

    if (end == text || *end != '\0')
        return FALSE;

    /*
     * Sunday is 0 and also 7.  Both spellings are in the wild and both
     * mean the same day.
     */
    if (names_are_weekdays && value == 7 && max == 6)
        value = 0;

    if (value < min || value > max)
        return FALSE;

    *out_value = (gint)value;

    return TRUE;
}

/*
 * One field, into a bitmask.
 *
 * @out_restricted tracks a leading wildcard, including wildcard steps.
 * The day-field combination rule depends on syntax, not mask coverage:
 * A wildcard with step one is unrestricted; an explicit full range is not.
 */
static gboolean
parse_field(const gchar *text, gint min, gint max, gboolean months,
            gboolean weekdays, guint64 *out_mask, gboolean *out_restricted,
            GError **error)
{
    g_auto(GStrv) items = NULL;
    guint64 mask = 0;
    guint i;

    if (out_restricted != NULL)
        *out_restricted = text[0] != '*';

    items = g_strsplit(text, ",", -1);

    for (i = 0; items[i] != NULL; i++) {
        g_auto(GStrv) step_parts = NULL;
        g_auto(GStrv) range_parts = NULL;
        const gchar *range;
        gint step = 1;
        gint first;
        gint last;
        gint value;

        g_strstrip(items[i]);

        if (*items[i] == '\0') {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' has an empty entry in it", text);
            return FALSE;
        }

        step_parts = g_strsplit(items[i], "/", 2);
        range = step_parts[0];

        if (step_parts[1] != NULL &&
            (!parse_number(step_parts[1], 1, max > 0 ? max : 1, FALSE, FALSE,
                           &step) || step < 1)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not a step", step_parts[1]);
            return FALSE;
        }

        if (g_strcmp0(range, "*") == 0) {
            first = min;
            last = max;
        } else {
            range_parts = g_strsplit(range, "-", 2);

            if (!parse_number(range_parts[0], min, weekdays ? 7 : max, months, weekdays,
                              &first)) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "'%s' is not a value between %d and %d",
                            range_parts[0], min, max);
                return FALSE;
            }

            if (range_parts[1] == NULL) {
                last = first;
            } else if (!parse_number(range_parts[1], min, weekdays ? 7 : max, months,
                                     weekdays, &last)) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "'%s' is not a value between %d and %d",
                            range_parts[1], min, max);
                return FALSE;
            }

            /*
             * A range that wraps -- `fri-mon`, `22-2` -- is accepted and
             * means what it looks like.  Vixie cron rejects it; refusing
             * something whose meaning is obvious helps nobody.
             */
            if (last < first) {
                for (value = first; value <= max; value += step)
                    mask |= G_GUINT64_CONSTANT(1) << value;

                for (value = min; value <= last; value += step)
                    mask |= G_GUINT64_CONSTANT(1) << value;

                continue;
            }
        }

        for (value = first; value <= last; value += step)
            mask |= G_GUINT64_CONSTANT(1) << value;
    }

    if (mask == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' matches nothing", text);
        return FALSE;
    }

	/* Keep endpoint 7 until ranges and steps are expanded, then alias Sunday. */
	if (weekdays && (mask & (G_GUINT64_CONSTANT(1) << 7)) != 0) {
		mask |= 1;
		mask &= ~(G_GUINT64_CONSTANT(1) << 7);
	}

    *out_mask = mask;

    return TRUE;
}

ClawtCron *
clawt_cron_parse(const gchar *expression, GError **error)
{
    g_autofree ClawtCron *self = NULL;
    g_auto(GStrv) fields = NULL;
    guint64 mask = 0;

    if (expression == NULL || *expression == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a cron expression is five fields: minute hour "
                            "day-of-month month day-of-week");
        return NULL;
    }

    fields = g_strsplit_set(expression, " \t", -1);

    /* Collapse the empty strings a run of spaces produces. */
    {
        guint read;
        guint write = 0;

        for (read = 0; fields[read] != NULL; read++) {
            if (*fields[read] == '\0') {
                g_free(fields[read]);
                continue;
            }

            fields[write++] = fields[read];
        }

        fields[write] = NULL;

        if (write != 5) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' has %u fields; a cron expression has five: "
                        "minute hour day-of-month month day-of-week",
                        expression, write);
            return NULL;
        }
    }

    self = g_new0(ClawtCron, 1);

    if (!parse_field(fields[0], 0, 59, FALSE, FALSE, &mask, NULL, error))
        return NULL;

    self->minutes = mask;

    if (!parse_field(fields[1], 0, 23, FALSE, FALSE, &mask, NULL, error))
        return NULL;

    self->hours = (guint32)mask;

    if (!parse_field(fields[2], 1, 31, FALSE, FALSE, &mask,
                     &self->day_restricted, error))
        return NULL;

    self->days = (guint32)mask;

    if (!parse_field(fields[3], 1, 12, TRUE, FALSE, &mask, NULL, error))
        return NULL;

    self->months = (guint16)mask;

    if (!parse_field(fields[4], 0, 6, FALSE, TRUE, &mask,
                     &self->weekday_restricted, error))
        return NULL;

    self->weekdays = (guint8)mask;
    self->expression = g_strdup(expression);

    return g_steal_pointer(&self);
}

/* ── When ────────────────────────────────────────────────────────── */

static gboolean
day_matches(ClawtCron *self, GDateTime *day)
{
    gint day_of_month = g_date_time_get_day_of_month(day);
    /* GLib counts Monday as 1 and Sunday as 7; cron puts Sunday at 0. */
    gint weekday = g_date_time_get_day_of_week(day) % 7;
    gboolean by_day = (self->days & (1u << day_of_month)) != 0;
    gboolean by_weekday = (self->weekdays & (1u << weekday)) != 0;

    if ((self->months & (1u << g_date_time_get_month(day))) == 0)
        return FALSE;

    /*
     * The OR, and only when both were restricted.  With one of them at
     * `*` the other decides, which is the ordinary case and the one that
     * would otherwise never fire.
     */
    if (self->day_restricted && self->weekday_restricted)
        return by_day || by_weekday;

    return by_day && by_weekday;
}

gboolean
clawt_cron_matches(ClawtCron *self, GDateTime *when)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(when != NULL, FALSE);

    return day_matches(self, when) &&
           (self->hours & (1u << g_date_time_get_hour(when))) != 0 &&
           (self->minutes &
            (G_GUINT64_CONSTANT(1) << g_date_time_get_minute(when))) != 0;
}

GDateTime *
clawt_cron_next(ClawtCron *self, GDateTime *after)
{
	g_autoptr(GTimeZone) zone = NULL;
	g_autoptr(GDateTime) local_after = NULL;
	g_autoptr(GDateTime) day = NULL;
	gint64 after_seconds;
	gint searched;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(after != NULL, NULL);

	/* Use UTC only as a calendar counter; actual firings use local time. */
	zone = g_time_zone_new_local();
	local_after = g_date_time_to_timezone(after, zone);
	after_seconds = g_date_time_to_unix(after);
	day = g_date_time_new_utc(g_date_time_get_year(local_after),
		g_date_time_get_month(local_after),
		g_date_time_get_day_of_month(local_after), 0, 0, 0);

	for (searched = 0; searched < SEARCH_DAYS && day != NULL; searched++) {
		g_autoptr(GDateTime) tomorrow = NULL;

		/* Skip whole calendar dates, keeping leap-day searches inexpensive. */
		if (day_matches(self, day)) {
			gint64 midnight = g_date_time_to_unix(day);
			gint64 best = G_MAXINT64;
			gint first_interval;
			gint last_interval;
			gint interval;

			/*
			 * UTC offsets are less than a day. Enumerate every timezone
			 * interval that could contain this local date, including both
			 * sides of a fold. Unlike constructing a local GDateTime, this
			 * neither normalizes a gap nor silently chooses one occurrence.
			 */
			first_interval = g_time_zone_find_interval(zone,
				G_TIME_TYPE_UNIVERSAL, midnight - 86400);
			last_interval = g_time_zone_find_interval(zone,
				G_TIME_TYPE_UNIVERSAL, midnight + 2 * 86400);

			for (interval = first_interval; interval <= last_interval;
				 interval++) {
				gint offset = g_time_zone_get_offset(zone, interval);
				gint hour;

				for (hour = 0; hour < 24; hour++) {
					gint minute;

					if ((self->hours & (1u << hour)) == 0)
						continue;

					for (minute = 0; minute < 60; minute++) {
						gint64 candidate;

						if ((self->minutes &
							 (G_GUINT64_CONSTANT(1) << minute)) == 0)
							continue;

						candidate = midnight + hour * 3600 + minute * 60 - offset;
						/* Interval membership rejects nonexistent wall times. */
						if (candidate > after_seconds && candidate < best &&
							g_time_zone_find_interval(zone, G_TIME_TYPE_UNIVERSAL,
								candidate) == interval)
							best = candidate;
					}
				}
			}

			if (best != G_MAXINT64) {
				g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(best);

				return utc != NULL ? g_date_time_to_timezone(utc, zone) : NULL;
			}
		}

		tomorrow = g_date_time_add_days(day, 1);
		g_clear_pointer(&day, g_date_time_unref);
		day = g_steal_pointer(&tomorrow);
	}

	return NULL;
}

/* ── Presets ─────────────────────────────────────────────────────── */

static gboolean
parse_at(const gchar *at, gint *out_hour, gint *out_minute, GError **error)
{
    gchar *end = NULL;
    gint64 hour;
    gint64 minute;

    /* An unset time means the top of the hour, which is what people mean. */
    if (at == NULL || *at == '\0') {
        *out_hour = 0;
        *out_minute = 0;
        return TRUE;
    }

    hour = g_ascii_strtoll(at, &end, 10);

    if (end == at || *end != ':' || hour < 0 || hour > 23) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a time of day such as 09:00", at);
        return FALSE;
    }

    at = end + 1;
    minute = g_ascii_strtoll(at, &end, 10);

    if (end == at || minute < 0 || minute > 59) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a time of day such as 09:00", at);
        return FALSE;
    }

    *out_hour = (gint)hour;
    *out_minute = (gint)minute;

    return TRUE;
}

gchar *
clawt_cron_from_preset(const gchar  *preset,
                       const gchar  *at,
                       const gchar  *weekday,
                       const gchar  *custom,
                       GError      **error)
{
    gint hour = 0;
    gint minute = 0;

    if (preset == NULL || g_strcmp0(preset, "manual") == 0)
        return NULL;

    if (g_strcmp0(preset, "custom") == 0) {
        g_autoptr(ClawtCron) parsed = NULL;

        if (custom == NULL || *custom == '\0') {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "a custom schedule needs a cron expression");
            return NULL;
        }

        /* Refused here rather than at the next tick, while somebody is
         * still looking at what they typed. */
        parsed = clawt_cron_parse(custom, error);

        return parsed != NULL ? g_strdup(custom) : NULL;
    }

    if (g_strcmp0(preset, "hourly") == 0) {
        if (!parse_at(at, &hour, &minute, error))
            return NULL;

        /* The hour is ignored; the minute within it is not. */
        return g_strdup_printf("%d * * * *", minute);
    }

    if (!parse_at(at, &hour, &minute, error))
        return NULL;

    if (g_strcmp0(preset, "daily") == 0)
        return g_strdup_printf("%d %d * * *", minute, hour);

    if (g_strcmp0(preset, "weekdays") == 0)
        return g_strdup_printf("%d %d * * 1-5", minute, hour);

    if (g_strcmp0(preset, "weekly") == 0) {
        gint day = 1;

        if (weekday != NULL && *weekday != '\0' &&
            !parse_number(weekday, 0, 6, FALSE, TRUE, &day)) {
            /* Full names too, since a dialog offers them. */
            static const gchar *const full[] = {
                "sunday", "monday", "tuesday", "wednesday", "thursday",
                "friday", "saturday"
            };
            gsize i;
            gboolean found = FALSE;

            for (i = 0; i < G_N_ELEMENTS(full); i++) {
                if (g_ascii_strcasecmp(weekday, full[i]) != 0)
                    continue;

                day = (gint)i;
                found = TRUE;
                break;
            }

            if (!found) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "'%s' is not a day of the week", weekday);
                return NULL;
            }
        }

        return g_strdup_printf("%d %d * * %d", minute, hour, day);
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                "'%s' is not a schedule: use manual, hourly, daily, "
                "weekdays, weekly or custom", preset);

    return NULL;
}
