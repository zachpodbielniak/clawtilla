/*
 * clawt-saved-steps.c - A desktop task's input, saved, and replayed with
 * no model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-saved-steps.h"

#include <yaml-glib.h>
#include <string.h>

/* How much typed text a step's description quotes. */
#define DESCRIBE_TEXT_CHARS (40)

/* ── A run ───────────────────────────────────────────────────────── */

G_DEFINE_BOXED_TYPE(ClawtStepRun, clawt_step_run, clawt_step_run_copy,
                    clawt_step_run_free)

ClawtStepRun *
clawt_step_run_new(
	void
){
	ClawtStepRun *self = g_new0(ClawtStepRun, 1);

	self->events = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_input_event_free);
	self->saveable = TRUE;

	return self;
}

ClawtStepRun *
clawt_step_run_copy(
	const ClawtStepRun *self
){
	ClawtStepRun *copy;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	copy = clawt_step_run_new();

	for (i = 0; i < self->events->len; i++)
		g_ptr_array_add(copy->events, clawt_input_event_copy(
			g_ptr_array_index(self->events, i)));

	copy->calls = self->calls;
	copy->saveable = self->saveable;
	copy->why_not = g_strdup(self->why_not);
	copy->checkpoint = g_strdup(self->checkpoint);
	copy->finished_at = self->finished_at;

	return copy;
}

void
clawt_step_run_free(
	ClawtStepRun *self
){
	if (self == NULL)
		return;

	g_clear_pointer(&self->events, g_ptr_array_unref);
	g_free(self->why_not);
	g_free(self->checkpoint);
	g_free(self);
}

static void
run_refuse(
	ClawtStepRun *run,
	const gchar  *why
){
	/* The first reason is the one worth reading: later ones follow from it. */
	if (!run->saveable)
		return;

	run->saveable = FALSE;
	run->why_not = g_strdup(why);
}

/* ── One call as one step ────────────────────────────────────────── */

static const gchar *
arg_string(
	JsonObject  *arguments,
	const gchar *key
){
	JsonNode *node;

	if (arguments == NULL || !json_object_has_member(arguments, key))
		return NULL;

	node = json_object_get_member(arguments, key);

	if (!JSON_NODE_HOLDS_VALUE(node) ||
		json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return json_node_get_string(node);
}

/* An integer argument, or FALSE when it is absent or not a number. */
static gboolean
arg_int(
	JsonObject  *arguments,
	const gchar *key,
	gint        *out
){
	JsonNode *node;
	GType type;

	if (arguments == NULL || !json_object_has_member(arguments, key))
		return FALSE;

	node = json_object_get_member(arguments, key);

	if (!JSON_NODE_HOLDS_VALUE(node))
		return FALSE;

	type = json_node_get_value_type(node);

	if (type == G_TYPE_INT64) {
		*out = (gint)json_node_get_int(node);
		return TRUE;
	}

	if (type == G_TYPE_DOUBLE) {
		*out = (gint)json_node_get_double(node);
		return TRUE;
	}

	return FALSE;
}

static guint
button_of(
	JsonObject *arguments
){
	const gchar *name = arg_string(arguments, "button");
	gint number = 0;

	if (g_strcmp0(name, "right") == 0)
		return 3;
	if (g_strcmp0(name, "middle") == 0)
		return 2;
	if (arg_int(arguments, "button", &number) && number >= 1 && number <= 3)
		return (guint)number;

	return 1;
}

ClawtInputEvent *
clawt_saved_step_from_call(
	const gchar  *tool,
	JsonObject   *arguments,
	gint         *pointer_x,
	gint         *pointer_y,
	gchar       **why_not
){
	g_autoptr(ClawtInputEvent) event = NULL;
	const gchar *text;
	gint x = 0;
	gint y = 0;

	g_return_val_if_fail(pointer_x != NULL, NULL);
	g_return_val_if_fail(pointer_y != NULL, NULL);

	if (why_not != NULL)
		*why_not = NULL;

	/* ── Keys ── */
	if (g_strcmp0(tool, "send_key") == 0 || g_strcmp0(tool, "key_press") == 0 ||
		g_strcmp0(tool, "key_combo") == 0) {
		text = arg_string(arguments, "key");
		if (text == NULL)
			text = arg_string(arguments, "combo");
		if (text == NULL)
			text = arg_string(arguments, "keys");

		if (text != NULL && *text != '\0') {
			event = clawt_input_event_new(CLAWT_INPUT_KEY);
			clawt_input_event_set_text(event, text);
		}
	}

	/* ── Text ── */
	else if (g_strcmp0(tool, "send_text") == 0 ||
		g_strcmp0(tool, "type_text") == 0) {
		text = arg_string(arguments, "text");

		if (text != NULL && *text != '\0') {
			event = clawt_input_event_new(CLAWT_INPUT_TEXT);
			clawt_input_event_set_text(event, text);
		}
	}

	/* ── The pointer ── */
	else if (g_strcmp0(tool, "send_mouse_move") == 0 ||
		g_strcmp0(tool, "mouse_move") == 0) {
		if (arg_int(arguments, "x", &x) && arg_int(arguments, "y", &y)) {
			event = clawt_input_event_new(CLAWT_INPUT_MOVE);
			event->x = x;
			event->y = y;
			*pointer_x = x;
			*pointer_y = y;
		}
	}

	else if (g_strcmp0(tool, "mouse_click") == 0) {
		if (arg_int(arguments, "x", &x) && arg_int(arguments, "y", &y)) {
			event = clawt_input_event_new(CLAWT_INPUT_CLICK);
			event->x = x;
			event->y = y;
			event->button = button_of(arguments);
			*pointer_x = x;
			*pointer_y = y;
		}
	}

	/*
	 * gowl's click is "where the pointer is", so it is a step only when
	 * the run knows where that is -- a click after a move it recorded.
	 * Anything that only clicks a press or a release is not a click.
	 */
	else if (g_strcmp0(tool, "send_mouse") == 0) {
		const gchar *action = arg_string(arguments, "action");

		if ((action == NULL || g_strcmp0(action, "click") == 0) &&
			*pointer_x >= 0 && *pointer_y >= 0) {
			event = clawt_input_event_new(CLAWT_INPUT_CLICK);
			event->x = *pointer_x;
			event->y = *pointer_y;
			event->button = button_of(arguments);
		}
	}

	else if (g_strcmp0(tool, "send_scroll") == 0 ||
		g_strcmp0(tool, "mouse_scroll") == 0) {
		const gchar *direction = arg_string(arguments, "direction");

		if (direction != NULL) {
			event = clawt_input_event_new(CLAWT_INPUT_SCROLL);
			event->x = MAX(*pointer_x, 0);
			event->y = MAX(*pointer_y, 0);

			if (g_strcmp0(direction, "up") == 0)
				event->dy = -1;
			else if (g_strcmp0(direction, "down") == 0)
				event->dy = 1;
			else if (g_strcmp0(direction, "left") == 0)
				event->dx = -1;
			else if (g_strcmp0(direction, "right") == 0)
				event->dx = 1;
			else
				g_clear_pointer(&event, clawt_input_event_free);
		}
	}

	if (event == NULL && why_not != NULL)
		*why_not = g_strdup_printf("it called %s%s, which cannot be "
			"repeated exactly without a model", tool != NULL ? tool : "a tool",
			(arguments == NULL) ? " with no arguments clawtilla could see"
				: "");

	return g_steal_pointer(&event);
}

gchar *
clawt_saved_step_describe(
	ClawtInputEvent *event
){
	g_autofree gchar *redacted = NULL;
	g_autofree gchar *clipped = NULL;

	g_return_val_if_fail(event != NULL, NULL);

	switch (event->kind) {
	case CLAWT_INPUT_KEY:
		return g_strdup_printf("Press %s",
			event->text != NULL ? event->text : "?");

	case CLAWT_INPUT_TEXT:
		redacted = clawt_redact_secrets(event->text != NULL ? event->text
			: "");
		clipped = clawt_clip_line(redacted, DESCRIBE_TEXT_CHARS);
		return g_strdup_printf("Type \342\200\234%s\342\200\235", clipped);

	case CLAWT_INPUT_CLICK:
		return g_strdup_printf("%s at %d, %d",
			event->button == 3 ? "Right-click"
				: event->button == 2 ? "Middle-click" : "Click",
			event->x, event->y);

	case CLAWT_INPUT_MOVE:
		return g_strdup_printf("Move the pointer to %d, %d", event->x,
			event->y);

	case CLAWT_INPUT_SCROLL:
		return g_strdup_printf("Scroll %s",
			event->dy < 0 ? "up" : event->dy > 0 ? "down"
				: event->dx < 0 ? "left" : "right");
	}

	return g_strdup("A step");
}

JsonNode *
clawt_saved_step_to_json(
	ClawtInputEvent *event
){
	g_autoptr(JsonBuilder) builder = json_builder_new();

	g_return_val_if_fail(event != NULL, NULL);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder,
		clawt_enum_to_nick(CLAWT_TYPE_INPUT_KIND, (gint)event->kind));

	if (event->text != NULL) {
		json_builder_set_member_name(builder, "text");
		json_builder_add_string_value(builder, event->text);
	}

	json_builder_set_member_name(builder, "x");
	json_builder_add_int_value(builder, event->x);
	json_builder_set_member_name(builder, "y");
	json_builder_add_int_value(builder, event->y);
	json_builder_set_member_name(builder, "button");
	json_builder_add_int_value(builder, event->button);
	json_builder_set_member_name(builder, "dx");
	json_builder_add_int_value(builder, (gint64)event->dx);
	json_builder_set_member_name(builder, "dy");
	json_builder_add_int_value(builder, (gint64)event->dy);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* A member that may have come back from YAML as a string. */
static gint64
member_int(
	JsonObject  *object,
	const gchar *key
){
	JsonNode *node;

	if (!json_object_has_member(object, key))
		return 0;

	node = json_object_get_member(object, key);

	if (!JSON_NODE_HOLDS_VALUE(node))
		return 0;

	if (json_node_get_value_type(node) == G_TYPE_STRING)
		return g_ascii_strtoll(json_node_get_string(node), NULL, 10);

	return json_node_get_int(node);
}

ClawtInputEvent *
clawt_saved_step_from_json(
	JsonObject *object
){
	ClawtInputEvent *event;
	gint kind = 0;

	if (object == NULL || !json_object_has_member(object, "kind") ||
		!clawt_enum_from_nick(CLAWT_TYPE_INPUT_KIND,
			json_object_get_string_member(object, "kind"), &kind))
		return NULL;

	event = clawt_input_event_new((ClawtInputKind)kind);

	if (json_object_has_member(object, "text"))
		clawt_input_event_set_text(event,
			json_object_get_string_member(object, "text"));

	event->x = (gint)member_int(object, "x");
	event->y = (gint)member_int(object, "y");
	event->button = (guint)member_int(object, "button");
	event->dx = (gdouble)member_int(object, "dx");
	event->dy = (gdouble)member_int(object, "dy");

	return event;
}

/* ── The recorder ────────────────────────────────────────────────── */

typedef struct {
	ClawtStepRun *running;     /* nullable */
	gint64        began;
	gint          pointer_x;
	gint          pointer_y;
	ClawtStepRun *last;        /* nullable */
} AgentRuns;

static void
agent_runs_free(
	gpointer data
){
	AgentRuns *runs = data;

	clawt_step_run_free(runs->running);
	clawt_step_run_free(runs->last);
	g_free(runs);
}

struct _ClawtStepRecorder {
	GObject parent_instance;

	GHashTable *agents;   /* agent id -> AgentRuns* */
};

G_DEFINE_FINAL_TYPE(ClawtStepRecorder, clawt_step_recorder, G_TYPE_OBJECT)

static AgentRuns *
runs_for(
	ClawtStepRecorder *self,
	const gchar       *agent_id
){
	AgentRuns *runs = g_hash_table_lookup(self->agents, agent_id);

	if (runs == NULL) {
		runs = g_new0(AgentRuns, 1);
		g_hash_table_insert(self->agents, g_strdup(agent_id), runs);
	}

	return runs;
}

ClawtStepRecorder *
clawt_step_recorder_new(
	void
){
	return g_object_new(CLAWT_TYPE_STEP_RECORDER, NULL);
}

void
clawt_step_recorder_begin_turn(
	ClawtStepRecorder *self,
	const gchar       *agent_id,
	gint64             now_usec
){
	AgentRuns *runs;

	g_return_if_fail(CLAWT_IS_STEP_RECORDER(self));

	if (agent_id == NULL)
		return;

	runs = runs_for(self, agent_id);
	clawt_step_run_free(runs->running);
	runs->running = clawt_step_run_new();
	runs->began = now_usec;
	runs->pointer_x = -1;
	runs->pointer_y = -1;
}

gint64
clawt_step_recorder_get_began(
	ClawtStepRecorder *self,
	const gchar       *agent_id
){
	AgentRuns *runs;

	g_return_val_if_fail(CLAWT_IS_STEP_RECORDER(self), 0);

	runs = (agent_id != NULL) ? g_hash_table_lookup(self->agents, agent_id)
		: NULL;

	return (runs != NULL && runs->running != NULL) ? runs->began : 0;
}

gboolean
clawt_step_recorder_wants_checkpoint(
	ClawtStepRecorder *self,
	const gchar       *agent_id
){
	AgentRuns *runs;

	g_return_val_if_fail(CLAWT_IS_STEP_RECORDER(self), FALSE);

	runs = (agent_id != NULL) ? g_hash_table_lookup(self->agents, agent_id)
		: NULL;

	return runs != NULL && runs->running != NULL &&
		runs->running->calls == 0;
}

void
clawt_step_recorder_note_call(
	ClawtStepRecorder *self,
	const gchar       *agent_id,
	const gchar       *tool,
	JsonObject        *arguments,
	const gchar       *checkpoint
){
	AgentRuns *runs;
	ClawtStepRun *run;
	g_autofree gchar *why = NULL;
	ClawtInputEvent *event;

	g_return_if_fail(CLAWT_IS_STEP_RECORDER(self));

	if (agent_id == NULL)
		return;

	runs = runs_for(self, agent_id);

	/* A call outside any turn the daemon saw begin: start one for it. */
	if (runs->running == NULL)
		clawt_step_recorder_begin_turn(self, agent_id, g_get_real_time());

	run = runs->running;

	if (run->calls == 0) {
		run->checkpoint = g_strdup(checkpoint);

		if (checkpoint == NULL)
			run_refuse(run, "there was no picture of the screen from before "
				"its first step to check a replay against");
	}

	run->calls++;

	if (run->events->len >= CLAWT_SAVED_STEPS_MAX) {
		run_refuse(run, "it took more steps than one saved list keeps");
		return;
	}

	event = clawt_saved_step_from_call(tool, arguments, &runs->pointer_x,
		&runs->pointer_y, &why);

	if (event == NULL) {
		run_refuse(run, why);
		return;
	}

	/*
	 * A secret typed into a screen would be stored as typed.  The same
	 * redaction every log line goes through decides what looks like one.
	 */
	if (event->kind == CLAWT_INPUT_TEXT && event->text != NULL) {
		g_autofree gchar *redacted = clawt_redact_secrets(event->text);

		if (g_strcmp0(redacted, event->text) != 0)
			run_refuse(run, "it typed something that looks like a secret, "
				"and a saved step is stored as typed");
	}

	g_ptr_array_add(run->events, event);
}

gboolean
clawt_step_recorder_end_turn(
	ClawtStepRecorder *self,
	const gchar       *agent_id,
	gint64             now_usec
){
	AgentRuns *runs;

	g_return_val_if_fail(CLAWT_IS_STEP_RECORDER(self), FALSE);

	runs = (agent_id != NULL) ? g_hash_table_lookup(self->agents, agent_id)
		: NULL;

	if (runs == NULL || runs->running == NULL)
		return FALSE;

	if (runs->running->calls == 0) {
		g_clear_pointer(&runs->running, clawt_step_run_free);
		return FALSE;
	}

	runs->running->finished_at = now_usec / G_USEC_PER_SEC;
	clawt_step_run_free(runs->last);
	runs->last = g_steal_pointer(&runs->running);

	return TRUE;
}

const ClawtStepRun *
clawt_step_recorder_get_last(
	ClawtStepRecorder *self,
	const gchar       *agent_id
){
	AgentRuns *runs;

	g_return_val_if_fail(CLAWT_IS_STEP_RECORDER(self), NULL);

	runs = (agent_id != NULL) ? g_hash_table_lookup(self->agents, agent_id)
		: NULL;

	return (runs != NULL) ? runs->last : NULL;
}

void
clawt_step_recorder_forget(
	ClawtStepRecorder *self,
	const gchar       *agent_id
){
	g_return_if_fail(CLAWT_IS_STEP_RECORDER(self));

	if (agent_id != NULL)
		g_hash_table_remove(self->agents, agent_id);
}

static void
clawt_step_recorder_finalize(
	GObject *object
){
	ClawtStepRecorder *self = CLAWT_STEP_RECORDER(object);

	g_clear_pointer(&self->agents, g_hash_table_unref);

	G_OBJECT_CLASS(clawt_step_recorder_parent_class)->finalize(object);
}

static void
clawt_step_recorder_class_init(
	ClawtStepRecorderClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_step_recorder_finalize;
}

static void
clawt_step_recorder_init(
	ClawtStepRecorder *self
){
	self->agents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		agent_runs_free);
}

/* ── On disk ─────────────────────────────────────────────────────── */

static gboolean
name_is_valid(
	const gchar *name
){
	g_autofree gchar *trimmed = NULL;
	const gchar *p;

	if (name == NULL || *name == '\0' || strlen(name) > 64)
		return FALSE;

	for (p = name; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != ' ' && *p != '-' && *p != '_' &&
			*p != '.')
			return FALSE;

	/* Not all spaces, and never a path. */
	trimmed = g_strstrip(g_strdup(name));

	return *trimmed != '\0' && strstr(name, "..") == NULL;
}

static gchar *
file_for(
	const gchar  *dir,
	const gchar  *agent_id,
	GError      **error
){
	g_autofree gchar *base = NULL;

	if (!clawt_is_valid_id(agent_id)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			"'%s' is not an agent id", agent_id != NULL ? agent_id : "");
		return NULL;
	}

	base = g_strdup_printf("%s.yaml", agent_id);

	return g_build_filename(dir, base, NULL);
}

/* The saved lists, as an array; empty when there is no file. */
static JsonNode *
read_all(
	const gchar  *path,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonObject *object;
	YamlNode *node;

	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		JsonNode *empty = json_node_new(JSON_NODE_ARRAY);

		json_node_take_array(empty, json_array_new());
		return empty;
	}

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_file(parser, path, error))
		return NULL;

	node = yaml_parser_get_root(parser);
	root = (node != NULL) ? yaml_node_to_json_node(node) : NULL;
	object = (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
		? json_node_get_object(root) : NULL;

	if (object == NULL || !json_object_has_member(object, "saved") ||
		!JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "saved"))) {
		JsonNode *empty = json_node_new(JSON_NODE_ARRAY);

		json_node_take_array(empty, json_array_new());
		return empty;
	}

	return json_node_copy(json_object_get_member(object, "saved"));
}

static gboolean
write_all(
	const gchar  *dir,
	const gchar  *path,
	JsonArray    *saved,
	GError      **error
){
	g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(YamlNode) yaml = NULL;
	g_autoptr(YamlGenerator) generator = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *object = json_object_new();

	json_object_set_array_member(object, "saved", json_array_ref(saved));
	json_node_take_object(root, object);

	yaml = yaml_node_from_json_node(root);
	generator = yaml_generator_new();
	yaml_generator_set_root(generator, yaml);
	text = yaml_generator_to_data(generator, NULL, error);

	if (text == NULL)
		return FALSE;

	if (!clawt_ensure_dir(dir, 0700, error))
		return FALSE;

	/* 0600: typed text is in here, as typed. */
	return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

static gint
index_of(
	JsonArray   *saved,
	const gchar *name
){
	guint i;

	for (i = 0; i < json_array_get_length(saved); i++) {
		JsonObject *entry = json_array_get_object_element(saved, i);

		if (entry != NULL && json_object_has_member(entry, "name") &&
			g_strcmp0(json_object_get_string_member(entry, "name"), name) == 0)
			return (gint)i;
	}

	return -1;
}

gboolean
clawt_saved_steps_save(
	const gchar         *dir,
	const gchar         *agent_id,
	const gchar         *name,
	const ClawtStepRun  *run,
	GError             **error
){
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) all = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	JsonArray *saved;
	gint existing;
	guint i;

	g_return_val_if_fail(dir != NULL, FALSE);
	g_return_val_if_fail(run != NULL, FALSE);

	if (!name_is_valid(name)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			"a saved list's name is letters, digits, spaces, '-', '_' and "
			"'.', at most 64 characters");
		return FALSE;
	}

	if (!run->saveable || run->checkpoint == NULL ||
		run->events->len == 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			"that turn's steps cannot be saved: %s",
			run->why_not != NULL ? run->why_not
				: run->events->len == 0 ? "it made no step"
				: "there is no picture of the screen to start from");
		return FALSE;
	}

	path = file_for(dir, agent_id, error);
	if (path == NULL)
		return FALSE;

	all = read_all(path, error);
	if (all == NULL)
		return FALSE;

	saved = json_node_get_array(all);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "created_at");
	json_builder_add_int_value(builder, g_get_real_time() / G_USEC_PER_SEC);
	json_builder_set_member_name(builder, "checkpoint");
	json_builder_add_string_value(builder, run->checkpoint);
	json_builder_set_member_name(builder, "steps");
	json_builder_begin_array(builder);

	for (i = 0; i < run->events->len; i++)
		json_builder_add_value(builder, clawt_saved_step_to_json(
			g_ptr_array_index(run->events, i)));

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	/* One of the same name is replaced, where it was in the list. */
	existing = index_of(saved, name);

	if (existing >= 0)
		json_array_remove_element(saved, (guint)existing);

	json_array_add_element(saved, json_builder_get_root(builder));

	return write_all(dir, path, saved, error);
}

JsonNode *
clawt_saved_steps_list(
	const gchar  *dir,
	const gchar  *agent_id,
	GError      **error
){
	g_autofree gchar *path = NULL;

	g_return_val_if_fail(dir != NULL, NULL);

	path = file_for(dir, agent_id, error);

	return (path != NULL) ? read_all(path, error) : NULL;
}

GPtrArray *
clawt_saved_steps_get(
	const gchar  *dir,
	const gchar  *agent_id,
	const gchar  *name,
	gchar       **checkpoint,
	GError      **error
){
	g_autoptr(JsonNode) all = clawt_saved_steps_list(dir, agent_id, error);
	g_autoptr(GPtrArray) events = NULL;
	JsonObject *entry;
	JsonArray *steps;
	gint at;
	guint i;

	g_return_val_if_fail(checkpoint != NULL, NULL);

	*checkpoint = NULL;

	if (all == NULL)
		return NULL;

	at = index_of(json_node_get_array(all), name);

	if (at < 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			"%s has no saved steps called '%s'", agent_id,
			name != NULL ? name : "");
		return NULL;
	}

	entry = json_array_get_object_element(json_node_get_array(all),
		(guint)at);
	steps = json_object_has_member(entry, "steps")
		? json_object_get_array_member(entry, "steps") : NULL;
	events = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_input_event_free);

	for (i = 0; steps != NULL && i < json_array_get_length(steps); i++) {
		ClawtInputEvent *event = clawt_saved_step_from_json(
			json_array_get_object_element(steps, i));

		/* A list that cannot be read whole is not replayed in part. */
		if (event == NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_PARSE,
				"step %u of '%s' could not be read", i + 1, name);
			return NULL;
		}

		g_ptr_array_add(events, event);
	}

	if (!json_object_has_member(entry, "checkpoint") || events->len == 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_PARSE,
			"'%s' has no checkpoint or no steps, so it cannot be replayed",
			name);
		return NULL;
	}

	*checkpoint = g_strdup(json_object_get_string_member(entry,
		"checkpoint"));

	return g_steal_pointer(&events);
}

gboolean
clawt_saved_steps_delete(
	const gchar  *dir,
	const gchar  *agent_id,
	const gchar  *name,
	GError      **error
){
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) all = NULL;
	gint at;

	g_return_val_if_fail(dir != NULL, FALSE);

	path = file_for(dir, agent_id, error);
	if (path == NULL)
		return FALSE;

	all = read_all(path, error);
	if (all == NULL)
		return FALSE;

	at = index_of(json_node_get_array(all), name);

	if (at < 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			"%s has no saved steps called '%s'", agent_id,
			name != NULL ? name : "");
		return FALSE;
	}

	json_array_remove_element(json_node_get_array(all), (guint)at);

	return write_all(dir, path, json_node_get_array(all), error);
}
