/*
 * clawt-cron.h - When a routine is next due
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The presets are sugar over cron rather than a second scheduler beside
 * it: "daily at 09:00" *is* `0 9 * * *`, and keeping them as separate
 * kinds of thing would mean two implementations of "when next", one of
 * which is exercised far less and is therefore the one that is wrong.
 *
 * Everything here is a pure function of a specification and a time, so
 * "what does this fire at" can be asserted on without waiting for it.
 * A scheduler tested by watching a clock is a scheduler tested once.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * ClawtCron:
 *
 * A parsed five-field cron expression.
 */
typedef struct _ClawtCron ClawtCron;

#define CLAWT_TYPE_CRON (clawt_cron_get_type())

GType clawt_cron_get_type(void) G_GNUC_CONST;

/**
 * clawt_cron_parse:
 * @expression: five fields: minute hour day-of-month month day-of-week
 * @error: (out) (optional): return location for a #GError
 *
 * Understands `*`, a number, `a-b`, `a-b/step`, `*&#47;step`, and
 * comma-separated lists of any of those.  Month and weekday also take
 * three-letter names.  Sunday is both 0 and 7, as everywhere else.
 *
 * Seconds are deliberately not a field.  A six-field expression is a
 * different dialect, and accepting both means a five-field one is
 * ambiguous -- `0 9 * * *` would be either nine in the morning or every
 * ninth minute depending on which we guessed.
 *
 * Returns: (transfer full) (nullable): the schedule, or %NULL
 */
ClawtCron *clawt_cron_parse(const gchar *expression, GError **error);

ClawtCron *clawt_cron_copy(ClawtCron *self);
void       clawt_cron_free(ClawtCron *self);

/**
 * clawt_cron_next:
 * @self: a #ClawtCron
 * @after: the time to search from, exclusive
 *
 * The first minute strictly after @after that this fires on.
 *
 * Local time, because a person who wrote 09:00 means nine o'clock where
 * they are.  That has the usual consequences twice a year: an hour that
 * does not exist is skipped, and one that happens twice fires twice.
 * Both are what every other cron does, and the alternative -- silently
 * running everything in UTC -- is a schedule that is an hour wrong for
 * half the year.
 *
 * Returns: (transfer full) (nullable): the next time, or %NULL if it
 *   cannot fire within four years (a date such as 30 February)
 */
GDateTime *clawt_cron_next(ClawtCron *self, GDateTime *after);

/**
 * clawt_cron_matches:
 * @self: a #ClawtCron
 * @when: a time
 *
 * Whether this fires on @when's minute.
 *
 * Returns: %TRUE if it does
 */
gboolean clawt_cron_matches(ClawtCron *self, GDateTime *when);

/**
 * clawt_cron_describe:
 * @self: a #ClawtCron
 *
 * The expression as it was written.
 *
 * Returns: (transfer none): the expression
 */
const gchar *clawt_cron_describe(ClawtCron *self);

/**
 * clawt_cron_from_preset:
 * @preset: manual, hourly, daily, weekdays, weekly or custom
 * @at: (nullable): a time of day such as `09:00`
 * @weekday: (nullable): for `weekly`, a day such as `monday` or `mon`
 * @custom: (nullable): for `custom`, the expression itself
 * @error: (out) (optional): return location for a #GError
 *
 * Turns a preset into the expression it means.
 *
 * `manual` returns %NULL with no error set: a routine that only runs
 * when asked has no next time, which is a perfectly good answer and not
 * a failure to compute one.
 *
 * Returns: (transfer full) (nullable): the expression
 */
gchar *clawt_cron_from_preset(const gchar  *preset,
                              const gchar  *at,
                              const gchar  *weekday,
                              const gchar  *custom,
                              GError      **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtCron, clawt_cron_free)

G_END_DECLS
