/*
 * clawt-steer-policy.h - Which deliveries steer into a running turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Without steering, a message for an agent whose turn is running waits
 * in libreclaw's queue and becomes the next turn.  A delegator's
 * correction ("stop, use the staging DB") then arrives after the work it
 * was meant to change, and a peer's follow-up costs a whole extra turn
 * that rereads everything.  Steering hands the words to the turn that is
 * running instead (libreclaw 0.30.0, ai-glib's AiSteerable).
 *
 * This is the rule for *which* deliveries are asked to steer: the
 * agent's `agents.steer` level, and the rules no level lifts.  One pure
 * function over the facts the router has at delivery time, so it can be
 * tested on its own and the clients can explain the same rule the
 * daemon applies.  The agent may still say no -- an engine that cannot
 * take input, a turn that ended -- and the message then waits as before.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * ClawtSteerFacts:
 * @policy: the receiving agent's `agents.steer`
 * @link_can_steer: the agent's link said it answers steer requests
 * @turn_running: a turn is running in the delivery's own room
 * @group_room: the room has more than two members (rounds and threads)
 * @handed_answer: the delivery is the daemon's notice handing back the
 *   answer to a request the running turn made (since 0.34.2)
 * @person_room: the room is the agent's direct room with the operator
 * @sender: (nullable): who sent the delivery ("user" for the operator,
 *   %CLAWT_SYSTEM_SENDER for the daemon, otherwise an agent id)
 * @sender_is_agent: @sender is an agent of the fleet
 * @is_result: a result the router holds for the end of a busy turn
 *   (a settle notice or a peer's answer; see clawt_mailbox_router_drain())
 * @consult: the delivery is a consultation question
 * @task_id: (nullable): the task the delivery is about
 * @model: (nullable): a model the delivery asks its own turn to run on
 * @turn_origin: (nullable): who the running turn is answering
 * @turn_task_id: (nullable): the task the running turn is on
 * @turn_task_delegator: (nullable): who delegated that task
 * @turn_consult: the running turn is a consultation
 *
 * Everything clawt_steer_decide() reads, gathered by the router.
 *
 * Since: 0.24.0
 */
typedef struct {
	ClawtSteerPolicy  policy;
	gboolean          link_can_steer;
	gboolean          turn_running;
	gboolean          group_room;
	gboolean          person_room;
	const gchar      *sender;
	gboolean          sender_is_agent;
	gboolean          is_result;
	gboolean          handed_answer;
	gboolean          consult;
	const gchar      *task_id;
	const gchar      *model;
	const gchar      *turn_origin;
	const gchar      *turn_task_id;
	const gchar      *turn_task_delegator;
	gboolean          turn_consult;
} ClawtSteerFacts;

/**
 * clawt_steer_decide:
 * @facts: what the router knows about the delivery and the running turn
 * @why: (out) (optional) (transfer none): one line saying why not, or
 *   why, for the log and the tests
 *
 * Whether to ask the agent to steer this delivery into its running turn.
 *
 * The rules no level lifts, in the order they are checked:
 *
 *  - an agent that never said it answers steer requests is never asked;
 *  - only into a turn running in the delivery's *own* room -- never a
 *    room turn from another room;
 *  - never in a group room, where every answer continues one thread of
 *    one round and a steered part would be answered under another's;
 *  - never the daemon's own notices, and never a result held for the
 *    end of the turn: those are merged into one wake after it;
 *  - never a delivery that asks for a model of its own, or names a task
 *    the running turn is not on: both are a turn of their own by
 *    definition;
 *  - never across the consultation line: a consultation question never
 *    steers into a turn that can act (it would run with every tool), an
 *    ordinary message never steers into a consultation (it would run
 *    with none), and into a consultation only the consulting agent may
 *    steer, asking again.
 *
 * Then the level: %CLAWT_STEER_DELEGATOR steers what the agent the turn
 * is answering, or the delegator of its task, sends; %CLAWT_STEER_PEERS
 * adds any other agent; %CLAWT_STEER_ALL adds the operator in their
 * direct room with the agent.
 *
 * Returns: %TRUE to ask for a steer
 *
 * Since: 0.24.0
 */
gboolean clawt_steer_decide(const ClawtSteerFacts  *facts,
			    const gchar           **why);

/**
 * clawt_steer_policy_explanation:
 * @policy: a level
 *
 * One plain paragraph for the clients' agent settings, so GTK, web and
 * lrg explain the level the same way the daemon applies it.
 *
 * Returns: (transfer none): the explanation
 *
 * Since: 0.24.0
 */
const gchar *clawt_steer_policy_explanation(ClawtSteerPolicy policy);

/**
 * clawt_steer_policy_label:
 * @policy: a level
 *
 * Returns: (transfer none): the level's name in a picker
 *
 * Since: 0.24.0
 */
const gchar *clawt_steer_policy_label(ClawtSteerPolicy policy);

/**
 * clawt_steer_policy_count:
 *
 * Returns: how many levels there are, for walking them in order
 *
 * Since: 0.24.0
 */
guint clawt_steer_policy_count(void);

/**
 * clawt_steer_policy_nick:
 * @policy: a level
 *
 * Returns: (transfer none): the config spelling ("never", "delegator",
 *   "peers", "all")
 *
 * Since: 0.24.0
 */
const gchar *clawt_steer_policy_nick(ClawtSteerPolicy policy);

/**
 * clawt_steer_policy_from_nick:
 * @nick: (nullable): a config spelling
 *
 * Returns: the level, %CLAWT_STEER_DELEGATOR (the default) when @nick is
 *   %NULL or unknown
 *
 * Since: 0.24.0
 */
ClawtSteerPolicy clawt_steer_policy_from_nick(const gchar *nick);

/**
 * clawt_steer_outcome_is_steered:
 * @outcome: (nullable): libreclaw's answer
 *
 * Returns: %TRUE for "delivered" and "indeterminate" -- the running turn
 *   took the words, or may have, which counts the same
 *
 * Since: 0.24.0
 */
gboolean clawt_steer_outcome_is_steered(const gchar *outcome);

/**
 * clawt_message_steered_label:
 *
 * What the clients draw on a message that was steered into a running
 * turn, instead of drawing it as a message waiting its turn.
 *
 * Returns: (transfer none): the label
 *
 * Since: 0.24.0
 */
const gchar *clawt_message_steered_label(void);

/**
 * clawt_message_steered_tooltip:
 * @outcome: (nullable): "delivered" or "indeterminate"
 *
 * Returns: (transfer none): the longer explanation under the label
 *
 * Since: 0.24.0
 */
const gchar *clawt_message_steered_tooltip(const gchar *outcome);

G_END_DECLS
