/*
 * clawt-activity.h - What is running across the fleet, as both clients draw it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_ACTIVITY_STEPS:
 *
 * How many of a running turn's most recent steps `activity.list` carries:
 * enough to see what it is doing, few enough that a fleet of busy agents
 * is one screen.
 */
#define CLAWT_ACTIVITY_STEPS (6)

/**
 * clawt_activity_elapsed_label:
 * @started_us: when the turn began, wall-clock microseconds
 * @now_us: now, in the same clock
 *
 * How long a turn has been running, compact enough for a row: `45s`,
 * `3m 12s`, `1h 04m`. A turn with no start, or one stamped in the
 * future by a skewed clock, is `just now` rather than a negative age.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_activity_elapsed_label(gint64 started_us,
                                    gint64 now_us);

/**
 * clawt_activity_stop_prompt:
 * @agent_id: the agent
 * @running: how many turns it has running
 *
 * The confirmation for Stop, which interrupts the whole agent and not
 * the row it was pressed on: an agent in three rooms has three turns and
 * agent.interrupt ends all of them. Said in both clients from here,
 * because a button that does more than its row suggests must say so.
 *
 * Returns: (transfer full): the question
 */
gchar *clawt_activity_stop_prompt(const gchar *agent_id,
                                  guint        running);

G_END_DECLS
