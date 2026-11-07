/*
 * clawt-activity.c - What is running across the fleet, as both clients draw it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The rules the Activity page applies in both clients, here so they are
 * tested without a window or a browser. The listing itself is the
 * daemon's (activity.list); this is only what a client says about it.
 */

#include "clawtilla.h"

gchar *
clawt_activity_elapsed_label(gint64 started_us, gint64 now_us)
{
    gint64 seconds;

    if (started_us <= 0 || now_us <= started_us)
        return g_strdup("just now");

    seconds = (now_us - started_us) / G_USEC_PER_SEC;

    if (seconds < 60)
        return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);
    if (seconds < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m %02" G_GINT64_FORMAT "s",
                               seconds / 60, seconds % 60);

    return g_strdup_printf("%" G_GINT64_FORMAT "h %02" G_GINT64_FORMAT "m",
                           seconds / 3600, (seconds % 3600) / 60);
}

gchar *
clawt_activity_stop_prompt(const gchar *agent_id, guint running)
{
    if (running <= 1)
        return g_strdup_printf("Stop %s? This interrupts the agent, not only "
                               "this turn.", agent_id != NULL ? agent_id : "this agent");

    return g_strdup_printf("Stop %s? It has %u turns running, and this "
                           "interrupts all of them.",
                           agent_id != NULL ? agent_id : "this agent", running);
}
