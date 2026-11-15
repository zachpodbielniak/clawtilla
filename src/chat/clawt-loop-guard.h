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
 * Five limits, because each catches a case the others do not:
 *
 *   hops    catches a chain that grows: A asks B asks C asks D...
 *   rate    catches one agent flooding, however shallow each message
 *   budget  catches an expensive loop that is short enough to pass the rest
 *   tree    catches a fan-out: a task tree that is shallow and wide and
 *           talks without anything in it settling
 *   cycles  catches two agents alternating the same two replies, where
 *           every message is a fresh chain with a depth of one
 *
 * Applied before a message is enqueued rather than on delivery, so a
 * runaway fan-out is stopped at the source rather than after the queue has
 * already grown.
 *
 * Four limits that **refuse a message** are not four limits that end a
 * loop.  A stuck pair kept producing turns, each of which was refused,
 * and each of those turns had already cost a model call before the
 * refusal arrived -- so the guard was billed for the runaway it was
 * built to prevent.  The cycle detector therefore does more than refuse:
 * it **stalls the room**, and everything an agent sends into a stalled
 * room is refused without a turn until a person says something there.
 *
 * Which limit stalls is a decision, not an oversight:
 *
 *   cycle   stalls.  The agent has demonstrated that it will produce the
 *           same text again, so there is nothing to wait for.
 *   hops    does not.  A conversation reaches the ceiling on its own and
 *           the refusal tells the agent to answer directly, which is an
 *           instruction it can follow.
 *   rate    does not.  It clears by itself in under a minute.
 *   budget  does not.  It is about money rather than about progress, and
 *           raising the budget is the operator's call.
 *
 * Ending an exchange needs to know which senders are agents, because
 * clawtilla ends conversations between agents and never a person's.  The
 * guard does not know that on its own, so it is told:
 * clawt_loop_guard_set_peer_func().  With no peer function set nothing
 * is a peer, nothing ever stalls, and the guard behaves exactly as it
 * did before stalls existed.
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
 *
 * How long one of those is remembered for is
 * clawt_loop_guard_set_cycle_seconds().
 */
void clawt_loop_guard_set_limits(ClawtLoopGuard *self,
                                 guint           max_hops,
                                 guint           rate_per_minute,
                                 guint           cycle_window);

/**
 * clawt_loop_guard_get_max_hops:
 * @self: a #ClawtLoopGuard
 *
 * The fleet's hop limit, for anything that has to decide *before* a
 * message exists.
 *
 * clawt_mcp_tools_is_permitted() withholds the peer tools from a turn
 * already at the limit rather than letting them be called and refused,
 * and it has no message to check -- so it asks here.  Reading
 * `orchestration.max_hops` a second time from the config would be a
 * second answer to the same question, and the two would differ the day
 * somebody set the guard from anywhere else.
 *
 * A room's own limit is deliberately not consulted: which room a call
 * will land in is not known until the message is built, and a gate that
 * guessed would hide a tool an agent could legitimately use.
 *
 * Returns: the limit, or 0 when there is none
 */
guint clawt_loop_guard_get_max_hops(ClawtLoopGuard *self);

/**
 * clawt_loop_guard_set_cycle_seconds:
 * @self: a #ClawtLoopGuard
 * @seconds: how long a repeat counts as a loop, or 0 to disable the check
 *
 * How far back the cycle check looks, in wall-clock terms.
 *
 * @cycle_window bounds the memory -- how many fingerprints a room keeps
 * -- and this bounds the meaning.  Without it the window was however
 * long its last @cycle_window messages took, which in a quiet room is
 * hours: an agent repeating one error string was silenced from its first
 * report until ten more messages had passed through the room.
 *
 * Its own setter rather than a fifth argument to
 * clawt_loop_guard_set_limits(), so callers that do not care keep
 * compiling.
 */
void clawt_loop_guard_set_cycle_seconds(ClawtLoopGuard *self,
                                        guint           seconds);

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
 * @reaches_anybody: whether it is delivered to at least one mailbox.
 *   A message that reaches nobody still faces the stall, hop and budget
 *   checks, and escapes only the rate limit and the cycle detector --
 *   the two that measure what a sender spends of other agents' time.
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
                                        gboolean         reaches_anybody,
                                        GError         **error);

/**
 * ClawtLoopGuardPeerFunc:
 * @sender_id: (nullable): who sent a message
 * @user_data: what was passed to clawt_loop_guard_set_peer_func()
 *
 * Whether @sender_id is an agent in the fleet rather than a person.
 *
 * Returns: %TRUE for a fleet agent
 */
typedef gboolean (*ClawtLoopGuardPeerFunc)(const gchar *sender_id,
                                           gpointer     user_data);

/**
 * clawt_loop_guard_set_peer_func:
 * @self: a #ClawtLoopGuard
 * @func: (nullable) (scope notified) (closure user_data): the predicate, or
 *   %NULL to treat nobody as a peer
 * @user_data: passed to @func
 * @notify: (nullable): frees @user_data
 *
 * Teaches the guard which senders are agents, which is what lets it end
 * an exchange rather than only refuse one more message.
 *
 * Set on the guard rather than passed at each check, so that the rule
 * lives in the function that applies it. A check that took the answer as
 * an argument would be a rule about whichever caller remembered to pass
 * it, which this codebase has paid for five times.
 */
void clawt_loop_guard_set_peer_func(ClawtLoopGuard         *self,
                                    ClawtLoopGuardPeerFunc  func,
                                    gpointer                user_data,
                                    GDestroyNotify          notify);

/**
 * clawt_loop_guard_stall_room:
 * @self: a #ClawtLoopGuard
 * @room_id: the room whose exchange is over
 * @reason: why
 * @detail: (nullable): the repeated text, or whatever names the loop
 *
 * Ends an exchange from outside the guard -- the turn watchdog and the
 * room budget both arrive here. Stalling a room that is already stalled
 * keeps the first reason, because the first one is the one that explains
 * how the fleet got here.
 *
 * Returns: %TRUE if this call is what stalled it
 */
gboolean clawt_loop_guard_stall_room(ClawtLoopGuard   *self,
                                     const gchar      *room_id,
                                     ClawtStallReason  reason,
                                     const gchar      *detail);

/**
 * clawt_loop_guard_get_stall_reason:
 * @self: a #ClawtLoopGuard
 * @room_id: (nullable): a room
 *
 * Returns: why the exchange in @room_id was ended, or %CLAWT_STALL_NONE
 */
ClawtStallReason clawt_loop_guard_get_stall_reason(ClawtLoopGuard *self,
                                                   const gchar    *room_id);

/**
 * clawt_loop_guard_get_stall_detail:
 * @self: a #ClawtLoopGuard
 * @room_id: (nullable): a room
 *
 * Returns: (transfer none) (nullable): what was repeating, or %NULL
 */
const gchar *clawt_loop_guard_get_stall_detail(ClawtLoopGuard *self,
                                               const gchar    *room_id);

/**
 * clawt_loop_guard_clear_stall:
 * @self: a #ClawtLoopGuard
 * @room_id: a room
 *
 * Lets the agents in @room_id talk again. A person sending into the room
 * does this on their own; this is for the client verbs that say so
 * explicitly.
 *
 * Returns: %TRUE if the room had been stalled
 */
gboolean clawt_loop_guard_clear_stall(ClawtLoopGuard *self,
                                      const gchar    *room_id);

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
 * ClawtLoopGuardTreeFunc:
 * @sender_id: (nullable): who is sending, or asking
 * @room_id: (nullable): which conversation, when the caller knows
 * @task_id: (nullable): the task the message names, if any
 * @user_data: what was passed to clawt_loop_guard_set_tree_func()
 *
 * Which task tree a message, or a turn, belongs to: the id of its root
 * task.  The guard knows neither agents nor tasks, so the daemon answers
 * -- from @task_id when the message names one, otherwise from the task
 * @sender_id's turn in @room_id is working on, walked up its parents.
 *
 * Returns: (transfer full) (nullable): the root task id, or %NULL for a
 *   message that belongs to no tree
 */
typedef gchar *(*ClawtLoopGuardTreeFunc)(const gchar *sender_id,
                                         const gchar *room_id,
                                         const gchar *task_id,
                                         gpointer     user_data);

/**
 * clawt_loop_guard_set_tree_func:
 * @self: a #ClawtLoopGuard
 * @func: (nullable) (scope notified) (closure user_data): the resolver,
 *   or %NULL to put nothing in a tree
 * @user_data: passed to @func
 * @notify: (nullable): frees @user_data
 *
 * Teaches the guard how to find a message's task tree.  With none set
 * nothing belongs to a tree and the tree budget counts nothing, which is
 * what a guard built for a test of something else should do.
 */
void clawt_loop_guard_set_tree_func(ClawtLoopGuard         *self,
                                    ClawtLoopGuardTreeFunc  func,
                                    gpointer                user_data,
                                    GDestroyNotify          notify);

/**
 * clawt_loop_guard_set_tree_budget:
 * @self: a #ClawtLoopGuard
 * @budget: requests one task tree may send between settlements, or 0 for
 *   no limit (`orchestration.tree_message_budget`)
 */
void clawt_loop_guard_set_tree_budget(ClawtLoopGuard *self, guint budget);

/**
 * clawt_loop_guard_get_tree_budget:
 * @self: a #ClawtLoopGuard
 *
 * Returns: the tree budget, or 0 when there is none
 */
guint clawt_loop_guard_get_tree_budget(ClawtLoopGuard *self);

/**
 * clawt_loop_guard_resolve_tree:
 * @self: a #ClawtLoopGuard
 * @sender_id: (nullable): who is sending, or whose turn is asking
 * @room_id: (nullable): the conversation, when known
 * @task_id: (nullable): a task the caller already has
 *
 * The root of the task tree, through the resolver set with
 * clawt_loop_guard_set_tree_func().  The one way to ask, so the gate
 * that withholds tools and the check that refuses a message cannot
 * disagree about which tree they are looking at.
 *
 * Returns: (transfer full) (nullable): the root task id, or %NULL
 */
gchar *clawt_loop_guard_resolve_tree(ClawtLoopGuard *self,
                                     const gchar    *sender_id,
                                     const gchar    *room_id,
                                     const gchar    *task_id);

/**
 * clawt_loop_guard_get_tree_count:
 * @self: a #ClawtLoopGuard
 * @root_id: (nullable): a root task id
 *
 * How many requests the tree under @root_id has sent since anything in
 * it last settled.
 *
 * Returns: the count
 */
guint clawt_loop_guard_get_tree_count(ClawtLoopGuard *self,
                                      const gchar    *root_id);

/**
 * clawt_loop_guard_tree_exhausted:
 * @self: a #ClawtLoopGuard
 * @root_id: (nullable): a root task id
 *
 * Whether the tree under @root_id has used its budget.  What
 * clawt_mcp_tools_is_permitted() asks before offering a tool that would
 * send a request, exactly as it asks about `max_hops`.
 *
 * Returns: %TRUE when no further request from that tree is allowed
 */
gboolean clawt_loop_guard_tree_exhausted(ClawtLoopGuard *self,
                                         const gchar    *root_id);

/**
 * clawt_loop_guard_renew_tree:
 * @self: a #ClawtLoopGuard
 * @root_id: (nullable): a root task id
 *
 * Starts the tree's count again, and lets it raise its alert again the
 * next time it reaches the limit.  The daemon calls this whenever any
 * task in the tree settles: a settlement is durable progress, and the
 * budget exists to stop the talk that is not.
 */
void clawt_loop_guard_renew_tree(ClawtLoopGuard *self,
                                 const gchar    *root_id);

/**
 * clawt_loop_guard_tree_alert_text:
 * @tree: (nullable): what the tree is called, as a person reads it
 * @count: how many requests it sent
 * @limit: the budget
 *
 * The sentence the `task.tree_budget` alert carries, from one place so
 * the daemon and the three clients word it the same way.
 *
 * Returns: (transfer full): the sentence
 */
gchar *clawt_loop_guard_tree_alert_text(const gchar *tree,
                                        guint        count,
                                        guint        limit);

/**
 * clawt_loop_guard_set_pair_budget:
 * @self: a #ClawtLoopGuard
 * @budget: how many rounds of requests two agents may send each other,
 *   or 0 for no limit (`orchestration.pair_message_budget`)
 * @quiet_seconds: how long the two must have sent each other nothing
 *   for the count to start again, or 0 for never
 *   (`orchestration.pair_quiet_minutes`)
 *
 * The one count nothing an agent does renews: only a person writing to
 * either agent (clawt_loop_guard_renew_pairs_of()) or the pair going
 * quiet starts it again.  A round is every request one agent sends the
 * other from one turn.
 *
 * Since: 0.34
 */
void clawt_loop_guard_set_pair_budget(ClawtLoopGuard *self,
                                      guint           budget,
                                      guint           quiet_seconds);

/**
 * clawt_loop_guard_get_pair_budget:
 * @self: a #ClawtLoopGuard
 *
 * Returns: the budget, or 0 for none
 *
 * Since: 0.34
 */
guint clawt_loop_guard_get_pair_budget(ClawtLoopGuard *self);

/**
 * clawt_loop_guard_get_pair_count:
 * @self: a #ClawtLoopGuard
 * @a: (nullable): one agent
 * @b: (nullable): the other
 *
 * Returns: how many rounds the two have had since the count last began
 *
 * Since: 0.34
 */
guint clawt_loop_guard_get_pair_count(ClawtLoopGuard *self,
                                      const gchar    *a,
                                      const gchar    *b);

/**
 * clawt_loop_guard_check_pair:
 * @self: a #ClawtLoopGuard
 * @sender: the agent sending a request
 * @peer: the agent it is for
 * @turn: the sender's turn number, or 0 when it is not known
 * @error: (nullable): return location for a #GError
 *
 * Whether @sender may send @peer another request.  Counts nothing: the
 * caller counts with clawt_loop_guard_count_pair() once the message is
 * certain to go.  A request from the turn the pair last counted is
 * always allowed -- it is the same round.
 *
 * Returns: %TRUE when it may; %FALSE with a #CLAWT_ERROR_LOOP_LIMIT
 *   whose message says what reopens the pair
 *
 * Since: 0.34
 */
gboolean clawt_loop_guard_check_pair(ClawtLoopGuard  *self,
                                     const gchar     *sender,
                                     const gchar     *peer,
                                     guint64          turn,
                                     GError         **error);

/**
 * clawt_loop_guard_count_pair:
 * @self: a #ClawtLoopGuard
 * @sender: the agent that sent a request
 * @peer: the agent it went to
 * @turn: the sender's turn number, or 0 when it is not known
 *
 * Counts one round between the two, unless @sender's turn @turn was
 * already counted.  Emits #ClawtLoopGuard::pair-exhausted at the limit.
 *
 * Since: 0.34
 */
void clawt_loop_guard_count_pair(ClawtLoopGuard *self,
                                 const gchar    *sender,
                                 const gchar    *peer,
                                 guint64         turn);

/**
 * clawt_loop_guard_renew_pairs_of:
 * @self: a #ClawtLoopGuard
 * @agent_id: (nullable): an agent a person, a routine or a trigger has
 *   just written to
 *
 * Starts the count again for every pair @agent_id is half of.
 *
 * Since: 0.34
 */
void clawt_loop_guard_renew_pairs_of(ClawtLoopGuard *self,
                                     const gchar    *agent_id);

/**
 * clawt_loop_guard_age_pairs:
 * @self: a #ClawtLoopGuard
 * @seconds: how far to move every pair's last request into the past
 *
 * For tests of the quiet rule, which otherwise need a real half hour.
 *
 * Since: 0.34
 */
void clawt_loop_guard_age_pairs(ClawtLoopGuard *self,
                                guint           seconds);

/**
 * clawt_loop_guard_pair_alert_text:
 * @first: (nullable): one agent's name
 * @second: (nullable): the other's
 * @count: how many rounds they had
 * @limit: the budget
 *
 * The sentence the `crosstalk.pair_budget` alert carries, from one place
 * so the daemon and the three clients word it the same way.
 *
 * Returns: (transfer full): the sentence
 *
 * Since: 0.34
 */
gchar *clawt_loop_guard_pair_alert_text(const gchar *first,
                                        const gchar *second,
                                        guint        count,
                                        guint        limit);

/**
 * clawt_loop_guard_reset:
 * @self: a #ClawtLoopGuard
 *
 * Forgets all history.  For tests, and for a daemon reload.
 */
void clawt_loop_guard_reset(ClawtLoopGuard *self);

G_END_DECLS
