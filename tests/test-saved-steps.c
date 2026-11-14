/*
 * test-saved-steps.c - A desktop task's input, saved, and replayed with
 * no model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every refusal is reached here, because the feature is its refusals:
 * a call that cannot be repeated exactly, a secret typed into a screen,
 * a turn with no picture from before it, a replay without the screen
 * held.  The daemon half goes through `computer.control` and the
 * `steps.*` frames as the relay and the clients do.  A replay against a
 * real screen needs a compositor and is not hermetic; what it checks
 * first -- that the screen is held and exists -- is.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

static JsonObject *
args(
	const gchar *json
){
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));

	return json_object_ref(json_node_get_object(json_parser_get_root(
		parser)));
}

/* gowl's and gnome-desktop-mcp's spellings, and what is not a step. */
static void
test_calls_become_steps(
	void
){
	g_autoptr(JsonObject) key = args("{\"key\": \"ctrl+s\"}");
	g_autoptr(JsonObject) combo = args("{\"combo\": \"Return\"}");
	g_autoptr(JsonObject) text = args("{\"text\": \"hello\"}");
	g_autoptr(JsonObject) move = args("{\"x\": 120, \"y\": 340}");
	g_autoptr(JsonObject) gowl_click = args("{\"button\": \"right\", "
		"\"action\": \"click\"}");
	g_autoptr(JsonObject) gnome_click = args("{\"x\": 5, \"y\": 6, "
		"\"button\": 1}");
	g_autoptr(JsonObject) scroll = args("{\"direction\": \"down\"}");
	g_autoptr(ClawtInputEvent) event = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *line = NULL;
	gint x = -1;
	gint y = -1;

	event = clawt_saved_step_from_call("send_key", key, &x, &y, NULL);
	g_assert_cmpint(event->kind, ==, CLAWT_INPUT_KEY);
	g_assert_cmpstr(event->text, ==, "ctrl+s");
	g_clear_pointer(&event, clawt_input_event_free);

	event = clawt_saved_step_from_call("key_combo", combo, &x, &y, NULL);
	g_assert_cmpstr(event->text, ==, "Return");
	g_clear_pointer(&event, clawt_input_event_free);

	event = clawt_saved_step_from_call("type_text", text, &x, &y, NULL);
	g_assert_cmpint(event->kind, ==, CLAWT_INPUT_TEXT);
	line = clawt_saved_step_describe(event);
	g_assert_nonnull(strstr(line, "hello"));
	g_clear_pointer(&event, clawt_input_event_free);

	/* A gowl click is a step only once the run knows where the pointer is. */
	event = clawt_saved_step_from_call("send_mouse", gowl_click, &x, &y,
		&why);
	g_assert_null(event);
	g_assert_nonnull(why);
	g_clear_pointer(&why, g_free);

	event = clawt_saved_step_from_call("send_mouse_move", move, &x, &y,
		NULL);
	g_assert_cmpint(event->kind, ==, CLAWT_INPUT_MOVE);
	g_assert_cmpint(x, ==, 120);
	g_clear_pointer(&event, clawt_input_event_free);

	event = clawt_saved_step_from_call("send_mouse", gowl_click, &x, &y,
		NULL);
	g_assert_cmpint(event->kind, ==, CLAWT_INPUT_CLICK);
	g_assert_cmpint(event->x, ==, 120);
	g_assert_cmpint(event->y, ==, 340);
	g_assert_cmpuint(event->button, ==, 3);
	g_clear_pointer(&event, clawt_input_event_free);

	event = clawt_saved_step_from_call("mouse_click", gnome_click, &x, &y,
		NULL);
	g_assert_cmpint(event->x, ==, 5);
	g_assert_cmpuint(event->button, ==, 1);
	g_clear_pointer(&event, clawt_input_event_free);

	event = clawt_saved_step_from_call("send_scroll", scroll, &x, &y, NULL);
	g_assert_cmpint(event->kind, ==, CLAWT_INPUT_SCROLL);
	g_assert_cmpfloat(event->dy, >, 0);
	g_clear_pointer(&event, clawt_input_event_free);

	/* Acting, but not something a replay can repeat exactly. */
	g_assert_null(clawt_saved_step_from_call("focus_window", move, &x, &y,
		&why));
	g_assert_nonnull(strstr(why, "focus_window"));
	g_clear_pointer(&why, g_free);

	/* A click with no coordinates is not a guess at them. */
	g_assert_null(clawt_saved_step_from_call("mouse_click", key, &x, &y,
		NULL));
}

/*
 * The recorder: a run per turn, refused for the first reason it meets,
 * and a turn that never touched the screen keeps the last run that did.
 */
static void
test_the_recorder(
	void
){
	g_autoptr(ClawtStepRecorder) recorder = clawt_step_recorder_new();
	g_autoptr(JsonObject) move = args("{\"x\": 1, \"y\": 2}");
	g_autoptr(JsonObject) click = args("{\"action\": \"click\"}");
	g_autoptr(JsonObject) secret = args("{\"text\": "
		"\"ghp_abcdefghijklmnopqrstuvwxyz0123456789AB\"}");
	const ClawtStepRun *last;

	clawt_step_recorder_begin_turn(recorder, "alpha", 1000);
	g_assert_true(clawt_step_recorder_wants_checkpoint(recorder, "alpha"));
	clawt_step_recorder_note_call(recorder, "alpha", "send_mouse_move", move,
		"abc123");
	g_assert_false(clawt_step_recorder_wants_checkpoint(recorder, "alpha"));
	clawt_step_recorder_note_call(recorder, "alpha", "send_mouse", click,
		NULL);
	g_assert_true(clawt_step_recorder_end_turn(recorder, "alpha", 2000000));

	last = clawt_step_recorder_get_last(recorder, "alpha");
	g_assert_nonnull(last);
	g_assert_true(last->saveable);
	g_assert_cmpuint(last->events->len, ==, 2);
	g_assert_cmpstr(last->checkpoint, ==, "abc123");
	g_assert_cmpint(last->finished_at, ==, 2);

	/* A turn with no desktop call keeps the last run that had one. */
	clawt_step_recorder_begin_turn(recorder, "alpha", 3000);
	g_assert_false(clawt_step_recorder_end_turn(recorder, "alpha", 4000));
	g_assert_true(clawt_step_recorder_get_last(recorder, "alpha") == last);

	/* No picture before the first step: recorded, not saveable. */
	clawt_step_recorder_begin_turn(recorder, "alpha", 5000);
	clawt_step_recorder_note_call(recorder, "alpha", "send_mouse_move", move,
		NULL);
	clawt_step_recorder_end_turn(recorder, "alpha", 6000);
	last = clawt_step_recorder_get_last(recorder, "alpha");
	g_assert_false(last->saveable);
	g_assert_nonnull(strstr(last->why_not, "picture"));

	/* A secret typed into the screen. */
	clawt_step_recorder_begin_turn(recorder, "beta", 5000);
	clawt_step_recorder_note_call(recorder, "beta", "type_text", secret,
		"abc");
	clawt_step_recorder_end_turn(recorder, "beta", 6000);
	last = clawt_step_recorder_get_last(recorder, "beta");
	g_assert_false(last->saveable);
	g_assert_nonnull(strstr(last->why_not, "secret"));

	/* Something that acts and cannot be repeated: the first reason stays. */
	clawt_step_recorder_begin_turn(recorder, "gamma", 5000);
	clawt_step_recorder_note_call(recorder, "gamma", "spawn", NULL, "abc");
	clawt_step_recorder_note_call(recorder, "gamma", "focus_window", NULL,
		NULL);
	clawt_step_recorder_end_turn(recorder, "gamma", 6000);
	last = clawt_step_recorder_get_last(recorder, "gamma");
	g_assert_false(last->saveable);
	g_assert_nonnull(strstr(last->why_not, "spawn"));
}

/* Saved, listed (numbers back as numbers), read whole, deleted. */
static void
test_the_store(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-steps-XXXXXX", NULL);
	g_autoptr(ClawtStepRun) run = clawt_step_run_new();
	g_autoptr(ClawtStepRun) bad = clawt_step_run_new();
	g_autoptr(ClawtInputEvent) click = clawt_input_event_new(
		CLAWT_INPUT_CLICK);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) listing = NULL;
	g_autoptr(GPtrArray) back = NULL;
	g_autofree gchar *checkpoint = NULL;
	ClawtInputEvent *first;

	click->x = 640;
	click->y = 480;
	click->button = 1;
	g_ptr_array_add(run->events, clawt_input_event_copy(click));
	run->checkpoint = g_strdup("feedface");

	g_assert_false(clawt_saved_steps_save(dir, "alpha", "../escape", run,
		&error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(clawt_saved_steps_save(dir, "alpha", "x", bad, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_assert_true(clawt_saved_steps_save(dir, "alpha", "open the report",
		run, &error));
	g_assert_no_error(error);

	listing = clawt_saved_steps_list(dir, "alpha", &error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(listing)), ==,
		1);

	back = clawt_saved_steps_get(dir, "alpha", "open the report", &checkpoint,
		&error);
	g_assert_no_error(error);
	g_assert_cmpstr(checkpoint, ==, "feedface");
	g_assert_cmpuint(back->len, ==, 1);
	first = g_ptr_array_index(back, 0);
	g_assert_cmpint(first->kind, ==, CLAWT_INPUT_CLICK);
	g_assert_cmpint(first->x, ==, 640);
	g_assert_cmpint(first->y, ==, 480);

	g_assert_true(clawt_saved_steps_delete(dir, "alpha", "open the report",
		&error));
	g_assert_false(clawt_saved_steps_delete(dir, "alpha", "open the report",
		&error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);

	clawt_test_remove_tree(dir);
}

/*
 * The relay hands the gate the call's arguments: without them the gate
 * knows a click happened and not where, and nothing could be saved.
 */
static void
test_the_gate_sees_arguments(
	void
){
	g_autoptr(JsonObject) found = clawt_mcp_relay_call_arguments(
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"mouse_click\",\"arguments\":"
		"{\"x\":4,\"y\":9}}}");
	g_autoptr(JsonObject) none = clawt_mcp_relay_call_arguments(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");

	g_assert_nonnull(found);
	g_assert_cmpint(json_object_get_int_member(found, "y"), ==, 9);
	g_assert_null(none);
}

/* ── Through the daemon ──────────────────────────────────────────── */

static JsonNode *
request(
	ClawtDaemon *daemon,
	const gchar *kind,
	const gchar *payload_json
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
	clawt_ipc_frame_set_payload(frame,
		json_node_copy(json_parser_get_root(parser)));

	return clawt_daemon_handle_request(daemon, frame);
}

static JsonObject *
payload_of(
	JsonNode *reply
){
	g_assert_false(json_object_has_member(json_node_get_object(reply),
		"error"));

	return json_object_get_object_member(json_node_get_object(reply),
		"payload");
}

static const gchar *
error_of(
	JsonNode *reply
){
	JsonObject *frame = json_node_get_object(reply);

	g_assert_true(json_object_has_member(frame, "error"));

	return json_object_get_string_member(frame, "error");
}

/*
 * The gate records what passes it, with its arguments; the last turn is
 * listed with its reason; a run with a checkpoint saves, lists and
 * deletes; and a replay is refused where there is no screen to hold.
 */
static void
test_the_frames(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-steps-d-XXXXXX", NULL);
	g_autofree gchar *config_path = g_build_filename(dir, "config.yaml",
		NULL);
	g_autofree gchar *yaml = NULL;
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(ClawtDaemon) daemon = NULL;
	g_autoptr(JsonNode) gate = NULL;
	g_autoptr(JsonNode) last = NULL;
	g_autoptr(JsonNode) refused = NULL;
	g_autoptr(JsonNode) saved = NULL;
	g_autoptr(JsonNode) listing = NULL;
	g_autoptr(JsonNode) replay = NULL;
	g_autoptr(JsonNode) deleted = NULL;
	g_autoptr(JsonObject) move = args("{\"x\": 10, \"y\": 20}");
	JsonObject *body;

	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n  - id: alpha\n",
		dir, dir, dir, dir, dir, dir);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, NULL));

	daemon = clawt_daemon_new(config_path, context);
	g_assert_true(clawt_daemon_start(daemon, NULL));

	/* A turn, one acting call through the gate, and its end. */
	clawt_daemon_turn_begin(daemon, "alpha", NULL);
	gate = request(daemon, "computer.control", "{\"agent\": \"alpha\", "
		"\"tool\": \"mouse_click\", \"arguments\": {\"x\": 7, \"y\": 8, "
		"\"button\": 1}}");
	g_assert_true(json_object_get_boolean_member(payload_of(gate),
		"allowed"));
	clawt_daemon_turn_settle(daemon, "alpha");

	last = request(daemon, "steps.last", "{\"agent\": \"alpha\"}");
	body = payload_of(last);
	g_assert_cmpint(json_object_get_int_member(body, "calls"), ==, 1);
	g_assert_false(json_object_get_boolean_member(body, "saveable"));
	g_assert_nonnull(strstr(json_object_get_string_member(body, "why_not"),
		"picture"));
	g_assert_cmpstr(json_array_get_string_element(
		json_object_get_array_member(body, "steps"), 0), ==,
		"Click at 7, 8");

	refused = request(daemon, "steps.save", "{\"agent\": \"alpha\", "
		"\"name\": \"try\"}");
	g_assert_nonnull(strstr(error_of(refused), "cannot be saved"));

	/* A run with a picture from before it saves. */
	clawt_step_recorder_begin_turn(daemon->step_recorder, "alpha",
		g_get_real_time());
	clawt_step_recorder_note_call(daemon->step_recorder, "alpha",
		"mouse_move", move, "0123abcd");
	clawt_step_recorder_end_turn(daemon->step_recorder, "alpha",
		g_get_real_time());

	saved = request(daemon, "steps.save", "{\"agent\": \"alpha\", "
		"\"name\": \"open it\"}");
	g_assert_true(json_object_get_boolean_member(payload_of(saved), "saved"));

	listing = request(daemon, "steps.list", "{\"agent\": \"alpha\"}");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		payload_of(listing), "saved")), ==, 1);

	/* No screen to hold: a replay is refused before anything is sent. */
	replay = request(daemon, "steps.replay", "{\"agent\": \"alpha\", "
		"\"name\": \"open it\"}");
	g_assert_nonnull(strstr(error_of(replay), "no screen"));

	deleted = request(daemon, "steps.delete", "{\"agent\": \"alpha\", "
		"\"name\": \"open it\"}");
	g_assert_true(json_object_get_boolean_member(payload_of(deleted),
		"deleted"));

	clawt_daemon_stop(daemon);
	g_clear_object(&daemon);

	while (g_main_context_iteration(context, FALSE))
		;

	clawt_test_remove_tree(dir);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/saved-steps/calls", test_calls_become_steps);
	g_test_add_func("/saved-steps/recorder", test_the_recorder);
	g_test_add_func("/saved-steps/store", test_the_store);
	g_test_add_func("/saved-steps/gate-arguments",
		test_the_gate_sees_arguments);
	g_test_add_func("/saved-steps/daemon/frames", test_the_frames);

	return g_test_run();
}
