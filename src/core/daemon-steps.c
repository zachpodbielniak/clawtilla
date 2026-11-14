/*
 * daemon-steps.c - Saved steps: a desktop task replayed with no model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The recorder is fed from `computer.control` (daemon-screen.c), the
 * gate every acting desktop call passes, and from the turn's edges
 * (daemon-turn.c).  This file is the client surface -- `steps.last`,
 * `steps.save`, `steps.list`, `steps.delete`, `steps.replay` -- and the
 * replay itself, which is conservative in every direction it can be:
 * see src/computer/clawt-saved-steps.h before loosening any of it.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* How long a replay waits for a fresh picture of the screen. */
#define REPLAY_FRAME_WAIT_USEC (15 * G_USEC_PER_SEC)

/* How often it looks for that picture. */
#define REPLAY_FRAME_POLL_MS (250)

static gchar *
steps_dir(
	ClawtDaemon *self
){
	return g_build_filename(self->state_dir, "saved-steps", NULL);
}

/* SHA-256 of a frame file, or NULL when there is none to read. */
static gchar *
hash_frame(
	const gchar *path
){
	g_autoptr(GBytes) bytes = NULL;
	gchar *contents = NULL;
	gsize length = 0;

	if (path == NULL || !g_file_get_contents(path, &contents, &length, NULL))
		return NULL;

	bytes = g_bytes_new_take(contents, length);

	return clawt_screen_hash_bytes(bytes);
}

/* ── Recording, from the gate and the turn's edges ───────────────── */

void
clawt_daemon_steps_turn_began(
	ClawtDaemon *self,
	const gchar *agent_id
){
	ClawtComputer *computer;
	ClawtAgent *agent;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->step_recorder == NULL || agent_id == NULL)
		return;

	clawt_step_recorder_begin_turn(self->step_recorder, agent_id,
		g_get_real_time());

	/*
	 * A picture now, so the first step has one from before it to
	 * checkpoint against.  Only where there is a screen, and through the
	 * observer's own path, so its minimum gap and one-at-a-time rule
	 * still apply: one grab per turn, not per call.
	 */
	agent = (self->agents != NULL)
		? clawt_agent_manager_get(self->agents, agent_id) : NULL;
	computer = (agent != NULL) ? clawt_agent_get_computer(agent) : NULL;

	if (computer != NULL && CLAWT_IS_OBSERVABLE(computer) &&
		self->observer != NULL)
		clawt_observer_refresh(self->observer, agent_id);
}

void
clawt_daemon_steps_turn_ended(
	ClawtDaemon *self,
	const gchar *agent_id
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->step_recorder == NULL || agent_id == NULL)
		return;

	if (clawt_step_recorder_end_turn(self->step_recorder, agent_id,
			g_get_real_time()) && self->bus != NULL) {
		g_autoptr(ClawtEvent) event = clawt_event_new("steps.changed",
			agent_id);

		clawt_event_bus_publish(self->bus, event);
	}
}

void
clawt_daemon_steps_note_call(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *tool,
	JsonObject  *arguments
){
	g_autofree gchar *checkpoint = NULL;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->step_recorder == NULL || agent_id == NULL)
		return;

	/*
	 * The checkpoint is the last picture taken *after* the turn began
	 * and before this, its first acting call.  An older one could show a
	 * screen the turn had already changed by other means; none at all
	 * makes the run unsaveable, which the recorder says.
	 */
	if (clawt_step_recorder_wants_checkpoint(self->step_recorder, agent_id) &&
		self->observer != NULL) {
		gint64 began = clawt_step_recorder_get_began(self->step_recorder,
			agent_id);
		gint64 stamp = clawt_observer_get_frame_stamp(self->observer,
			agent_id);

		if (began > 0 && stamp >= began)
			checkpoint = hash_frame(clawt_observer_get_frame_path(
				self->observer, agent_id));
	}

	clawt_step_recorder_note_call(self->step_recorder, agent_id, tool,
		arguments, checkpoint);
}

/* ── Replay ──────────────────────────────────────────────────────── */

typedef struct {
	ClawtDaemon *daemon;
	gchar       *agent_id;
	gchar       *name;
	gchar       *checkpoint;
	GPtrArray   *events;
	guint        next;
	gint64       asked_at;     /* wall clock, when the picture was asked */
	gint64       gave_up_at;   /* monotonic */
	GSource     *poll;
} ReplayJob;

static void
replay_free(
	ReplayJob *job
){
	if (job->poll != NULL) {
		g_source_destroy(job->poll);
		g_source_unref(job->poll);
	}

	g_clear_object(&job->daemon);
	g_free(job->agent_id);
	g_free(job->name);
	g_free(job->checkpoint);
	g_clear_pointer(&job->events, g_ptr_array_unref);
	g_free(job);
}

/* The end of a replay, either way, said once. */
static void
replay_finish(
	ReplayJob   *job,
	const gchar *error
){
	ClawtDaemon *self = job->daemon;

	if (self->bus != NULL) {
		g_autoptr(ClawtEvent) event = clawt_event_new("steps.replayed",
			job->agent_id);

		clawt_event_set_detail(event, "name", job->name);
		clawt_event_set_detail(event, "ok", error == NULL ? "true" : "false");
		clawt_event_set_detail_int(event, "done", (gint64)job->next);
		clawt_event_set_detail_int(event, "steps", (gint64)job->events->len);

		if (error != NULL)
			clawt_event_set_detail(event, "error", error);

		clawt_event_bus_publish(self->bus, event);
	}

	if (error != NULL)
		g_message("%s: saved steps '%s' stopped after %u of %u: %s",
			job->agent_id, job->name, job->next, job->events->len, error);

	if (self->replays != NULL)
		g_hash_table_remove(self->replays, job->agent_id);

	replay_free(job);
}

static void send_next(ReplayJob *job);

static void
on_step_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	ReplayJob *job = user_data;
	g_autoptr(GError) error = NULL;

	if (!clawt_observer_send_input_finish(CLAWT_OBSERVER(source), result,
			&error)) {
		g_autofree gchar *why = g_strdup_printf("step %u failed: %s",
			job->next + 1, error->message);

		replay_finish(job, why);
		return;
	}

	job->next++;
	send_next(job);
}

static void
send_next(
	ReplayJob *job
){
	ClawtDaemon *self = job->daemon;
	ClawtAgent *agent;
	ClawtComputer *computer;

	if (self->observer == NULL || self->takeover == NULL ||
		self->agents == NULL) {
		replay_finish(job, "the daemon stopped");
		return;
	}

	if (job->next >= job->events->len) {
		clawt_observer_refresh(self->observer, job->agent_id);
		replay_finish(job, NULL);
		return;
	}

	/*
	 * Checked before every step, not only the first: releasing the
	 * screen is how a person says "stop", and the agent may act again
	 * the moment it is released.
	 */
	if (!clawt_takeover_is_held(self->takeover, job->agent_id)) {
		replay_finish(job, "the screen was released, so the replay stopped "
			"before the agent could act on it too");
		return;
	}

	agent = clawt_agent_manager_get(self->agents, job->agent_id);
	computer = (agent != NULL) ? clawt_agent_get_computer(agent) : NULL;

	if (computer == NULL || !CLAWT_IS_OBSERVABLE(computer)) {
		replay_finish(job, "the agent no longer has a screen");
		return;
	}

	clawt_observer_send_input_async(self->observer, job->agent_id, computer,
		g_ptr_array_index(job->events, job->next), on_step_sent, job);
}

/*
 * Waits for a picture taken after the replay asked, then compares it
 * with the checkpoint.  Polled rather than signalled: the observer has
 * no per-frame callback, and a quarter-second look at a stamp costs
 * nothing.
 */
static gboolean
on_frame_poll(
	gpointer user_data
){
	ReplayJob *job = user_data;
	ClawtDaemon *self = job->daemon;
	g_autofree gchar *hash = NULL;
	gint64 stamp;

	/* The daemon stopped under the replay: nothing left to drive. */
	if (self->observer == NULL || self->takeover == NULL) {
		g_clear_pointer(&job->poll, g_source_unref);
		replay_finish(job, "the daemon stopped");
		return G_SOURCE_REMOVE;
	}

	stamp = clawt_observer_get_frame_stamp(self->observer, job->agent_id);

	if (stamp < job->asked_at) {
		if (g_get_monotonic_time() < job->gave_up_at) {
			clawt_observer_refresh(self->observer, job->agent_id);
			return G_SOURCE_CONTINUE;
		}

		g_clear_pointer(&job->poll, g_source_unref);
		replay_finish(job, "no fresh picture of the screen arrived to check "
			"against the saved checkpoint; keep the Screen view open while "
			"replaying, since nothing is captured unless somebody is "
			"looking");
		return G_SOURCE_REMOVE;
	}

	g_clear_pointer(&job->poll, g_source_unref);
	hash = hash_frame(clawt_observer_get_frame_path(self->observer,
		job->agent_id));

	if (g_strcmp0(hash, job->checkpoint) != 0) {
		replay_finish(job, "the screen does not look the way it did when "
			"these steps were saved, so nothing was sent; put it back as it "
			"was (the same windows, nothing moved) and try again");
		return G_SOURCE_REMOVE;
	}

	send_next(job);

	return G_SOURCE_REMOVE;
}

static JsonNode *
start_replay(
	ClawtDaemon *self,
	JsonNode    *request,
	const gchar *agent_id,
	const gchar *name
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = steps_dir(self);
	g_autofree gchar *checkpoint = NULL;
	g_autoptr(GPtrArray) events = NULL;
	ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);
	ClawtComputer *computer = (agent != NULL)
		? clawt_agent_get_computer(agent) : NULL;
	ReplayJob *job;

	if (computer == NULL || !CLAWT_IS_OBSERVABLE(computer) ||
		self->observer == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
			"that agent has no screen to replay anything on");

	/*
	 * The claim gate: a replay is input a person sends, exactly as
	 * computer.input is, and only while they hold the screen -- which is
	 * also what keeps the agent's own acting calls refused meanwhile.
	 */
	if (!clawt_takeover_is_held(self->takeover, agent_id))
		return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			"take the screen first: a replay drives it with no model, and "
			"only while you hold it, so the agent cannot act on it at the "
			"same time");

	if (self->replays != NULL &&
		g_hash_table_contains(self->replays, agent_id))
		return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			"a replay is already running on that screen");

	events = clawt_saved_steps_get(dir, agent_id, name, &checkpoint, &error);

	if (events == NULL)
		return clawt_ipc_error_new(request, error->code, error->message);

	job = g_new0(ReplayJob, 1);
	job->daemon = g_object_ref(self);
	job->agent_id = g_strdup(agent_id);
	job->name = g_strdup(name);
	job->checkpoint = g_steal_pointer(&checkpoint);
	job->events = g_steal_pointer(&events);
	job->asked_at = g_get_real_time();
	job->gave_up_at = g_get_monotonic_time() + REPLAY_FRAME_WAIT_USEC;

	g_hash_table_add(self->replays, g_strdup(agent_id));

	/* A fresh picture first: nothing is sent until it matches. */
	clawt_observer_refresh(self->observer, agent_id);

	job->poll = g_timeout_source_new(REPLAY_FRAME_POLL_MS);
	g_source_set_callback(job->poll, on_frame_poll, job, NULL);
	g_source_attach(job->poll, self->main_context);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "started");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "steps");
	json_builder_add_int_value(builder, (gint64)job->events->len);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

/* ── The client surface ──────────────────────────────────────────── */

/* A member YAML may have handed back as the string "42". */
static gint64
entry_int(
	JsonObject  *entry,
	const gchar *key
){
	JsonNode *node;

	if (entry == NULL || !json_object_has_member(entry, key))
		return 0;

	node = json_object_get_member(entry, key);

	if (!JSON_NODE_HOLDS_VALUE(node))
		return 0;

	if (json_node_get_value_type(node) == G_TYPE_STRING)
		return g_ascii_strtoll(json_node_get_string(node), NULL, 10);

	return json_node_get_int(node);
}

static void
add_steps(
	JsonBuilder *builder,
	GPtrArray   *events
){
	guint i;

	json_builder_set_member_name(builder, "steps");
	json_builder_begin_array(builder);

	for (i = 0; events != NULL && i < events->len; i++) {
		g_autofree gchar *line = clawt_saved_step_describe(
			g_ptr_array_index(events, i));

		json_builder_add_string_value(builder, line);
	}

	json_builder_end_array(builder);
}

static void
publish_changed(
	ClawtDaemon *self,
	const gchar *agent_id
){
	g_autoptr(ClawtEvent) event = NULL;

	if (self->bus == NULL)
		return;

	event = clawt_event_new("steps.changed", agent_id);
	clawt_event_bus_publish(self->bus, event);
}

JsonNode *
clawt_daemon_handle_steps(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	const gchar *agent_id;
	const gchar *name;

	if (!g_str_has_prefix(kind, "steps.")) {
		*handled = FALSE;
		return NULL;
	}

	*handled = TRUE;
	agent_id = clawt_ipc_payload_string(payload, "agent");
	name = clawt_ipc_payload_string(payload, "name");

	if (agent_id == NULL ||
		clawt_agent_manager_get(self->agents, agent_id) == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			"no such agent");

	dir = steps_dir(self);
	builder = json_builder_new();

	/* The last finished turn's desktop input, and whether it can be saved. */
	if (g_strcmp0(kind, "steps.last") == 0) {
		const ClawtStepRun *run = (self->step_recorder != NULL)
			? clawt_step_recorder_get_last(self->step_recorder, agent_id)
			: NULL;

		json_builder_begin_object(builder);

		if (run != NULL) {
			json_builder_set_member_name(builder, "finished_at");
			json_builder_add_int_value(builder, run->finished_at);
			json_builder_set_member_name(builder, "calls");
			json_builder_add_int_value(builder, run->calls);
			json_builder_set_member_name(builder, "saveable");
			json_builder_add_boolean_value(builder,
				run->saveable && run->checkpoint != NULL &&
				run->events->len > 0);
			clawt_daemon_add_string_member(builder, "why_not", run->why_not);
			add_steps(builder, run->events);
		}

		json_builder_end_object(builder);
		return clawt_ipc_response_new(request,
			json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "steps.save") == 0) {
		const ClawtStepRun *run = (self->step_recorder != NULL)
			? clawt_step_recorder_get_last(self->step_recorder, agent_id)
			: NULL;

		if (run == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
				"that agent has not finished a turn on its screen since the "
				"daemon started");

		if (!clawt_saved_steps_save(dir, agent_id, name, run, &error))
			return clawt_ipc_error_new(request, error->code, error->message);

		publish_changed(self, agent_id);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "saved");
		json_builder_add_boolean_value(builder, TRUE);
		json_builder_end_object(builder);
		return clawt_ipc_response_new(request,
			json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "steps.list") == 0) {
		g_autoptr(JsonNode) all = clawt_saved_steps_list(dir, agent_id,
			&error);
		JsonArray *saved;
		guint i;

		if (all == NULL)
			return clawt_ipc_error_new(request, error->code, error->message);

		saved = json_node_get_array(all);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "replaying");
		json_builder_add_boolean_value(builder, self->replays != NULL &&
			g_hash_table_contains(self->replays, agent_id));
		json_builder_set_member_name(builder, "saved");
		json_builder_begin_array(builder);

		for (i = 0; i < json_array_get_length(saved); i++) {
			JsonObject *entry = json_array_get_object_element(saved, i);
			const gchar *entry_name = json_object_has_member(entry, "name")
				? json_object_get_string_member(entry, "name") : NULL;
			g_autofree gchar *checkpoint = NULL;
			g_autoptr(GPtrArray) events = clawt_saved_steps_get(dir,
				agent_id, entry_name, &checkpoint, NULL);

			json_builder_begin_object(builder);
			clawt_daemon_add_string_member(builder, "name", entry_name);
			json_builder_set_member_name(builder, "created_at");
			json_builder_add_int_value(builder, entry_int(entry, "created_at"));
			add_steps(builder, events);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
		json_builder_end_object(builder);
		return clawt_ipc_response_new(request,
			json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "steps.delete") == 0) {
		if (!clawt_saved_steps_delete(dir, agent_id, name, &error))
			return clawt_ipc_error_new(request, error->code, error->message);

		publish_changed(self, agent_id);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "deleted");
		json_builder_add_boolean_value(builder, TRUE);
		json_builder_end_object(builder);
		return clawt_ipc_response_new(request,
			json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "steps.replay") == 0)
		return start_replay(self, request, agent_id, name);

	return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
		"steps.* is last, save, list, delete or replay");
}
