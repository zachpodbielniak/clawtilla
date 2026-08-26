/*
 * clawt-loop-guard.h - What stops agents talking to each other for ever
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Agent-to-agent messaging is the point of clawtilla and also the thing
 * most able to run away.  Two agents that each reply politely will do so
 * until something stops them, and the cost is real money.
 *
 * Four limits, because each catches a case the others do not:
 *
 *   hops    catches a chain that grows: A asks B asks C asks D...
 *   rate    catches one agent flooding, however shallow each message
 *   budget  catches an expensive loop that is short enough to pass the rest
 *   cycles  catches two agents alternating the same two replies, where
 *           every message is a fresh chain with a depth of one
 *
 * Applied before a message is enqueued rather than on delivery, so a
 * runaway fan-out is stopped at the source rather than after the queue has
 * already grown.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "chat/clawt-message.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_LOOP_GUARD (clawt_loop_guard_get_type())

G_DECLARE_FINAL_TYPE(ClawtLoopGuard, clawt_loop_guard,
                     CLAWT, LOOP_GUARD, GObject)

/**
 * clawt_loop_guard_new:
 *
 * Created with no limits.  Call clawt_loop_guard_set_limits() and
 * clawt_loop_guard_set_task_budget() before it guards anything -- a
 * guard with every limit at zero permits everything, which is the safe
 * default for a constructor and the wrong one for a running fleet.
 *
 * Returns: (transfer full): a new #ClawtLoopGuard
 */
ClawtLoopGuard *clawt_loop_guard_new(void);

/**
 * clawt_loop_guard_set_limits:
 * @self: a #ClawtLoopGuard
 * @max_hops: how far a message may travel agent-to-agent
 * @rate_per_minute: messages one agent may send per minute, or 0 for no limit
 * @cycle_window: how many recent messages per room to remember
 */
void clawt_loop_guard_set_limits(ClawtLoopGuard *self,
                                 guint           max_hops,
                                 guint           rate_per_minute,
                                 guint           cycle_window);

/**
 * clawt_loop_guard_set_task_budget:
 * @self: a #ClawtLoopGuard
 * @budget_usd: spend cap for one task and everything it spawns, or 0 for none
 */
void clawt_loop_guard_set_task_budget(ClawtLoopGuard *self,
                                      gdouble         budget_usd);

/**
 * clawt_loop_guard_check:
 * @self: a #ClawtLoopGuard
 * @message: (transfer none): the message about to be sent
 * @error: (out) (optional): return location for why it was refused
 *
 * Decides whether a message may be sent, and records it if so.
 *
 * Returns: %TRUE if the message may proceed
 */
gboolean clawt_loop_guard_check(ClawtLoopGuard  *self,
                                ClawtMessage    *message,
                                GError         **error);

/**
 * clawt_loop_guard_check_in_room:
 * @self: a #ClawtLoopGuard
 * @message: (transfer none): the message about to be sent
 * @room_max_hops: this room's own hop limit, or 0 to use the fleet's
 * @error: (out) (optional): return location for why it was refused
 *
 * clawt_loop_guard_check() with the destination room's hop limit.
 *
 * `rooms.max_hops` was parsed, stored on the #ClawtRoom and read by
 * nobody: clawt_room_get_max_hops() had no caller at all, so every hop
 * in every room counted against `orchestration.max_hops` whatever a room
 * declared.  This is the parameter that was missing.
 *
 * **A room may raise the fleet limit as well as lower it**, and that is
 * a decision rather than an oversight.  The case the key exists for is a
 * *conversation*: three agents in a room each reply one hop deeper, so
 * an ordinary standup reaches the fleet ceiling on its own without
 * anybody doing anything wrong -- which check_hops() already says in its
 * own refusal, where the only advice it can offer is to raise
 * `orchestration.max_hops`.  Taking that advice loosens the limit for
 * every delegation chain in the fleet, to fix one room.  Only-tighten
 * would leave the key unable to do the one thing it was written for, and
 * leave the wider change as the only remedy.
 *
 * It is not an escalation: `rooms:` is in the same clawtilla.yaml, set
 * by the same hand as the fleet value, and the room limit reaches
 * nothing else -- the rate limit, the task budget and the cycle detector
 * are untouched, so a loop that costs money still has three limits on
 * it.  Only the hop count, only in a room somebody declared.
 *
 * The other three checks take no room value because none of them is
 * about a room: rate is per sender, budget is per task, and the cycle
 * window is already keyed by room and needs no limit of its own.
 *
 * Returns: %TRUE if the message may proceed
 */
gboolean clawt_loop_guard_check_in_room(ClawtLoopGuard  *self,
                                        ClawtMessage    *message,
                                        guint            room_max_hops,
                                        GError         **error);

/**
 * clawt_loop_guard_record_spend:
 * @self: a #ClawtLoopGuard
 * @task_id: the task the spend belongs to
 * @amount_usd: how much
 *
 * Adds to a task's running cost.  Charged against the task rather than the
 * agent, so a chief-of-staff delegating ten subtasks is bounded as one
 * piece of work rather than ten.
 */
void clawt_loop_guard_record_spend(ClawtLoopGuard *self,
                                   const gchar    *task_id,
                                   gdouble         amount_usd);

/**
 * clawt_loop_guard_get_task_spend:
 * @self: a #ClawtLoopGuard
 * @task_id: a task id
 *
 * Returns: what has been spent on @task_id so far
 */
gdouble clawt_loop_guard_get_task_spend(ClawtLoopGuard *self,
                                        const gchar    *task_id);

/**
 * clawt_loop_guard_forget_task:
 * @self: a #ClawtLoopGuard
 * @task_id: a task id
 *
 * Drops a finished task's accounting, so the table does not grow without
 * bound over a long-running daemon.
 */
void clawt_loop_guard_forget_task(ClawtLoopGuard *self,
                                  const gchar    *task_id);

/**
 * clawt_loop_guard_reset:
 * @self: a #ClawtLoopGuard
 *
 * Forgets all history.  For tests, and for a daemon reload.
 */
void clawt_loop_guard_reset(ClawtLoopGuard *self);

G_END_DECLS
