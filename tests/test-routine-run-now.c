/*
 * test-routine-run-now.c - A person's Run now is a person's action
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A routine's prompt is the same text on every run, by construction.
 * The loop guard's cycle detector refuses an exact repeat inside
 * `orchestration.cycle_seconds` (300 by default), so pressing Run now a
 * second time within five minutes was refused as the conversation
 * "going in circles" -- and the refusal failed the run's task, which
 * filed it as an incident: a problem to look at, about a button the
 * operator had just pressed twice.
 *
 * Held here at both ends: the guard passes a message marked by hand and
 * still refuses the same text unmarked, and the daemon runs two Run nows
 * back to back, then refuses a third on the rate limit -- which every
 * person's action is still measured by -- without filing anything.
 *
 * The daemon half includes core/clawt-daemon-private.h, as
 * test-routine-read-only does.
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

static void
daemon_setup(
	DaemonFixture *fixture,
	const gchar   *extra
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-runnow-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, extra);
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

/* One routine.run, straight into the daemon's handler. */
static JsonNode *
run_now(
	DaemonFixture *fixture,
	const gchar   *routine
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new("routine.run", "t1");

	clawt_ipc_frame_set_payload(frame,
		clawt_ipc_payload_new("id", routine, NULL));
	return clawt_daemon_handle_request(fixture->daemon, frame);
}

/* How many incidents the desk holds, resolved or not. */
static guint
incident_count(
	DaemonFixture *fixture
){
	g_autoptr(GPtrArray) incidents = NULL;

	if (fixture->daemon->incidents == NULL)
		return 0;

	incidents = clawt_incident_desk_list(fixture->daemon->incidents);
	return (incidents != NULL) ? incidents->len : 0;
}

static const gchar AGENTS[] =
	"agents:\n"
	"  - id: watcher\n"
	"    model:\n      provider: claude-code\n"
	"    runtime:\n      autostart: false\n"
	"routines:\n"
	"  - id: brief\n    agent: watcher\n    instructions: \"Summarise the day.\"\n"
	"    schedule: manual\n";

/*
 * The guard itself: the same text twice inside the window is refused,
 * unless a person's action sent it.
 */
static void
test_the_cycle_detector_passes_a_person(void)
{
	g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
	g_autoptr(ClawtMessage) first = NULL;
	g_autoptr(ClawtMessage) again = NULL;
	g_autoptr(ClawtMessage) pressed = NULL;
	g_autoptr(ClawtMessage) pressed_again = NULL;
	g_autoptr(GError) error = NULL;

	clawt_loop_guard_set_limits(guard, 8, 0, 16);
	clawt_loop_guard_set_cycle_seconds(guard, 300);

	/* unmarked: the second is the loop the detector exists for */
	first = clawt_message_new("dm:user:watcher", "user", "Summarise the day.");
	again = clawt_message_new("dm:user:watcher", "user", "Summarise the day.");
	g_assert_true(clawt_loop_guard_check_in_room(guard, first, 0, TRUE,
	                                             &error));
	g_assert_no_error(error);
	g_assert_false(clawt_loop_guard_check_in_room(guard, again, 0, TRUE,
	                                              &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_clear_error(&error);

	/* marked by hand: a person pressing twice is not a loop */
	pressed = clawt_message_new("dm:user:other", "user", "Run the brief.");
	pressed_again = clawt_message_new("dm:user:other", "user",
	                                  "Run the brief.");
	clawt_message_set_by_hand(pressed, TRUE);
	clawt_message_set_by_hand(pressed_again, TRUE);
	g_assert_true(clawt_message_get_by_hand(pressed));
	g_assert_true(clawt_loop_guard_check_in_room(guard, pressed, 0, TRUE,
	                                             &error));
	g_assert_true(clawt_loop_guard_check_in_room(guard, pressed_again, 0,
	                                             TRUE, &error));
	g_assert_no_error(error);

	/* and the mark survives the copy the router guards with */
	{
		g_autoptr(ClawtMessage) copy = clawt_message_copy(pressed);

		g_assert_true(clawt_message_get_by_hand(copy));
	}
}

/*
 * Run now, and Run now again a moment later: two runs, two tasks, and
 * nothing filed.
 */
static void
test_run_now_twice_runs_twice(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) second = NULL;
	g_autoptr(GPtrArray) tasks = NULL;
	guint i;

	daemon_setup(&fixture, AGENTS);

	first = run_now(&fixture, "brief");
	second = run_now(&fixture, "brief");

	g_assert_false(clawt_ipc_frame_is_error(first));
	g_assert_false(clawt_ipc_frame_is_error(second));

	tasks = clawt_task_manager_list(fixture.daemon->tasks, "watcher", TRUE);
	g_assert_cmpuint(tasks->len, ==, 2);

	for (i = 0; i < tasks->len; i++)
		g_assert_cmpint(clawt_task_get_state(g_ptr_array_index(tasks, i)),
		                !=, CLAWT_TASK_FAILED);

	g_assert_cmpuint(incident_count(&fixture), ==, 0);

	daemon_teardown(&fixture);
}

/*
 * A refusal is an answer, not an incident.  The rate limit still
 * measures a person's action, so a second Run now inside the minute is
 * refused -- to the person, as the reply -- and its task is cancelled
 * with the reason rather than failed into the incident desk.
 */
static void
test_a_refused_run_now_is_not_an_incident(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *extra = NULL;
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) second = NULL;
	g_autoptr(GPtrArray) tasks = NULL;
	g_autoptr(GError) refusal = NULL;
	guint cancelled = 0;
	guint i;

	extra = g_strdup_printf("orchestration:\n  rate_limit_per_minute: 1\n%s",
	                        AGENTS);
	daemon_setup(&fixture, extra);

	first = run_now(&fixture, "brief");
	second = run_now(&fixture, "brief");

	g_assert_false(clawt_ipc_frame_is_error(first));
	g_assert_true(clawt_ipc_frame_is_error(second));

	refusal = clawt_ipc_frame_to_error(second);
	g_assert_nonnull(refusal);
	g_assert_nonnull(strstr(refusal->message, "in the last minute"));

	tasks = clawt_task_manager_list(fixture.daemon->tasks, "watcher", TRUE);

	for (i = 0; i < tasks->len; i++) {
		ClawtTask *task = g_ptr_array_index(tasks, i);

		g_assert_cmpint(clawt_task_get_state(task), !=, CLAWT_TASK_FAILED);

		if (clawt_task_get_state(task) == CLAWT_TASK_CANCELLED) {
			cancelled++;
			g_assert_nonnull(strstr(clawt_task_get_reason(task),
			                        "in the last minute"));
		}
	}

	g_assert_cmpuint(cancelled, ==, 1);
	g_assert_cmpuint(incident_count(&fixture), ==, 0);

	daemon_teardown(&fixture);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/routine-run-now/cycle-detector-passes-a-person",
	                test_the_cycle_detector_passes_a_person);
	g_test_add_func("/routine-run-now/twice-runs-twice",
	                test_run_now_twice_runs_twice);
	g_test_add_func("/routine-run-now/refusal-is-not-an-incident",
	                test_a_refused_run_now_is_not_an_incident);

	return g_test_run();
}
