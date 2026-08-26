/*
 * clawt-chat-run.c - Grouping consecutive messages into runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "chat/clawt-chat-run.h"

gboolean
clawt_chat_run_is_start(const gchar *previous_sender,
                        const gchar *previous_day,
                        const gchar *sender,
                        const gchar *day,
                        gboolean    *out_new_day)
{
    gboolean new_day = (g_strcmp0(previous_day, day) != 0);

    if (out_new_day != NULL)
        *out_new_day = new_day;

    /*
     * The first message of a transcript is a run start, and it is a new
     * day: previous_day is NULL there, which compares unequal to any
     * real date.  That is what puts a divider at the top of a loaded
     * history rather than leaving the first block undated.
     */
    return new_day || g_strcmp0(previous_sender, sender) != 0;
}

gchar *
clawt_chat_day_label(GDateTime *when, GDateTime *now)
{
    g_autoptr(GDateTime) today = (now != NULL) ? g_date_time_ref(now)
                                               : g_date_time_new_now_local();
    g_autoptr(GDateTime) yesterday = g_date_time_add_days(today, -1);
    gchar *text;

    g_return_val_if_fail(when != NULL, g_strdup(""));

    if (g_date_time_get_year(when) == g_date_time_get_year(today) &&
        g_date_time_get_day_of_year(when) ==
        g_date_time_get_day_of_year(today))
        return g_strdup("Today");

    if (g_date_time_get_year(when) == g_date_time_get_year(yesterday) &&
        g_date_time_get_day_of_year(when) ==
        g_date_time_get_day_of_year(yesterday))
        return g_strdup("Yesterday");

    /*
     * "%-d", not "%e".  GLib pads %e with U+2007 FIGURE SPACE rather
     * than an ordinary one -- so "%A %e %B" renders "Wednesday" then two
     * spaces then "5", and g_strstrip() cannot help because the padding
     * is in the middle of the string.  It reads as a rendering fault on
     * the nine days a month it happens.
     */
    text = g_date_time_format(when, "%A %-d %B");

    return text;
}

gchar *
clawt_chat_time_label(GDateTime *when)
{
    if (when == NULL)
        return NULL;

    return g_date_time_format(when, "%H:%M");
}
