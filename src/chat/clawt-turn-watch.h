/*
 * clawt-turn-watch.h - A turn that stops making progress has to end
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two budgets in the fleet are about a turn that has stopped getting
 * anywhere, and they are the same arithmetic with one difference:
 *
 *   `agents.runtime.turn_timeout_seconds` watches **activity**.  A turn
 *   may legitimately run for an hour while events keep arriving; a turn
 *   that has emitted nothing at all for the budget is wedged.  Every
 *   sign of life pushes the deadline out again.
 *
 *   `rooms.turn_timeout_seconds` watches **work**.  Nothing pushes it
 *   out: a member holds the room's turn for that long and then yields,
 *   however chatty it has been.
 *
 * Both of them **hold** while the turn is parked on an open decision,
 * and resume with the remainder.  Waiting for a person is not a stall,
 * and stopping a turn under an unanswered question manufactures a
 * stranded decision that the daemon then has to repair.
 *
 * Two details in the hold are bugs if they are missed, and both have a
 * test:
 *
 *   - **The hold counter clamps at zero.**  A resolve can arrive for a
 *     card this turn never opened -- stale cleanup after an interrupt is
 *     the ordinary way -- and a counter that goes negative never gets
 *     back to running.
 *
 *   - **A hold that finds the budget already spent expires at once.**  A
 *     delayed main loop can deliver the card event after the deadline
 *     has passed, and a person's wait must not retroactively extend a
 *     budget that was already gone.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TURN_WATCH (clawt_turn_watch_get_type())

G_DECLARE_FINAL_TYPE(ClawtTurnWatch, clawt_turn_watch,
                     CLAWT, TURN_WATCH, GObject)

/**
 * ClawtTurnWatchClockFunc:
 * @user_data: what was passed to clawt_turn_watch_set_clock()
 *
 * Monotonic microseconds, the way g_get_monotonic_time() gives them.
 *
 * Returns: the current time
 */
typedef gint64 (*ClawtTurnWatchClockFunc)(gpointer user_data);

/**
 * clawt_turn_watch_new_activity:
 *
 * A watch whose deadline is pushed out by every sign of life, for
 * `agents.runtime.turn_timeout_seconds`.
 *
 * Returns: (transfer full): a new #ClawtTurnWatch
 */
ClawtTurnWatch *clawt_turn_watch_new_activity(void);

/**
 * clawt_turn_watch_new_work:
 *
 * A watch nothing but a hold moves, for `rooms.turn_timeout_seconds`.
 *
 * Two constructors rather than a mode argument, because a boolean at a
 * call site says nothing about which of the two behaviours was wanted.
 *
 * Returns: (transfer full): a new #ClawtTurnWatch
 */
ClawtTurnWatch *clawt_turn_watch_new_work(void);

/**
 * clawt_turn_watch_set_budget:
 * @self: a #ClawtTurnWatch
 * @seconds: the budget, or 0 to watch nothing
 *
 * Applies to turns begun after it, not to those already running: a
 * config reload must not shorten a turn that is already half-way through
 * one.
 */
void clawt_turn_watch_set_budget(ClawtTurnWatch *self, guint seconds);

/**
 * clawt_turn_watch_get_budget:
 * @self: a #ClawtTurnWatch
 *
 * Returns: the budget in seconds, 0 when the watch is off
 */
guint clawt_turn_watch_get_budget(ClawtTurnWatch *self);

/**
 * clawt_turn_watch_set_clock:
 * @self: a #ClawtTurnWatch
 * @clock: (nullable) (scope notified) (closure user_data): where the time comes
 *   from, or %NULL for g_get_monotonic_time()
 * @user_data: passed to @clock
 * @notify: (nullable): frees @user_data
 *
 * For tests. A budget measured in minutes cannot be reached by waiting,
 * and a test that sleeps for it is a test that hangs.
 */
void clawt_turn_watch_set_clock(ClawtTurnWatch          *self,
                                ClawtTurnWatchClockFunc  clock,
                                gpointer                 user_data,
                                GDestroyNotify           notify);

/**
 * clawt_turn_watch_begin:
 * @self: a #ClawtTurnWatch
 * @key: what is taking the turn -- an agent id, or a room id
 *
 * Starts the clock. Beginning a turn already being watched restarts it,
 * because a turn that begins twice is one turn as far as the budget goes.
 */
void clawt_turn_watch_begin(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_note_activity:
 * @self: a #ClawtTurnWatch
 * @key: whose turn produced something
 *
 * A sign of life. Pushes an activity watch's deadline out by the whole
 * budget; a work watch ignores it, which is the entire difference
 * between the two.
 */
void clawt_turn_watch_note_activity(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_end:
 * @self: a #ClawtTurnWatch
 * @key: whose turn has settled
 *
 * Stops watching. Safe on a key that was never begun.
 */
void clawt_turn_watch_end(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_is_watching:
 * @self: a #ClawtTurnWatch
 * @key: the turn to ask about
 *
 * Returns: %TRUE while there is a live budget for @key
 */
gboolean clawt_turn_watch_is_watching(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_hold:
 * @self: a #ClawtTurnWatch
 * @key: whose turn is now waiting on a person
 *
 * Parks the clock with whatever is left of the budget. Nested holds are
 * counted, so two open decisions need two releases.
 *
 * A hold taken when the budget is already spent does not park anything:
 * it latches the turn as expired, so the next
 * clawt_turn_watch_collect_expired() reports it.
 */
void clawt_turn_watch_hold(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_release:
 * @self: a #ClawtTurnWatch
 * @key: whose turn is running again
 *
 * Resumes with the remainder. Clamped at zero: a release for a hold that
 * was never taken does nothing at all, rather than crediting the turn
 * with a budget it did not earn.
 */
void clawt_turn_watch_release(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_get_holds:
 * @self: a #ClawtTurnWatch
 * @key: the turn to ask about
 *
 * Returns: how many holds are outstanding
 */
guint clawt_turn_watch_get_holds(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_remaining:
 * @self: a #ClawtTurnWatch
 * @key: the turn to ask about
 *
 * Returns: microseconds left, 0 once spent, or -1 when @key is not being
 *   watched at all -- which is a different answer from "no time left"
 */
gint64 clawt_turn_watch_remaining(ClawtTurnWatch *self, const gchar *key);

/**
 * clawt_turn_watch_collect_expired:
 * @self: a #ClawtTurnWatch
 *
 * Every turn whose budget has run out, removed from the watch as it is
 * reported. Removing is what makes the answer once-only: the caller is
 * about to interrupt the turn, and a second report while that is in
 * flight would interrupt it twice.
 *
 * Returns: (transfer full) (element-type utf8): the keys, possibly empty
 */
GPtrArray *clawt_turn_watch_collect_expired(ClawtTurnWatch *self);

/**
 * clawt_turn_watch_reset:
 * @self: a #ClawtTurnWatch
 *
 * Forgets every turn. For tests, and for a daemon reload.
 */
void clawt_turn_watch_reset(ClawtTurnWatch *self);

G_END_DECLS
