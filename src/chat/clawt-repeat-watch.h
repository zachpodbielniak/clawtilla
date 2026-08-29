/*
 * clawt-repeat-watch.h - Noticing that an agent keeps making the same call
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * #ClawtLoopGuard watches messages between agents, which clawtilla owns
 * end to end and can therefore stop.  This watches the tool calls inside
 * one turn, which clawtilla does **not** own: the loop that decides to
 * call a tool again lives inside the model's CLI, and nothing on this
 * side of the socket can steer it mid-turn.
 *
 * So this half observes.  It counts the calls the daemon serves, says so
 * in the thread, and at the last threshold escalates to the interrupt
 * that already exists.  Saying that plainly matters: a check that implies
 * more reach than it has sends the next reader to the wrong layer.
 *
 * Three decisions the counting rests on, each of which was a way of
 * being wrong:
 *
 *   - **A bare tool name is not a call worth counting.**  Five `bash`
 *     calls may be five different commands, and reporting them as a loop
 *     is a false positive that teaches people to ignore the real one.
 *     clawt_repeat_key() answers %NULL for a call with no arguments.
 *
 *   - **Counts are cumulative per (turn, key), never a sliding window.**
 *     A window says "five in the last minute", which a slow loop passes
 *     for ever.  The question is whether this turn has done the same
 *     thing over and over, and that is a total.
 *
 *   - **A threshold fires only when the count lands exactly on it.**
 *     Reporting every repeat past a floor turns one signal into a
 *     hundred, and a hundred is noise.  5, 10 and 20 report once each;
 *     6 and 21 report nothing.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_REPEAT_WATCH (clawt_repeat_watch_get_type())

G_DECLARE_FINAL_TYPE(ClawtRepeatWatch, clawt_repeat_watch,
                     CLAWT, REPEAT_WATCH, GObject)

/**
 * clawt_repeat_watch_new:
 *
 * Created with the schema's own defaults -- thresholds at 5, 10 and 20,
 * and 256 keys remembered per turn.
 *
 * Returns: (transfer full): a new #ClawtRepeatWatch
 */
ClawtRepeatWatch *clawt_repeat_watch_new(void);

/**
 * clawt_repeat_watch_set_thresholds:
 * @self: a #ClawtRepeatWatch
 * @csv: (nullable): comma-separated counts, as `orchestration.repeat_thresholds`
 *   spells them, or %NULL for none
 *
 * Anything that is not a positive whole number is dropped with a warning
 * rather than taken as zero, because a threshold of zero would fire on
 * every call. The list is sorted, so the highest is the escalation point
 * whatever order somebody wrote them in.
 */
void clawt_repeat_watch_set_thresholds(ClawtRepeatWatch *self,
                                       const gchar      *csv);

/**
 * clawt_repeat_watch_set_max_keys:
 * @self: a #ClawtRepeatWatch
 * @max_keys: how many distinct calls one turn remembers, or 0 for the default
 *
 * The table is an LRU: past this, the least recently seen call is
 * dropped and starts counting again from one. Unbounded, one turn making
 * a million distinct calls would grow the daemon for as long as it ran.
 */
void clawt_repeat_watch_set_max_keys(ClawtRepeatWatch *self, guint max_keys);

/**
 * clawt_repeat_watch_get_highest_threshold:
 * @self: a #ClawtRepeatWatch
 *
 * Returns: the largest configured threshold, or 0 when none is set
 */
guint clawt_repeat_watch_get_highest_threshold(ClawtRepeatWatch *self);

/**
 * clawt_repeat_key:
 * @tool: (nullable): the tool being called
 * @args: (nullable): its arguments, as text
 *
 * The one spelling of what makes two calls the same call: the tool name,
 * a colon, and the arguments with every run of whitespace collapsed to a
 * single space. Collapsing is what makes a re-serialised JSON object with
 * different spacing count as the call it is.
 *
 * Returns: (transfer full) (nullable): the key, or %NULL when this is not
 *   a call worth counting -- no tool, or no arguments at all
 */
gchar *clawt_repeat_key(const gchar *tool, const gchar *args);

/**
 * clawt_repeat_watch_note:
 * @self: a #ClawtRepeatWatch
 * @turn_id: which turn this call belongs to, usually an agent id
 * @tool: the tool being called
 * @args: (nullable): its arguments, as text
 *
 * Records one call and says whether it just landed on a threshold.
 *
 * Returns: the count, when it landed exactly on a configured threshold;
 *   0 otherwise, including for a call with no arguments
 */
guint clawt_repeat_watch_note(ClawtRepeatWatch *self,
                              const gchar      *turn_id,
                              const gchar      *tool,
                              const gchar      *args);

/**
 * clawt_repeat_watch_count:
 * @self: a #ClawtRepeatWatch
 * @turn_id: which turn to ask about
 * @tool: the tool
 * @args: (nullable): its arguments
 *
 * Returns: how many times this turn has made this exact call
 */
guint clawt_repeat_watch_count(ClawtRepeatWatch *self,
                               const gchar      *turn_id,
                               const gchar      *tool,
                               const gchar      *args);

/**
 * clawt_repeat_watch_end_turn:
 * @self: a #ClawtRepeatWatch
 * @turn_id: the turn that has settled
 *
 * Drops everything counted for @turn_id. Counters are per turn: the same
 * call made once in each of twenty turns is twenty pieces of work, not a
 * loop.
 */
void clawt_repeat_watch_end_turn(ClawtRepeatWatch *self,
                                 const gchar      *turn_id);

/**
 * clawt_repeat_watch_reset:
 * @self: a #ClawtRepeatWatch
 *
 * Forgets every turn. For tests, and for a daemon reload.
 */
void clawt_repeat_watch_reset(ClawtRepeatWatch *self);

G_END_DECLS
