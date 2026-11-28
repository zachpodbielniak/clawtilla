/*
 * test-board-daemon.c - task.board through a real daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * test-board.c pins the board's rules against a fixture source.  This
 * asks a started daemon, the way every client does, so the daemon's own
 * source is covered: who a team's board belongs to, the lanes from the
 * config's team, decisions from the inbox, a phase reported through the
 * task manager, the refusals, and the CLI's text drawn from the reply.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
} DaemonFixture;

static const gchar FLEET[] =
	"teams:\n"
	"  - id: forge\n"
	"    description: the coders\n"
	"  - id: empty\n"
	"agents:\n"
	"  - id: oxpecker\n"
	"    team: forge\n"
	"    team_role: lead\n"
	"    model:\n      provider: claude-code\n"
	"    runtime:\n      autostart: false\n"
	"  - id: wildebuild\n"
	"    team: forge\n"
	"    model:\n      provider: claude-code\n"
	"    runtime:\n      autostart: false\n"
	"  - id: leopard\n"
	"    model:\n      provider: claude-code\n"
	"    runtime:\n      autostart: false\n";

static void
daemon_setup(
	DaemonFixture *fixture
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-board-daemon-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, FLEET);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_teardown(
	DaemonFixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);
	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_clear_pointer(&fixture->context, g_main_context_unref);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

/* task.board with @payload, straight into the daemon's handler. */
static JsonNode *
ask_board(
	DaemonFixture *fixture,
	JsonNode      *payload
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new("task.board", "b1");

	clawt_ipc_frame_set_payload(frame, payload);
	return clawt_daemon_handle_request(fixture->daemon, frame);
}

static gint
error_code(JsonNode *reply)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(clawt_ipc_frame_is_error(reply));
	error = clawt_ipc_frame_to_error(reply);
	g_assert_nonnull(error);
	return error->code;
}

/* The ids of column @id's cards, comma-separated, in order. */
static gchar *
column_ids(JsonObject *board, const gchar *id)
{
	JsonArray *columns = json_object_get_array_member(board, "columns");
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;
	guint j;

	for (i = 0; i < json_array_get_length(columns); i++) {
		JsonObject *column = json_array_get_object_element(columns, i);
		JsonArray *cards;

		if (g_strcmp0(json_object_get_string_member(column, "id"), id) != 0)
			continue;

		cards = json_object_get_array_member(column, "cards");

		for (j = 0; j < json_array_get_length(cards); j++)
			g_string_append_printf(out, "%s%s", j > 0 ? "," : "",
			                       json_object_get_string_member(
			                           json_array_get_object_element(cards, j),
			                           "id"));
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

static void
test_board_from_the_daemon(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(JsonNode) by_team = NULL;
	g_autoptr(ClawtDecision) decision = NULL;
	g_autofree gchar *decision_id = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *backlog = NULL;
	g_autofree gchar *gating = NULL;
	g_autofree gchar *delegated = NULL;
	g_autofree gchar *blocked = NULL;
	ClawtTaskManager *tasks;
	ClawtTask *queued;
	ClawtTask *gate;
	ClawtTask *fan;
	ClawtTask *asked;
	ClawtTask *child;
	JsonObject *board;
	JsonArray *lanes;
	g_autoptr(GError) error = NULL;

	daemon_setup(&fixture);
	tasks = fixture.daemon->tasks;

	/* One queued, one gating by its own word, one delegated, one asked. */
	queued = clawt_task_manager_create(tasks, "chief", "oxpecker",
	                                   "Fix bug one", NULL, &error);
	g_assert_no_error(error);
	gate = clawt_task_manager_create(tasks, "chief", "oxpecker",
	                                 "Fix bug two", NULL, &error);
	g_assert_true(clawt_task_manager_note_progress_in_phase(
		tasks, clawt_task_get_id(gate), "make test", CLAWT_TASK_PHASE_GATING));
	fan = clawt_task_manager_create(tasks, "chief", "oxpecker",
	                                "Fix bug three", NULL, &error);
	g_assert_true(clawt_task_manager_note_progress(tasks,
	                                               clawt_task_get_id(fan),
	                                               "handed the parser on"));
	child = clawt_task_manager_create(tasks, "oxpecker", "wildebuild",
	                                  "Rebuild the parser",
	                                  clawt_task_get_id(fan), &error);
	g_assert_no_error(error);
	g_assert_true(clawt_task_manager_start(tasks, clawt_task_get_id(child)));
	asked = clawt_task_manager_create(tasks, "chief", "oxpecker",
	                                  "Fix bug four", NULL, &error);
	g_assert_true(clawt_task_manager_start(tasks, clawt_task_get_id(asked)));

	/* An operator decision on the last, from the inbox the daemon keeps. */
	decision = clawt_decision_new(NULL, "oxpecker", "Ship it now?");
	clawt_decision_set_task(decision, clawt_task_get_id(asked));
	decision_id = clawt_decision_store_post(fixture.daemon->decisions,
	                                        decision, &error);
	g_assert_no_error(error);
	g_assert_nonnull(decision_id);

	reply = ask_board(&fixture, clawt_ipc_payload_new("agent", "oxpecker",
	                                                  "window", "6h", NULL));
	g_assert_false(clawt_ipc_frame_is_error(reply));
	board = clawt_ipc_frame_get_payload(reply);

	backlog = column_ids(board, "backlog");
	gating = column_ids(board, "gating");
	delegated = column_ids(board, "delegated");
	blocked = column_ids(board, "blocked");

	g_assert_cmpstr(backlog, ==, clawt_task_get_id(queued));
	g_assert_cmpstr(gating, ==, clawt_task_get_id(gate));
	g_assert_cmpstr(delegated, ==, clawt_task_get_id(fan));
	g_assert_cmpstr(blocked, ==, clawt_task_get_id(asked));
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(board, "window"), "spec"), ==, "6h");
	g_assert_cmpint(json_object_get_int_member(board, "quiet_minutes"), ==,
	                20);
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(board, "header"), "state"), ==,
		"stopped");
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(board, "totals"), "listed"), ==, 4);

	/* Lanes: the agent's own team, from the config, then chip workers. */
	lanes = json_object_get_array_member(board, "lanes");
	g_assert_cmpuint(json_array_get_length(lanes), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(lanes, 0), "agent"), ==, "wildebuild");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(lanes, 0), "role"), ==, "member");

	/* The CLI's text, drawn from this reply. */
	text = clawt_board_render_text(board);
	g_assert_true(g_str_has_prefix(text, "oxpecker  stopped"));
	g_assert_nonnull(strstr(text, "blocked: 1 decision(s) for the operator"));
	g_assert_nonnull(strstr(text, "phase: gating"));
	g_assert_nonnull(strstr(text, "> wildebuild"));

	/* A team without an agent is its lead's board, with the team named. */
	by_team = ask_board(&fixture, clawt_ipc_payload_new("team", "forge",
	                                                    "timeline", "false",
	                                                    NULL));
	g_assert_false(clawt_ipc_frame_is_error(by_team));
	board = clawt_ipc_frame_get_payload(by_team);
	g_assert_cmpstr(json_object_get_string_member(board, "agent"), ==,
	                "oxpecker");
	g_assert_cmpstr(json_object_get_string_member(board, "team"), ==, "forge");

	daemon_teardown(&fixture);
}

static void
test_board_refusals(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) nobody = NULL;
	g_autoptr(JsonNode) unknown = NULL;
	g_autoptr(JsonNode) no_team = NULL;
	g_autoptr(JsonNode) leaderless = NULL;
	g_autoptr(JsonNode) bad_window = NULL;

	daemon_setup(&fixture);

	nobody = ask_board(&fixture, clawt_ipc_payload_new(NULL, NULL));
	g_assert_cmpint(error_code(nobody), ==, CLAWT_ERROR_INVALID_ARGUMENT);

	unknown = ask_board(&fixture, clawt_ipc_payload_new("agent", "nobody",
	                                                    NULL));
	g_assert_cmpint(error_code(unknown), ==, CLAWT_ERROR_NOT_FOUND);

	no_team = ask_board(&fixture, clawt_ipc_payload_new("team", "nowhere",
	                                                    NULL));
	g_assert_cmpint(error_code(no_team), ==, CLAWT_ERROR_NOT_FOUND);

	leaderless = ask_board(&fixture, clawt_ipc_payload_new("team", "empty",
	                                                       NULL));
	g_assert_cmpint(error_code(leaderless), ==, CLAWT_ERROR_INVALID_ARGUMENT);

	bad_window = ask_board(&fixture, clawt_ipc_payload_new(
		"agent", "oxpecker", "window", "last week", NULL));
	g_assert_cmpint(error_code(bad_window), ==, CLAWT_ERROR_INVALID_ARGUMENT);

	daemon_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/board-daemon/from-the-daemon",
	                test_board_from_the_daemon);
	g_test_add_func("/board-daemon/refusals", test_board_refusals);

	return g_test_run();
}
