/*
 * daemon-replay.c - What happens to a turn a restart cut short
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A delivery is acknowledged when it reaches the agent's socket, and
 * libreclaw's own queue is in memory, so a process that dies mid-turn
 * used to take the turn with it: the person who asked heard nothing,
 * and nothing anywhere said the request had been dropped.  The mailbox
 * now keeps a record of every delivery until its turn ends
 * (clawt_mailbox_turn_open()), marked when the turn acts outside the
 * fleet (clawt_agent_mark_outward()).  This file reads those records
 * back once the process that held them is gone, and settles each one
 * of two ways:
 *
 *   - rerun once: the items go back into the queue and the agent is
 *     handed them again, told it is a rerun and to check what it
 *     already did;
 *   - interrupted: a turn that had already sent, posted or run
 *     something outside the fleet, a turn that was itself a rerun, or
 *     one cut by a stop somebody asked for, ends with a notice in its
 *     room instead -- the daemon's voice, marked so every client draws
 *     it apart.
 *
 * Never both and never twice: the record is deleted in the same
 * transaction that decides, and a rerun carries its count with it.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"
#include "task/clawt-incident.h"

#include <string.h>

/* How much of the message a notice quotes. */
#define REPLAY_QUOTE_CHARS (160)

/*
 * The notice for one turn that will not be resumed.
 *
 * Says which request it was, because the room may have moved on by the
 * time anybody reads it; why it is not being rerun, because "it already
 * sent the email" and "you stopped it" ask for different follow-ups;
 * and what to do, which is ask again after checking.  The quoted text
 * is somebody else's and is escaped and fenced, so a message ending in
 * a forged `[clawtilla]` line cannot speak in this notice's voice.
 */
static gchar *
interrupted_text(
	const gchar     *agent_id,
	ClawtTurnRecord *record
){
	g_autoptr(GString) text = g_string_new(NULL);

	g_string_append_printf(text,
		"[clawtilla] %s's turn was cut short and will not be resumed: %s.",
		agent_id, record->reason != NULL ? record->reason : "it was interrupted");

	if (record->outward != NULL && *record->outward != '\0')
		g_string_append_printf(text,
			" Before it stopped it had already: %s -- so running it again "
			"could do that twice.", record->outward);

	if (record->body != NULL && *record->body != '\0') {
		g_autofree gchar *line = clawt_clip_line(record->body,
			REPLAY_QUOTE_CHARS);
		g_autofree gchar *escaped = clawt_preamble_escape(line);
		g_autofree gchar *fenced = clawt_incident_fence(NULL, escaped);

		g_string_append_printf(text,
			"\n\nIt was answering%s%s%s:\n%s", record->from != NULL
				? " a message from '" : "",
			record->from != NULL ? record->from : "",
			record->from != NULL ? "'" : "", fenced);
	}

	g_string_append(text,
		"\nIf it still needs doing, check what was already done and ask "
		"again.");

	return g_string_free(g_steal_pointer(&text), FALSE);
}

/* Posts one interrupted notice into the turn's room. */
static void
post_interrupted(
	ClawtDaemon     *self,
	const gchar     *agent_id,
	ClawtTurnRecord *record
){
	g_autofree gchar *body = interrupted_text(agent_id, record);
	g_autoptr(ClawtMessage) note = NULL;
	g_autoptr(GError) error = NULL;
	ClawtRoom *room = NULL;

	g_message("%s: a cut turn ends as interrupted (%s)", agent_id,
		record->reason != NULL ? record->reason : "no reason");

	if (self->router == NULL || self->rooms == NULL)
		return;

	/*
	 * The room it ran in, or the operator's direct room with the agent
	 * when it named none or no longer exists -- somebody has to read
	 * this, and that is where they look first.
	 */
	if (record->room != NULL)
		room = clawt_room_manager_get(self->rooms, record->room);
	if (room == NULL)
		room = clawt_room_manager_get_direct(self->rooms, "user", agent_id);
	if (room == NULL)
		return;

	note = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER,
		body);
	clawt_message_set_invites_reply(note, FALSE);
	clawt_message_set_interrupted(note, TRUE);

	if (!clawt_mailbox_router_record(self->router, note, &error))
		g_warning("%s: could not record that a turn was interrupted: %s",
			agent_id, error != NULL ? error->message : "unknown");
}

/*
 * A turn whose task is over is not rerun.  An agent that crashes
 * mid-run has its running tasks failed and their delegators told
 * (docs/orchestration.org, "Incidents") before this runs, so a rerun
 * would do work for a task its delegator already heard had failed, and
 * the answer would arrive for something that no longer exists.
 */
static const gchar *
veto_settled_task(
	ClawtMailbox    *mailbox,
	ClawtTurnRecord *record,
	gpointer         user_data
){
	ClawtDaemon *self = user_data;
	guint i;

	if (self->tasks == NULL)
		return NULL;

	for (i = 0; record->items != NULL && record->items[i] != NULL; i++) {
		g_autoptr(ClawtMailboxItem) item = clawt_mailbox_get(mailbox,
			record->items[i]);
		const gchar *task_id = item != NULL
			? clawt_mailbox_item_get_task_id(item) : NULL;
		ClawtTask *task = task_id != NULL
			? clawt_task_manager_get(self->tasks, task_id) : NULL;

		/*
		 * Finished, by the type's own definition rather than a list of
		 * states written here: tasks survive a restart now, so this
		 * sees every state a stored task can be in.
		 */
		if (task != NULL && clawt_task_is_finished(task))
			return "the task it was working on had already ended";
	}

	return NULL;
}

/*
 * Settles every open turn record of one agent, either way, and acts on
 * the outcome.  The shared half of recover and abandon.
 */
static guint
settle(
	ClawtDaemon *self,
	const gchar *agent_id,
	gboolean     replay,
	const gchar *why
){
	ClawtAgent *agent;
	ClawtMailbox *mailbox;
	g_autoptr(GPtrArray) settled = NULL;
	g_autoptr(GError) error = NULL;
	guint redelivered = 0;
	guint i;

	if (self->agents == NULL || agent_id == NULL)
		return 0;

	agent = clawt_agent_manager_get(self->agents, agent_id);
	mailbox = agent != NULL ? clawt_agent_get_mailbox(agent) : NULL;

	if (mailbox == NULL || clawt_mailbox_turn_count_open(mailbox) == 0)
		return 0;

	settled = clawt_mailbox_turn_recover(mailbox, replay, why,
		veto_settled_task, self, &error);

	if (settled == NULL) {
		/*
		 * Left as they were, which is the safe failure: the records
		 * are still open and the next start asks again.  Rerunning
		 * from a half-read table is how a turn would run twice.
		 */
		g_warning("%s: could not read back the turns a restart cut "
			"short: %s", agent_id,
			error != NULL ? error->message : "unknown");
		return 0;
	}

	for (i = 0; i < settled->len; i++) {
		ClawtTurnRecord *record = g_ptr_array_index(settled, i);

		if (record->redelivered) {
			g_message("%s: a turn cut short by a restart is handed back "
				"once (room %s)", agent_id,
				record->room != NULL ? record->room : "unnamed");
			redelivered++;
		} else {
			post_interrupted(self, agent_id, record);
		}
	}

	/*
	 * Delivered at once when the agent is up -- a restart policy brings
	 * it back before anything else would drain -- and otherwise left
	 * queued, which is the mailbox doing what it is for.
	 */
	if (redelivered > 0 && self->router != NULL &&
		clawt_agent_get_link(agent) != NULL)
		clawt_mailbox_router_drain(self->router, agent_id);

	return redelivered;
}

guint
clawt_daemon_turns_recover(
	ClawtDaemon *self,
	const gchar *agent_id
){
	g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

	return settle(self, agent_id, TRUE, NULL);
}

void
clawt_daemon_turns_abandon(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *why
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	settle(self, agent_id, FALSE, why);
}

void
clawt_daemon_turns_recover_all(
	ClawtDaemon *self
){
	GPtrArray *agents;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->agents == NULL)
		return;

	/*
	 * Before anything starts: a record still open when the daemon comes
	 * up was left by one that died, and its agent's process went with
	 * it.  Read now, a rerun is queued before the agent's link exists
	 * and is the first thing it is handed.
	 */
	/* transfer none: the manager's own list */
	agents = clawt_agent_manager_list(self->agents);

	for (i = 0; agents != NULL && i < agents->len; i++)
		settle(self, clawt_agent_get_id(g_ptr_array_index(agents, i)),
			TRUE, NULL);
}
