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
	clawt_step_recorder_note_title(recorder, "alpha", "Report", 1500);
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
	g_assert_cmpstr(last->title, ==, "Report");
	g_assert_cmpint(last->finished_at, ==, 2);
	g_assert_true(clawt_step_run_can_save(last,
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, NULL));

	/* A turn with no desktop call keeps the last run that had one. */
	clawt_step_recorder_begin_turn(recorder, "alpha", 3000);
	g_assert_false(clawt_step_recorder_end_turn(recorder, "alpha", 4000));
	g_assert_true(clawt_step_recorder_get_last(recorder, "alpha") == last);

	/*
	 * No picture before the first step: saveable by its title only.  The
	 * title read before this turn began (at 1500) is not this turn's.
	 */
	clawt_step_recorder_begin_turn(recorder, "alpha", 5000);
	clawt_step_recorder_note_title(recorder, "alpha", "Report", 5500);
	clawt_step_recorder_note_call(recorder, "alpha", "send_mouse_move", move,
		NULL);
	clawt_step_recorder_end_turn(recorder, "alpha", 6000);
	last = clawt_step_recorder_get_last(recorder, "alpha");
	g_assert_true(last->saveable);
	{
		const gchar *why = NULL;

		g_assert_false(clawt_step_run_can_save(last,
			CLAWT_STEP_CHECK_PICTURE_AND_TITLE, &why));
		g_assert_nonnull(strstr(why, "title only"));
		g_assert_true(clawt_step_run_can_save(last,
			CLAWT_STEP_CHECK_TITLE_ONLY, NULL));
	}

	/* A title read before the turn began is not the turn's checkpoint. */
	clawt_step_recorder_begin_turn(recorder, "alpha", 7000);
	clawt_step_recorder_note_call(recorder, "alpha", "send_mouse_move", move,
		"abc");
	clawt_step_recorder_end_turn(recorder, "alpha", 8000);
	last = clawt_step_recorder_get_last(recorder, "alpha");
	g_assert_null(last->title);
	{
		const gchar *why = NULL;

		g_assert_false(clawt_step_run_can_save(last,
			CLAWT_STEP_CHECK_TITLE_ONLY, &why));
		g_assert_nonnull(strstr(why, "focus"));
	}

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
	g_autofree gchar *title = NULL;
	ClawtStepCheck check = CLAWT_STEP_CHECK_TITLE_ONLY;
	ClawtInputEvent *first;

	click->x = 640;
	click->y = 480;
	click->button = 1;
	g_ptr_array_add(run->events, clawt_input_event_copy(click));
	run->checkpoint = g_strdup("feedface");
	run->title = g_strdup("2026");

	g_assert_false(clawt_saved_steps_save(dir, "alpha", "../escape", run,
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(clawt_saved_steps_save(dir, "alpha", "x", bad,
		CLAWT_STEP_CHECK_TITLE_ONLY, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_assert_true(clawt_saved_steps_save(dir, "alpha", "open the report",
		run, CLAWT_STEP_CHECK_PICTURE_AND_TITLE, &error));
	g_assert_no_error(error);

	listing = clawt_saved_steps_list(dir, "alpha", &error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(listing)), ==,
		1);

	back = clawt_saved_steps_get(dir, "alpha", "open the report", &checkpoint,
		&title, &check, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(checkpoint, ==, "feedface");

	/* A title of digits comes back from YAML as the same text. */
	g_assert_cmpstr(title, ==, "2026");
	g_assert_cmpint(check, ==, CLAWT_STEP_CHECK_PICTURE_AND_TITLE);
	g_assert_cmpuint(back->len, ==, 1);
	first = g_ptr_array_index(back, 0);
	g_assert_cmpint(first->kind, ==, CLAWT_INPUT_CLICK);
	g_assert_cmpint(first->x, ==, 640);
	g_assert_cmpint(first->y, ==, 480);

	/* Switched to title only, and back. */
	g_assert_true(clawt_saved_steps_set_check(dir, "alpha", "open the report",
		CLAWT_STEP_CHECK_TITLE_ONLY, &error));
	g_clear_pointer(&back, g_ptr_array_unref);
	g_clear_pointer(&checkpoint, g_free);
	g_clear_pointer(&title, g_free);
	back = clawt_saved_steps_get(dir, "alpha", "open the report", &checkpoint,
		&title, &check, &error);
	g_assert_cmpint(check, ==, CLAWT_STEP_CHECK_TITLE_ONLY);

	/* A list saved with no picture cannot be switched to need one. */
	g_clear_pointer(&run->checkpoint, g_free);
	g_assert_true(clawt_saved_steps_save(dir, "alpha", "clock", run,
		CLAWT_STEP_CHECK_TITLE_ONLY, &error));
	g_assert_false(clawt_saved_steps_set_check(dir, "alpha", "clock",
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(clawt_saved_steps_save(dir, "alpha", "clock", run,
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

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
		"focus"));
	g_assert_cmpstr(json_array_get_string_element(
		json_object_get_array_member(body, "steps"), 0), ==,
		"Click at 7, 8");

	refused = request(daemon, "steps.save", "{\"agent\": \"alpha\", "
		"\"name\": \"try\"}");
	g_assert_nonnull(strstr(error_of(refused), "cannot be saved"));

	/* A run with a picture and a title from before it saves. */
	clawt_step_recorder_begin_turn(daemon->step_recorder, "alpha",
		g_get_real_time());
	clawt_step_recorder_note_title(daemon->step_recorder, "alpha", "Report",
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
	{
		JsonObject *entry = json_array_get_object_element(
			json_object_get_array_member(payload_of(listing), "saved"), 0);

		g_assert_cmpstr(json_object_get_string_member(entry, "check"), ==,
			"picture-and-title");
		g_assert_cmpstr(json_object_get_string_member(entry, "title"), ==,
			"Report");
		g_assert_cmpstr(json_object_get_string_member(entry, "other_check"),
			==, "title-only");
	}

	/* Switched per list, by the clients' one button. */
	{
		g_autoptr(JsonNode) switched = request(daemon, "steps.set_check",
			"{\"agent\": \"alpha\", \"name\": \"open it\", "
			"\"check\": \"title-only\"}");
		g_autoptr(JsonNode) wrong = request(daemon, "steps.set_check",
			"{\"agent\": \"alpha\", \"name\": \"open it\", "
			"\"check\": \"sometimes\"}");

		g_assert_cmpstr(json_object_get_string_member(payload_of(switched),
			"check"), ==, "title-only");
		g_assert_nonnull(strstr(error_of(wrong), "title-only"));
	}

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

/* ── The title half of the checkpoint ────────────────────────────── */

/* Both backends' answers read to one string; anything else refused. */
static void
test_the_focused_title(
	void
){
	g_autoptr(GError) error = NULL;
	gchar *title = NULL;

	/* gowl's get_focused_client. */
	g_assert_true(clawt_screen_parse_focused_title(
		"{\"id\": 3, \"app_id\": \"foot\", \"title\": \"~/src\"}", &title,
		&error));
	g_assert_cmpstr(title, ==, "~/src");
	g_clear_pointer(&title, g_free);

	/* gowl with nothing focused. */
	g_assert_true(clawt_screen_parse_focused_title("null", &title, NULL));
	g_assert_cmpstr(title, ==, "");
	g_clear_pointer(&title, g_free);

	/* gnome-desktop-mcp's window list. */
	g_assert_true(clawt_screen_parse_focused_title(
		"[{\"title\": \"Files\", \"focused\": false},"
		" {\"title\": \"Report - Editor\", \"focused\": true}]", &title,
		NULL));
	g_assert_cmpstr(title, ==, "Report - Editor");
	g_clear_pointer(&title, g_free);

	g_assert_true(clawt_screen_parse_focused_title("[]", &title, NULL));
	g_assert_cmpstr(title, ==, "");
	g_clear_pointer(&title, g_free);

	/* Two focused windows, a title that is not text, not JSON at all. */
	g_assert_false(clawt_screen_parse_focused_title(
		"[{\"title\": \"a\", \"focused\": true},"
		" {\"title\": \"b\", \"has_focus\": true}]", &title, &error));
	g_assert_null(title);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
	g_clear_error(&error);
	g_assert_false(clawt_screen_parse_focused_title("{\"title\": 7}", &title,
		NULL));
	g_assert_false(clawt_screen_parse_focused_title("Error: no such method",
		&title, NULL));
	g_assert_false(clawt_screen_parse_focused_title(NULL, &title, NULL));
}

/* The comparison a replay makes, every way it can refuse. */
static void
test_the_screen_check(
	void
){
	g_autofree gchar *why = NULL;

	g_assert_true(clawt_saved_steps_check_screen(
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, "aaa", "Editor", "aaa", "Editor",
		&why));
	g_assert_null(why);

	/* The picture moved. */
	g_assert_false(clawt_saved_steps_check_screen(
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, "aaa", "Editor", "bbb", "Editor",
		&why));
	g_assert_nonnull(strstr(why, "does not look"));
	g_clear_pointer(&why, g_free);

	/* Title only: a ticking clock does not block it. */
	g_assert_true(clawt_saved_steps_check_screen(CLAWT_STEP_CHECK_TITLE_ONLY,
		"aaa", "Editor", "bbb", "Editor", NULL));
	g_assert_true(clawt_saved_steps_check_screen(CLAWT_STEP_CHECK_TITLE_ONLY,
		NULL, "Editor", NULL, "Editor", NULL));

	/* The title is a different window, in either check. */
	g_assert_false(clawt_saved_steps_check_screen(CLAWT_STEP_CHECK_TITLE_ONLY,
		NULL, "Editor", NULL, "Terminal", &why));
	g_assert_nonnull(strstr(why, "Terminal"));
	g_clear_pointer(&why, g_free);
	g_assert_false(clawt_saved_steps_check_screen(
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, "aaa", "Editor", "aaa",
		"Terminal", NULL));

	/* Unknown is never a match; nor is a list saved before titles. */
	g_assert_false(clawt_saved_steps_check_screen(CLAWT_STEP_CHECK_TITLE_ONLY,
		NULL, "Editor", NULL, NULL, &why));
	g_assert_nonnull(strstr(why, "could not be read"));
	g_clear_pointer(&why, g_free);
	g_assert_false(clawt_saved_steps_check_screen(
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE, "aaa", NULL, "aaa", "Editor",
		&why));
	g_assert_nonnull(strstr(why, "save them again"));
	g_clear_pointer(&why, g_free);

	/* No focused window is a state, and it matches itself. */
	g_assert_true(clawt_saved_steps_check_screen(CLAWT_STEP_CHECK_TITLE_ONLY,
		NULL, "", NULL, "", NULL));

	g_assert_cmpint(clawt_step_check_other(CLAWT_STEP_CHECK_TITLE_ONLY), ==,
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE);
	g_assert_cmpstr(clawt_step_check_label(CLAWT_STEP_CHECK_TITLE_ONLY), ==,
		"Title only");
}

/* ── A replay, against a fake screen ─────────────────────────────── */

#define TEST_TYPE_SCREEN (test_screen_get_type())

G_DECLARE_FINAL_TYPE(TestScreen, test_screen, TEST, SCREEN, ClawtComputer)

/*
 * A screen whose picture never changes and whose focused window is
 * whatever the test says.  Input is counted, atomically -- it arrives on
 * the observer's worker thread -- and each one takes a moment, so a test
 * can let go of the screen between two of them.
 */
struct _TestScreen {
	ClawtComputer parent_instance;

	GMutex  lock;
	gchar  *title;
	gint    inputs;
};

static void test_screen_observable_init(ClawtObservableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
	TestScreen, test_screen, CLAWT_TYPE_COMPUTER,
	G_IMPLEMENT_INTERFACE(CLAWT_TYPE_OBSERVABLE, test_screen_observable_init))

static const guchar tiny_png[] = {
	0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
	0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
	0x00, 0x00, 0x02, 0x80,
	0x00, 0x00, 0x01, 0xe0,
	0x08, 0x06, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static gboolean
test_screen_start(
	ClawtObservable  *observable,
	guint             fps,
	GError          **error
){
	(void)observable;
	(void)fps;
	(void)error;

	return TRUE;
}

static GBytes *
test_screen_frame(
	ClawtObservable  *observable,
	const gchar      *if_changed_from,
	gint64           *stamp_out,
	gchar           **hash_out,
	GError          **error
){
	g_autoptr(GBytes) bytes = g_bytes_new_static(tiny_png, sizeof(tiny_png));

	(void)observable;
	(void)if_changed_from;
	(void)error;

	if (stamp_out != NULL)
		*stamp_out = g_get_real_time();

	if (hash_out != NULL)
		*hash_out = clawt_screen_hash_bytes(bytes);

	return g_bytes_new(tiny_png, sizeof(tiny_png));
}

static gboolean
test_screen_can_input(
	ClawtObservable *observable
){
	(void)observable;

	return TRUE;
}

static gboolean
test_screen_send_input(
	ClawtObservable  *observable,
	ClawtInputEvent  *event,
	GError          **error
){
	(void)event;
	(void)error;

	g_atomic_int_inc(&TEST_SCREEN(observable)->inputs);
	g_usleep(40 * 1000);

	return TRUE;
}

static gchar *
test_screen_focused_title(
	ClawtObservable  *observable,
	GError          **error
){
	TestScreen *self = TEST_SCREEN(observable);
	gchar *title;

	(void)error;

	g_mutex_lock(&self->lock);
	title = g_strdup(self->title);
	g_mutex_unlock(&self->lock);

	return title;
}

static void
test_screen_observable_init(
	ClawtObservableInterface *iface
){
	iface->observe_start = test_screen_start;
	iface->observe_frame = test_screen_frame;
	iface->observe_can_input = test_screen_can_input;
	iface->observe_send_input = test_screen_send_input;
	iface->observe_focused_title = test_screen_focused_title;
}

static ClawtComputerType
test_screen_get_computer_type(
	ClawtComputer *computer
){
	(void)computer;

	return CLAWT_COMPUTER_VM;
}

static void
test_screen_finalize(
	GObject *object
){
	TestScreen *self = TEST_SCREEN(object);

	g_free(self->title);
	g_mutex_clear(&self->lock);

	G_OBJECT_CLASS(test_screen_parent_class)->finalize(object);
}

static void
test_screen_class_init(
	TestScreenClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = test_screen_finalize;
	CLAWT_COMPUTER_CLASS(klass)->get_computer_type =
		test_screen_get_computer_type;
}

static void
test_screen_init(
	TestScreen *self
){
	g_mutex_init(&self->lock);
	self->title = g_strdup("Report - Editor");
}

typedef struct {
	gchar         *dir;
	GMainContext  *context;
	ClawtDaemon   *daemon;
	TestScreen    *screen;
	gchar         *error;      /* the steps.replayed event's, if any */
	gboolean       finished;
	gint64         done;
} ReplayFixture;

static void
on_bus_event(
	ClawtEventBus *bus,
	ClawtEvent    *event,
	gpointer       user_data
){
	ReplayFixture *fixture = user_data;

	(void)bus;

	if (g_strcmp0(clawt_event_get_kind(event), "steps.replayed") != 0)
		return;

	fixture->finished = TRUE;
	fixture->error = g_strdup(clawt_event_get_detail(event, "error"));
	fixture->done = g_ascii_strtoll(clawt_event_get_detail(event, "done"),
		NULL, 10);
}

static void
replay_setup(
	ReplayFixture *fixture
){
	g_autofree gchar *config_path = NULL;
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-replay-XXXXXX", NULL);
	config_path = g_build_filename(fixture->dir, "config.yaml", NULL);
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
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, NULL));

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(config_path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));

	fixture->screen = g_object_new(TEST_TYPE_SCREEN, NULL);
	clawt_agent_set_computer(clawt_agent_manager_get(
		clawt_daemon_get_agents(fixture->daemon), "alpha"),
		CLAWT_COMPUTER(fixture->screen));

	/* Watched, as the Screen view does, so frames are taken at all. */
	g_assert_true(clawt_observer_subscribe(fixture->daemon->observer,
		"alpha", CLAWT_COMPUTER(fixture->screen), "test", 4, &error));
	g_assert_no_error(error);

	g_signal_connect(fixture->daemon->bus, "event",
		G_CALLBACK(on_bus_event), fixture);
}

static void
replay_teardown(
	ReplayFixture *fixture
){
	g_signal_handlers_disconnect_by_data(fixture->daemon->bus, fixture);
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_main_context_unref(fixture->context);
	g_clear_object(&fixture->screen);

	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
	g_free(fixture->error);
}

/* Saves @n moves under @name, with the checkpoint and check given. */
static void
save_moves(
	ReplayFixture  *fixture,
	const gchar    *name,
	guint           n,
	const gchar    *checkpoint,
	const gchar    *title,
	ClawtStepCheck  check
){
	g_autoptr(ClawtStepRun) run = clawt_step_run_new();
	g_autofree gchar *dir = g_build_filename(fixture->dir, "state",
		"saved-steps", NULL);
	g_autoptr(GError) error = NULL;
	guint i;

	for (i = 0; i < n; i++) {
		ClawtInputEvent *move = clawt_input_event_new(CLAWT_INPUT_MOVE);

		move->x = (gint)(10 * i);
		move->y = 5;
		g_ptr_array_add(run->events, move);
	}

	run->checkpoint = g_strdup(checkpoint);
	run->title = g_strdup(title);
	g_assert_true(clawt_saved_steps_save(dir, "alpha", name, run, check,
		&error));
	g_assert_no_error(error);
}

static gboolean
on_replay_watchdog(
	gpointer data
){
	gboolean *fired = data;

	*fired = TRUE;
	return G_SOURCE_REMOVE;
}

/*
 * Starts a replay and waits for its steps.replayed, inside a watchdog.
 * @release_after > 0 lets go of the screen once that many steps have
 * been sent -- the way a person says stop.
 */
static void
replay(
	ReplayFixture *fixture,
	const gchar   *name,
	gint           release_after
){
	g_autofree gchar *payload = g_strdup_printf("{\"agent\": \"alpha\", "
		"\"name\": \"%s\"}", name);
	g_autoptr(JsonNode) started = NULL;
	gboolean fired = FALSE;
	GSource *watchdog = g_timeout_source_new_seconds(20);

	g_source_set_callback(watchdog, on_replay_watchdog, &fired, NULL);
	g_source_attach(watchdog, fixture->context);

	fixture->finished = FALSE;
	g_clear_pointer(&fixture->error, g_free);

	g_assert_true(clawt_takeover_take(fixture->daemon->takeover, "alpha",
		"operator", 300, NULL));
	started = request(fixture->daemon, "steps.replay", payload);
	g_assert_true(json_object_get_boolean_member(payload_of(started),
		"started"));

	while (!fixture->finished && !fired) {
		g_main_context_iteration(fixture->context, TRUE);

		if (release_after > 0 &&
			g_atomic_int_get(&fixture->screen->inputs) >= release_after)
			clawt_takeover_release(fixture->daemon->takeover, "alpha");
	}

	g_assert_false(fired);
	g_source_destroy(watchdog);
	g_source_unref(watchdog);
	clawt_takeover_release(fixture->daemon->takeover, "alpha");
}

static gchar *
screen_hash(
	void
){
	g_autoptr(GBytes) bytes = g_bytes_new_static(tiny_png, sizeof(tiny_png));

	return clawt_screen_hash_bytes(bytes);
}

/* The picture matches but another window has the focus: nothing sent. */
static void
test_replay_refuses_a_different_title(
	void
){
	ReplayFixture fixture = { 0 };
	g_autofree gchar *hash = screen_hash();

	replay_setup(&fixture);
	save_moves(&fixture, "edit", 3, hash, "Inbox - Mail",
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE);

	replay(&fixture, "edit", 0);
	g_assert_nonnull(fixture.error);
	g_assert_nonnull(strstr(fixture.error, "Report - Editor"));
	g_assert_cmpint(g_atomic_int_get(&fixture.screen->inputs), ==, 0);

	replay_teardown(&fixture);
}

/* The title matches but the picture does not: nothing sent. */
static void
test_replay_refuses_a_different_picture(
	void
){
	ReplayFixture fixture = { 0 };

	replay_setup(&fixture);
	save_moves(&fixture, "edit", 3, "0000feed", "Report - Editor",
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE);

	replay(&fixture, "edit", 0);
	g_assert_nonnull(fixture.error);
	g_assert_nonnull(strstr(fixture.error, "does not look"));
	g_assert_cmpint(g_atomic_int_get(&fixture.screen->inputs), ==, 0);

	/*
	 * The same list switched to title only replays all of it: a clock
	 * that moved the picture no longer blocks it.
	 */
	{
		g_autoptr(JsonNode) switched = request(fixture.daemon,
			"steps.set_check", "{\"agent\": \"alpha\", \"name\": \"edit\", "
			"\"check\": \"title-only\"}");

		g_assert_cmpstr(json_object_get_string_member(payload_of(switched),
			"check"), ==, "title-only");
	}

	replay(&fixture, "edit", 0);
	g_assert_null(fixture.error);
	g_assert_cmpint(fixture.done, ==, 3);
	g_assert_cmpint(g_atomic_int_get(&fixture.screen->inputs), ==, 3);

	replay_teardown(&fixture);
}

/* Both halves match: every step is sent. */
static void
test_replay_sends_on_a_match(
	void
){
	ReplayFixture fixture = { 0 };
	g_autofree gchar *hash = screen_hash();

	replay_setup(&fixture);
	save_moves(&fixture, "edit", 4, hash, "Report - Editor",
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE);

	replay(&fixture, "edit", 0);
	g_assert_null(fixture.error);
	g_assert_cmpint(fixture.done, ==, 4);
	g_assert_cmpint(g_atomic_int_get(&fixture.screen->inputs), ==, 4);

	replay_teardown(&fixture);
}

/* Letting go of the screen mid-replay stops it at the next step. */
static void
test_replay_stops_when_released(
	void
){
	ReplayFixture fixture = { 0 };
	g_autofree gchar *hash = screen_hash();

	replay_setup(&fixture);
	save_moves(&fixture, "long", 10, hash, "Report - Editor",
		CLAWT_STEP_CHECK_PICTURE_AND_TITLE);

	replay(&fixture, "long", 1);
	g_assert_nonnull(fixture.error);
	g_assert_nonnull(strstr(fixture.error, "released"));

	/* At most the step in flight when it was let go, never the rest. */
	g_assert_cmpint(g_atomic_int_get(&fixture.screen->inputs), <=, 2);
	g_assert_cmpint(fixture.done, <, 10);

	replay_teardown(&fixture);
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
	g_test_add_func("/saved-steps/focused-title", test_the_focused_title);
	g_test_add_func("/saved-steps/screen-check", test_the_screen_check);
	g_test_add_func("/saved-steps/daemon/replay-title",
		test_replay_refuses_a_different_title);
	g_test_add_func("/saved-steps/daemon/replay-picture",
		test_replay_refuses_a_different_picture);
	g_test_add_func("/saved-steps/daemon/replay-match",
		test_replay_sends_on_a_match);
	g_test_add_func("/saved-steps/daemon/replay-released",
		test_replay_stops_when_released);

	return g_test_run();
}
