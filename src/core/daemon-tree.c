/*
 * daemon-tree.c - A message budget per task tree
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `orchestration.max_hops` bounds how deep a chain of delegations goes
 * and says nothing about how wide it is: a chief that hands work to ten
 * agents, each of which asks two more, is three hops deep and can talk
 * all night.  `orchestration.tree_message_budget` counts the requests a
 * task tree sends -- one per call, however many teammates it reaches --
 * and renews whenever any task in the tree settles.
 *
 * The counting is the loop guard's (clawt-loop-guard.c), because that is
 * the one function every message passes through.  What it cannot know is
 * which tree a message belongs to, which takes the agents and the tasks;
 * this file answers that, renews a tree when its work settles, and turns
 * the guard's one signal per exhaustion into the alert that names it.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * How far up a parent chain the walk goes before it gives up.  Far past
 * any real tree -- `orchestration.max_hops` bounds a delegation chain's
 * depth long before this -- and only here so a parent cycle, which the
 * manager should never produce, ends in an answer rather than a hang.
 */
#define TREE_WALK_LIMIT (256)

/* How much of a root task's prompt names the tree in an alert. */
#define TREE_LABEL_CHARS (60)

/*
 * The root of @task_id's tree, or %NULL when the manager does not know
 * it.  A parent the manager has forgotten ends the walk where it is:
 * the tree is then rooted at the oldest task still on record, which is
 * the same answer for every task under it.
 */
static gchar *
root_of(
	ClawtDaemon *self,
	const gchar *task_id
){
	ClawtTask *task;
	guint steps;

	if (task_id == NULL || self->tasks == NULL)
		return NULL;

	task = clawt_task_manager_get(self->tasks, task_id);

	if (task == NULL)
		return NULL;

	for (steps = 0; steps < TREE_WALK_LIMIT; steps++) {
		const gchar *parent_id = clawt_task_get_parent_id(task);
		ClawtTask *parent;

		if (parent_id == NULL)
			break;

		parent = clawt_task_manager_get(self->tasks, parent_id);

		if (parent == NULL)
			break;

		task = parent;
	}

	return g_strdup(clawt_task_get_id(task));
}

/*
 * The guard's resolver (ClawtLoopGuardTreeFunc).
 *
 * The task the message names when it names one -- a delegation names
 * the child it created, a task's own messages name the task -- and
 * otherwise the task the sender's turn in that room is working on, which
 * is the daemon's record of the delivery and never the model's claim.
 * clawt_agent_get_turn_task_id_in() falls back to every running turn
 * and answers only when they agree, so a call the daemon cannot place
 * belongs to no tree rather than to a guessed one.
 */
static gchar *
resolve_tree(
	const gchar *sender_id,
	const gchar *room_id,
	const gchar *task_id,
	gpointer     user_data
){
	ClawtDaemon *self = user_data;
	ClawtAgent *agent;

	if (task_id != NULL)
		return root_of(self, task_id);

	if (sender_id == NULL || self->agents == NULL)
		return NULL;

	agent = clawt_agent_manager_get(self->agents, sender_id);

	if (agent == NULL)
		return NULL;

	return root_of(self, clawt_agent_get_turn_task_id_in(agent, room_id));
}

gchar *
clawt_daemon_tree_label(
	ClawtDaemon *self,
	const gchar *root_id
){
	ClawtTask *task;
	g_autofree gchar *brief = NULL;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

	if (root_id == NULL)
		return g_strdup("(unknown)");

	task = (self->tasks != NULL)
		? clawt_task_manager_get(self->tasks, root_id) : NULL;

	if (task == NULL)
		return g_strdup(root_id);

	/*
	 * What the work was, then which task and whose: a person reading
	 * the alert recognises the brief, and needs the id and the agent to
	 * find it on the Work page.
	 */
	brief = clawt_clip_line(clawt_task_get_prompt(task), TREE_LABEL_CHARS);

	return g_strdup_printf("\"%s\" (%s, %s)",
	                       brief != NULL ? brief : "",
	                       root_id,
	                       clawt_task_get_assignee(task) != NULL
	                           ? clawt_task_get_assignee(task) : "?");
}

/*
 * One alert per exhaustion, naming the tree.  A notice, not an error:
 * nothing broke, and the tree's agents can still answer and finish
 * their own work -- but a fleet that quietly stops talking reads as a
 * broken one to anybody who was not told why.
 */
static void
on_tree_exhausted(
	ClawtLoopGuard *guard,
	const gchar    *root_id,
	guint           count,
	guint           limit,
	gpointer        user_data
){
	ClawtDaemon *self = user_data;
	g_autoptr(ClawtEvent) event = NULL;
	g_autofree gchar *label = clawt_daemon_tree_label(self, root_id);
	g_autofree gchar *text = clawt_loop_guard_tree_alert_text(label, count,
	                                                          limit);
	ClawtTask *task;

	(void)guard;

	g_message("orchestration: %s", text);

	if (self->bus == NULL)
		return;

	event = clawt_event_new("task.tree_budget", root_id);
	clawt_event_set_detail(event, "tree", label);
	clawt_event_set_detail(event, "message", text);
	clawt_event_set_detail_int(event, "count", (gint64)count);
	clawt_event_set_detail_int(event, "limit", (gint64)limit);

	/*
	 * The root's assignee, for the reason task.changed carries one: a
	 * pod scoped to an agent hears about that agent's trees and not
	 * every tree in the fleet.
	 */
	task = (self->tasks != NULL)
		? clawt_task_manager_get(self->tasks, root_id) : NULL;

	if (task != NULL)
		clawt_event_set_detail(event, "agent", clawt_task_get_assignee(task));

	clawt_event_bus_publish(self->bus, event);
}

void
clawt_daemon_tree_settled(
	ClawtDaemon *self,
	ClawtTask   *task
){
	g_autofree gchar *root = NULL;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (task == NULL || self->guard == NULL)
		return;

	/*
	 * Before the settle notice goes out, which is what makes "the
	 * follow-up turn owed when every child has settled is never
	 * refused" true by construction: the notice itself is the daemon's
	 * and passes unmeasured, and by the time the delegator's turn on it
	 * sends anything the tree's count has already started again.
	 */
	root = root_of(self, clawt_task_get_id(task));
	clawt_loop_guard_renew_tree(self->guard, root);
}

void
clawt_daemon_tree_setup(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->guard == NULL)
		return;

	clawt_loop_guard_set_tree_func(self->guard, resolve_tree, self, NULL);
	g_signal_connect(self->guard, "tree-exhausted",
	                 G_CALLBACK(on_tree_exhausted), self);
}

void
clawt_daemon_tree_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->guard == NULL)
		return;

	g_signal_handlers_disconnect_by_func(self->guard,
	                                     G_CALLBACK(on_tree_exhausted), self);
	clawt_loop_guard_set_tree_func(self->guard, NULL, NULL, NULL);
}
