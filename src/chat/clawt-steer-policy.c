/*
 * clawt-steer-policy.c - Which deliveries steer into a running turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-steer-policy.h"

/*
 * The levels in the order a picker lists them, with what each is called
 * and what it does.  One table, so the settings in three clients and the
 * schema's description cannot drift from each other.
 */
static const struct {
	ClawtSteerPolicy  policy;
	const gchar      *nick;
	const gchar      *label;
	const gchar      *explanation;
} clawt_steer_levels[] = {
	{ CLAWT_STEER_NEVER, "never", "Never",
	  "Nothing is steered. A message for this agent while it works waits "
	  "and becomes its next turn, as it did before steering existed." },
	{ CLAWT_STEER_DELEGATOR, "delegator", "From whoever handed out the work",
	  "A message from the agent whose request the running turn is "
	  "answering, or from the delegator of the task it is working, is "
	  "handed to that turn at its next step, so a correction lands before "
	  "the work it corrects is done. Everything else waits for a turn of "
	  "its own. The default." },
	{ CLAWT_STEER_PEERS, "peers", "From any agent in the conversation",
	  "As above, and any other agent's message in the same conversation "
	  "joins the running turn instead of costing a turn of its own. Your "
	  "own messages are still held and sent when the turn ends." },
	{ CLAWT_STEER_ALL, "all", "From agents and from you",
	  "As above, and what you type in your own conversation with this "
	  "agent while it works goes straight into the running turn instead "
	  "of being held until the turn ends." }
};

guint
clawt_steer_policy_count(void)
{
	return G_N_ELEMENTS(clawt_steer_levels);
}

/* The table row for @policy, or the default's when it is out of range. */
static guint
level_index(
	ClawtSteerPolicy policy
){
	guint i;

	for (i = 0; i < G_N_ELEMENTS(clawt_steer_levels); i++)
		if (clawt_steer_levels[i].policy == policy)
			return i;

	return 1;
}

const gchar *
clawt_steer_policy_nick(
	ClawtSteerPolicy policy
){
	return clawt_steer_levels[level_index(policy)].nick;
}

const gchar *
clawt_steer_policy_label(
	ClawtSteerPolicy policy
){
	return clawt_steer_levels[level_index(policy)].label;
}

const gchar *
clawt_steer_policy_explanation(
	ClawtSteerPolicy policy
){
	return clawt_steer_levels[level_index(policy)].explanation;
}

ClawtSteerPolicy
clawt_steer_policy_from_nick(
	const gchar *nick
){
	guint i;

	for (i = 0; nick != NULL && i < G_N_ELEMENTS(clawt_steer_levels); i++)
		if (g_strcmp0(nick, clawt_steer_levels[i].nick) == 0)
			return clawt_steer_levels[i].policy;

	return CLAWT_STEER_DELEGATOR;
}

gboolean
clawt_steer_outcome_is_steered(
	const gchar *outcome
){
	return g_strcmp0(outcome, "delivered") == 0 ||
		g_strcmp0(outcome, "indeterminate") == 0;
}

const gchar *
clawt_message_steered_label(void)
{
	return "steered into the running turn";
}

const gchar *
clawt_message_steered_tooltip(
	const gchar *outcome
){
	if (g_strcmp0(outcome, "indeterminate") == 0)
		return "Handed to the turn that was already running. The engine "
			"did not confirm it took the words before the turn ended, so it "
			"is counted as taken and not sent again: running it twice would "
			"be worse. The turn's answer answers this message too.";

	return "Handed to the turn that was already running, at its next step, "
		"instead of waiting to become a turn of its own. The turn's answer "
		"answers this message too.";
}

/* Says @line, when the caller asked, and returns @answer. */
static gboolean
decided(
	const gchar **why,
	const gchar  *line,
	gboolean      answer
){
	if (why != NULL)
		*why = line;

	return answer;
}

gboolean
clawt_steer_decide(
	const ClawtSteerFacts  *facts,
	const gchar           **why
){
	gboolean person;
	gboolean handed_out;

	g_return_val_if_fail(facts != NULL, FALSE);

	/* The level first: never means never. */
	if (facts->policy == CLAWT_STEER_NEVER)
		return decided(why, "the agent's steer policy is never", FALSE);

	/*
	 * An agent that never said it answers would take the message as an
	 * ordinary turn and never say so, and the daemon would hold its
	 * description waiting for an answer that cannot come.
	 */
	if (!facts->link_can_steer)
		return decided(why, "the agent does not answer steer requests",
			FALSE);

	/* Into the turn running in this room, never another room's. */
	if (!facts->turn_running)
		return decided(why, "no turn is running in its room", FALSE);

	/*
	 * A group room's answer continues one thread of one round, read off
	 * the delivery that started the turn; a steered part from another
	 * thread would be answered under the wrong one, and the once-per-
	 * thread rule would count a turn that never ran.
	 */
	if (facts->group_room)
		return decided(why, "a group room's turns follow its rounds", FALSE);

	/*
	 * The answer to a request this very turn made (since 0.34.2), at
	 * every level but never.  The asker is told to end its turn and be
	 * woken with the answer, but a turn that is still running when the
	 * answer lands -- one that had more to do, or that waited -- is the
	 * turn that wants it: handed over now it is used once, where queued
	 * it would start a second turn whose whole content is something the
	 * first has by then worked out or reported.  It is a notice of the
	 * daemon's, so it has to be admitted ahead of the rule below.
	 */
	if (facts->handed_answer)
		return decided(why,
			"it answers a request the running turn made", TRUE);

	/*
	 * The daemon's notices and the results held for the end of a busy
	 * turn are merged into one wake after it.  Handed to the turn one by
	 * one they would be the five interruptions that merge exists to
	 * avoid, and a settle notice read mid-turn is read before the work
	 * it reports on could be checked.
	 */
	if (g_strcmp0(facts->sender, CLAWT_SYSTEM_SENDER) == 0)
		return decided(why, "the daemon's own notices take a turn", FALSE);

	if (facts->is_result)
		return decided(why, "a result waits for the turn to end", FALSE);

	/* A model of its own is a turn of its own: a turn has one model. */
	if (facts->model != NULL && *facts->model != '\0')
		return decided(why, "it asks for a model of its own", FALSE);

	/*
	 * Another task is another piece of work.  Folded into this turn,
	 * the turn would end both, and one of them would be marked done by
	 * an answer that was never about it.
	 */
	if (facts->task_id != NULL &&
		g_strcmp0(facts->task_id, facts->turn_task_id) != 0)
		return decided(why, "it is about another task", FALSE);

	/*
	 * The consultation line.  A consultation is offered no tool that
	 * acts; steered into a turn that can act it would run with all of
	 * them, and an ordinary message steered into a consultation would
	 * run with none.  Into a consultation, only its asker, asking again.
	 */
	if (facts->consult != facts->turn_consult)
		return decided(why, facts->consult
			? "a consultation never steers into a turn that can act"
			: "an ordinary message never steers into a consultation",
			FALSE);

	if (facts->turn_consult &&
		g_strcmp0(facts->sender, facts->turn_origin) != 0)
		return decided(why,
			"only the consulting agent steers into a consultation", FALSE);

	person = g_strcmp0(facts->sender, "user") == 0;

	/*
	 * A person: only under all, and only in their own conversation with
	 * the agent -- anywhere else their words would land in a turn they
	 * are not watching.
	 */
	if (person) {
		if (facts->policy != CLAWT_STEER_ALL)
			return decided(why, "your messages steer only under all",
				FALSE);
		if (!facts->person_room)
			return decided(why, "you steer only in your conversation with "
				"the agent", FALSE);
		return decided(why, "your message in your conversation, under all",
			TRUE);
	}

	if (!facts->sender_is_agent)
		return decided(why, "only agents and you can steer", FALSE);

	/*
	 * Whoever handed out the work this turn is on: the agent it is
	 * answering, or the one that delegated its task.  Their word is the
	 * one that goes into that work.
	 */
	handed_out = (facts->sender != NULL &&
		(g_strcmp0(facts->sender, facts->turn_origin) == 0 ||
		 g_strcmp0(facts->sender, facts->turn_task_delegator) == 0));

	if (handed_out)
		return decided(why, "its sender handed out the running turn's work",
			TRUE);

	if (facts->policy == CLAWT_STEER_DELEGATOR)
		return decided(why, "its sender did not hand out the running "
			"turn's work", FALSE);

	return decided(why, "another agent in the conversation, under peers",
		TRUE);
}
