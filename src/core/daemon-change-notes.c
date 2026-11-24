/*
 * daemon-change-notes.c - Saying in an agent's chat that it was changed
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * - Every clawt_config_save() reaches here through the save observer,
 *   with the text just written.  The configuration as it was before is
 *   kept as JSON (the snapshot); clawt_change_diff() compares the two,
 *   and each agent with changes gets one note in its chat with the
 *   operator.
 * - Who made the change is whoever pushed the actor: a tool call pushes
 *   its agent, an IPC request pushes `user` (or the agent a CLI run in
 *   its shell names, as attribution only).  The same actor is the
 *   revision history's author, so the two never name different people.
 * - A reload that took up a hand edit rebases the snapshot without a
 *   note, or the next save would attribute the edit to whoever saved.
 * - The note is recorded, never delivered: a change starting a turn in
 *   the agent it changed would invite it to react, and a reaction that
 *   changes something back is a loop nobody asked for.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

/* Reads the file as it is now into the snapshot. */
void
clawt_daemon_change_notes_rebase(
	ClawtDaemon *self
){
	g_autofree gchar *text = NULL;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->config_path != NULL)
		(void)g_file_get_contents(self->config_path, &text, NULL, NULL);

	g_clear_pointer(&self->change_snapshot, json_node_unref);
	self->change_snapshot = clawt_change_config_to_json(text);
}

/* Whether @actor is an agent of this fleet, whose chat can take a note. */
static gboolean
actor_is_local_agent(
	ClawtDaemon *self,
	const gchar *actor
){
	return actor != NULL && self->agents != NULL &&
	       clawt_agent_manager_get(self->agents, actor) != NULL;
}

/* Records one note into the operator's chat with @agent. */
static void
record_note(
	ClawtDaemon *self,
	const gchar *agent,
	GPtrArray   *changes,
	const gchar *actor,
	const gchar *via
){
	g_autofree gchar *body = NULL;
	g_autoptr(ClawtMessage) note = NULL;
	g_autoptr(GError) error = NULL;
	ClawtRoom *room;
	const ClawtChange *first;

	if (changes->len == 0 || self->rooms == NULL || self->router == NULL)
		return;

	room = clawt_room_manager_get_direct(self->rooms, "user", agent);
	if (room == NULL)
		return;

	first = g_ptr_array_index(changes, 0);
	body = clawt_change_note_body(changes, actor, via);
	note = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER,
	                         body);
	clawt_message_set_invites_reply(note, FALSE);
	clawt_message_set_change(note,
		clawt_enum_to_nick(CLAWT_TYPE_CHANGE_KIND, (gint)first->kind),
		actor);

	if (!clawt_mailbox_router_record(self->router, note, &error))
		g_warning("%s: a change note was not recorded: %s", agent,
		          error != NULL ? error->message : "unknown");
}

void
clawt_daemon_change_notes_saved(
	ClawtDaemon *self,
	const gchar *text
){
	g_autoptr(JsonNode) after = NULL;
	g_autoptr(GPtrArray) changes = NULL;
	g_autoptr(GPtrArray) group = NULL;
	const gchar *actor = clawt_change_actor_get();
	const gchar *via = clawt_change_actor_get_via();
	const gchar *mode;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	after = clawt_change_config_to_json(text);

	if (self->change_snapshot == NULL) {
		self->change_snapshot = g_steal_pointer(&after);
		return;
	}

	changes = clawt_change_diff(self->change_snapshot, after);
	g_clear_pointer(&self->change_snapshot, json_node_unref);
	self->change_snapshot = g_steal_pointer(&after);

	if (changes == NULL || changes->len == 0)
		return;

	mode = clawt_enum_to_nick(CLAWT_TYPE_CONFIG_NOTES,
		clawt_config_get_enum(self->config, "orchestration.config_notes"));

	/*
	 * One note per agent: the changes arrive grouped by agent, so a run
	 * ends where the agent changes.  A removed agent has no chat left to
	 * be told in, so its removal goes to the chat of the agent that
	 * removed it -- when an agent did.
	 */
	group = g_ptr_array_new();

	for (i = 0; i <= changes->len; i++) {
		ClawtChange *change = (i < changes->len)
			? g_ptr_array_index(changes, i) : NULL;

		if (group->len > 0 &&
		    (change == NULL ||
		     g_strcmp0(change->agent,
		               ((ClawtChange *)g_ptr_array_index(group, 0))->agent)
		         != 0)) {
			const gchar *agent =
				((ClawtChange *)g_ptr_array_index(group, 0))->agent;

			if (clawt_change_should_note(mode, actor, agent))
				record_note(self, agent, group, actor, via);
			g_ptr_array_set_size(group, 0);
		}

		if (change == NULL)
			break;

		if (change->kind == CLAWT_CHANGE_REMOVED) {
			g_autoptr(GPtrArray) alone = g_ptr_array_new();

			g_ptr_array_add(alone, change);
			if (actor_is_local_agent(self, actor) &&
			    clawt_change_should_note(mode, actor, actor))
				record_note(self, actor, alone, actor, via);
			continue;
		}

		g_ptr_array_add(group, change);
	}
}

void
clawt_daemon_change_notes_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	g_clear_pointer(&self->change_snapshot, json_node_unref);
}
