/*
 * daemon-rescue.c - First-reply rescue: a turn that has shown nothing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A room's turn starts on its typing edge and is watched until it shows
 * anything: a step (daemon-step.c) or a reply.  Past the agent's
 * `runtime.first_reply_seconds` the daemon publishes `turn.no_reply`,
 * and the clients offer Stop and, where one exists, a fallback model
 * for this turn.  Anything the turn shows afterwards publishes
 * `turn.no_reply_cleared`, so an offer is never left standing over a
 * turn that has since answered -- both ends published, or a client's
 * banner could only ever appear.
 *
 * The limits of "nothing" are in src/chat/clawt-rescue.h and are worth
 * reading before changing anything here.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

static gint64
rescue_now(
	ClawtDaemon *self
){
	/* The turn watch's clock, which is the fake one in a test. */
	return (self->turn_watch != NULL)
		? clawt_turn_watch_now(self->turn_watch) : g_get_monotonic_time();
}

/*
 * How long this agent's turns may be silent before they are offered;
 * 0 when they never are -- including when its steps are not streamed,
 * because then silence is all there ever is until the answer.
 */
static guint
allowance_for(
	const gchar *agent_id,
	gpointer     data
){
	ClawtDaemon *self = data;
	ClawtAgentConfig *config = (self->config != NULL)
		? clawt_config_get_agent(self->config, agent_id) : NULL;
	gint64 seconds;

	if (config == NULL)
		return 0;

	if (!clawt_agent_config_get_boolean(config, "runtime.stream_steps"))
		return 0;

	seconds = clawt_agent_config_get_int(config,
		"runtime.first_reply_seconds");

	return (guint)CLAMP(seconds, 0, (gint64)G_MAXUINT);
}

/* The models this agent's turn can be moved to, comma separated. */
static gchar *
choices_for(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	GPtrArray   **refused
){
	ClawtAgentConfig *config = (self->config != NULL)
		? clawt_config_get_agent(self->config, agent_id) : NULL;
	g_autoptr(GPtrArray) fallbacks = NULL;
	g_autoptr(GPtrArray) choices = NULL;

	if (refused != NULL)
		*refused = NULL;

	if (config == NULL)
		return g_strdup("");

	fallbacks = clawt_agent_config_get_fallbacks(config);
	choices = clawt_rescue_switch_choices(
		clawt_agent_config_get_string(config, "model.provider"),
		clawt_agent_config_get_string(config, "model.account"),
		fallbacks, refused);
	g_ptr_array_add(choices, NULL);

	return g_strjoinv(",", (gchar **)choices->pdata);
}

static void
publish(
	ClawtDaemon *self,
	const gchar *kind,
	const gchar *agent_id,
	const gchar *room_id,
	gint64       seconds
){
	g_autoptr(ClawtEvent) event = NULL;

	if (self->bus == NULL)
		return;

	event = clawt_event_new(kind, agent_id);
	clawt_event_set_detail(event, "room", room_id);

	if (seconds >= 0) {
		g_autofree gchar *choices = choices_for(self, agent_id, NULL);

		clawt_event_set_detail_int(event, "seconds", seconds);
		clawt_event_set_detail(event, "choices", choices);
	}

	clawt_event_bus_publish(self->bus, event);
}

void
clawt_daemon_rescue_begin(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *room_id
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->rescues == NULL || agent_id == NULL)
		return;

	if (allowance_for(agent_id, self) == 0)
		return;

	clawt_rescue_watch_begin(self->rescues, agent_id, room_id,
		rescue_now(self));
}

void
clawt_daemon_rescue_life(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *room_id
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->rescues == NULL || agent_id == NULL)
		return;

	if (clawt_rescue_watch_note_life(self->rescues, agent_id, room_id))
		publish(self, "turn.no_reply_cleared", agent_id, room_id, -1);
}

void
clawt_daemon_rescue_sweep(
	ClawtDaemon *self
){
	g_autoptr(GPtrArray) due = NULL;
	gint64 now;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->rescues == NULL)
		return;

	now = rescue_now(self);
	due = clawt_rescue_watch_collect_due(self->rescues, now, allowance_for,
		self);

	for (i = 0; i < due->len; i++) {
		g_autofree gchar *agent_id = NULL;
		g_autofree gchar *room_id = NULL;

		clawt_rescue_key_split(g_ptr_array_index(due, i), &agent_id,
			&room_id);
		publish(self, "turn.no_reply", agent_id, room_id,
			clawt_rescue_watch_silent_for(self->rescues, agent_id, room_id,
				now));
	}
}

/* ── turn.rescue ─────────────────────────────────────────────────── */

static JsonNode *
list_offers(
	ClawtDaemon *self,
	JsonNode    *request,
	const gchar *agent_filter
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) offered = NULL;
	gint64 now = rescue_now(self);
	guint i;

	offered = (self->rescues != NULL)
		? clawt_rescue_watch_offered(self->rescues, agent_filter)
		: g_ptr_array_new_with_free_func(g_free);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "offers");
	json_builder_begin_array(builder);

	for (i = 0; i < offered->len; i++) {
		g_autofree gchar *agent_id = NULL;
		g_autofree gchar *room_id = NULL;
		g_autofree gchar *choices = NULL;
		g_autoptr(GPtrArray) refused = NULL;
		guint j;

		clawt_rescue_key_split(g_ptr_array_index(offered, i), &agent_id,
			&room_id);
		choices = choices_for(self, agent_id, &refused);

		json_builder_begin_object(builder);
		clawt_daemon_add_string_member(builder, "agent", agent_id);
		clawt_daemon_add_string_member(builder, "room", room_id);
		json_builder_set_member_name(builder, "seconds");
		json_builder_add_int_value(builder, clawt_rescue_watch_silent_for(
			self->rescues, agent_id, room_id, now));
		json_builder_set_member_name(builder, "choices");
		json_builder_begin_array(builder);

		if (choices != NULL && *choices != '\0') {
			g_auto(GStrv) split = g_strsplit(choices, ",", -1);

			for (j = 0; split[j] != NULL; j++)
				json_builder_add_string_value(builder, split[j]);
		}

		json_builder_end_array(builder);

		/* Why the other fallbacks are not offered, so none just vanishes. */
		json_builder_set_member_name(builder, "not_offered");
		json_builder_begin_array(builder);

		for (j = 0; refused != NULL && j < refused->len; j++)
			json_builder_add_string_value(builder,
				g_ptr_array_index(refused, j));

		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

/*
 * Runs the room's message again on @model.  The record of the running
 * turn is handed back first -- while it is still open -- and then the
 * agent is interrupted: the interrupt's settle is what drains the queue,
 * so the message goes out again with the model riding on it, which
 * libreclaw applies when it takes the message off its queue.
 */
static gboolean
switch_model(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	const gchar  *room_id,
	const gchar  *model,
	guint        *out_killed,
	GError      **error
){
	ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);
	ClawtMailbox *mailbox = (agent != NULL) ? clawt_agent_get_mailbox(agent)
		: NULL;
	g_autofree gchar *choices = choices_for(self, agent_id, NULL);
	g_auto(GStrv) allowed = g_strsplit(choices, ",", -1);

	if (model == NULL || *model == '\0' ||
		!g_strv_contains((const gchar * const *)allowed, model)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			"%s cannot be used for one of %s's turns: only its "
			"model.fallbacks entries on its own CLI and login can (%s)",
			model != NULL ? model : "(none)", agent_id,
			*choices != '\0' ? choices : "there are none");
		return FALSE;
	}

	if (mailbox == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
			"%s has no mailbox to run the message from again", agent_id);
		return FALSE;
	}

	if (clawt_mailbox_turn_hand_back(mailbox, room_id, model, error) == 0)
		return FALSE;

	return clawt_daemon_interrupt_agent(self, agent_id, out_killed, error);
}

JsonNode *
clawt_daemon_handle_rescue(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *action;
	const gchar *agent_id;
	const gchar *room_id;
	guint killed = 0;
	gboolean ok;

	if (g_strcmp0(kind, "turn.rescue") != 0) {
		*handled = FALSE;
		return NULL;
	}

	*handled = TRUE;
	action = clawt_ipc_payload_string(payload, "action");
	agent_id = clawt_ipc_payload_string(payload, "agent");
	room_id = clawt_ipc_payload_string(payload, "room");

	if (action == NULL || g_strcmp0(action, "list") == 0)
		return list_offers(self, request, agent_id);

	if (agent_id == NULL ||
		clawt_agent_manager_get(self->agents, agent_id) == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			"no such agent");

	if (g_strcmp0(action, "stop") == 0) {
		ok = clawt_daemon_interrupt_agent(self, agent_id, &killed, &error);
	} else if (g_strcmp0(action, "switch") == 0) {
		if (room_id == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
				"switching needs the room whose turn runs again");

		ok = switch_model(self, agent_id, room_id,
			clawt_ipc_payload_string(payload, "model"), &killed, &error);
	} else {
		return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			"action is list, stop or switch");
	}

	if (!ok)
		return clawt_ipc_error_new(request, error->code, error->message);

	/* Whatever it was, the offer is answered. */
	clawt_daemon_rescue_life(self, agent_id, room_id);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "done");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "killed");
	json_builder_add_int_value(builder, (gint64)killed);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}
