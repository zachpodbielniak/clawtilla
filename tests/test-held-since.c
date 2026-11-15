/*
 * test-held-since.c - The in-memory lists say how far back they reach
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Tasks are held in memory by design (docs/orchestration.org), and the
 * incidents that describe them with them: an incident kept past its task
 * would be a row whose Retry has nothing to retry and that no task
 * settling would ever resolve.  So a daemon restart clears both, and an
 * operator opening Tasks straight after one saw empty lists -- which
 * read as nothing ever having been delegated or broken.
 *
 * The honest fix is to say so: task.list and incident.list carry the
 * daemon's start (`since`), and every client words it through
 * clawt_held_since_caption()'s one sentence, which also says where the
 * history went (the room transcripts, which persist).
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

	fixture->dir = g_dir_make_tmp("clawt-since-XXXXXX", NULL);
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

/* One request with no payload, straight into the daemon's handler. */
static JsonNode *
ask(
	DaemonFixture *fixture,
	const gchar   *kind
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

	clawt_ipc_frame_set_payload(frame, clawt_ipc_payload_new(NULL, NULL));
	return clawt_daemon_handle_request(fixture->daemon, frame);
}

static const gchar AGENTS[] =
	"agents:\n"
	"  - id: watcher\n"
	"    model:\n      provider: claude-code\n"
	"    runtime:\n      autostart: false\n";

/* Both lists name the start, and it is this daemon's. */
static void
test_both_lists_say_since_when(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) task_reply = NULL;
	g_autoptr(JsonNode) incident_reply = NULL;
	gint64 before = g_get_real_time() / G_USEC_PER_SEC;
	JsonObject *root;

	daemon_setup(&fixture, AGENTS);

	g_assert_cmpint(fixture.daemon->started_at, >=, before);
	g_assert_cmpint(fixture.daemon->started_at, <=,
	                g_get_real_time() / G_USEC_PER_SEC);

	task_reply = ask(&fixture, "task.list");
	g_assert_false(clawt_ipc_frame_is_error(task_reply));
	root = clawt_ipc_frame_get_payload(task_reply);
	g_assert_nonnull(root);
	g_assert_cmpint(clawt_ipc_payload_int(root, "since", 0), ==,
	                fixture.daemon->started_at);

	incident_reply = ask(&fixture, "incident.list");
	g_assert_false(clawt_ipc_frame_is_error(incident_reply));
	root = clawt_ipc_frame_get_payload(incident_reply);
	g_assert_nonnull(root);
	g_assert_cmpint(clawt_ipc_payload_int(root, "since", 0), ==,
	                fixture.daemon->started_at);

	daemon_teardown(&fixture);
}

/*
 * The sentence: when, today or not, and always where the rest went.  A
 * daemon that did not say (no `since`) is still described honestly.
 */
static void
test_the_caption_says_what_the_list_covers(void)
{
	g_autoptr(GDateTime) morning = g_date_time_new_local(2026, 11, 14, 9, 14,
	                                                     0.0);
	g_autoptr(GDateTime) evening = g_date_time_new_local(2026, 11, 14, 21,
	                                                     40, 0.0);
	g_autoptr(GDateTime) next_week = g_date_time_new_local(2026, 11, 21, 8,
	                                                       0, 0.0);
	gint64 since = g_date_time_to_unix(morning);
	g_autofree gchar *today = clawt_held_since_caption(
		since, g_date_time_to_unix(evening));
	g_autofree gchar *later = clawt_held_since_caption(
		since, g_date_time_to_unix(next_week));
	g_autofree gchar *unknown = clawt_held_since_caption(
		0, g_date_time_to_unix(evening));

	g_assert_true(g_str_has_prefix(today, "Since the daemon started at 09:14."));
	g_assert_true(g_str_has_prefix(later,
	                               "Since the daemon started on 14 Nov at 09:14."));
	g_assert_true(g_str_has_prefix(unknown, "Since the daemon last started."));

	/* each one says why, and where the history is */
	g_assert_nonnull(strstr(today, "held in memory"));
	g_assert_nonnull(strstr(today, "restart clears"));
	g_assert_nonnull(strstr(today, "room transcripts"));
	g_assert_nonnull(strstr(unknown, "room transcripts"));
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/held-since/both-lists-say-since",
	                test_both_lists_say_since_when);
	g_test_add_func("/held-since/caption",
	                test_the_caption_says_what_the_list_covers);

	return g_test_run();
}
