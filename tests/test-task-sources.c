/*
 * test-task-sources.c - A routine's run is still its routine's after a restart
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon kept the task -> routine/trigger map in a hash table, so a
 * run that spanned a restart lost it and its remaining turns were tagged
 * `task:<id>`.  "Restart" here is the thing that matters: a second
 * #ClawtTaskSources made from the same file, with nothing carried across
 * in memory.
 */

#include "clawtilla.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "clawt-test-util.h"

/* A sweep that knows no task at all, as a daemon just after a restart. */
static gboolean
knows_nothing(
	const gchar *task_id,
	gboolean    *known,
	gpointer     user_data
){
	*known = FALSE;
	return FALSE;
}

/* A sweep that knows every task, and finds them all finished. */
static gboolean
all_finished(
	const gchar *task_id,
	gboolean    *known,
	gpointer     user_data
){
	*known = TRUE;
	return FALSE;
}

/* Only "t-live" is still running. */
static gboolean
only_one_live(
	const gchar *task_id,
	gboolean    *known,
	gpointer     user_data
){
	*known = TRUE;
	return g_strcmp0(task_id, "t-live") == 0;
}

/*
 * The item's whole point: noted by one daemon, answered by the next.
 * Two kinds and two ids, because a table that kept one entry's fields
 * for every key would pass with one.
 */
static void
test_a_run_survives_a_restart(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	const ClawtUsageOrigin *origin;
	struct stat st;

	dir = g_dir_make_tmp("clawt-task-sources-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "state", "task-sources.tsv", NULL);

	{
		g_autoptr(ClawtTaskSources) before = clawt_task_sources_new(path);

		g_assert_cmpuint(clawt_task_sources_get_count(before), ==, 0);
		g_assert_true(clawt_task_sources_note(before, "t-1",
			CLAWT_USAGE_ORIGIN_ROUTINE, "morning-digest"));
		g_assert_true(clawt_task_sources_note(before, "t-2",
			CLAWT_USAGE_ORIGIN_TRIGGER, "github-push"));
	}

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	{
		g_autoptr(ClawtTaskSources) after = clawt_task_sources_new(path);

		g_assert_cmpuint(clawt_task_sources_get_count(after), ==, 2);

		origin = clawt_task_sources_lookup(after, "t-1");
		g_assert_nonnull(origin);
		g_assert_cmpint(origin->kind, ==, CLAWT_USAGE_ORIGIN_ROUTINE);
		g_assert_cmpstr(origin->id, ==, "morning-digest");

		origin = clawt_task_sources_lookup(after, "t-2");
		g_assert_nonnull(origin);
		g_assert_cmpint(origin->kind, ==, CLAWT_USAGE_ORIGIN_TRIGGER);
		g_assert_cmpstr(origin->id, ==, "github-push");

		g_assert_null(clawt_task_sources_lookup(after, "t-3"));
		g_assert_null(clawt_task_sources_lookup(after, NULL));
	}

	clawt_test_remove_tree(dir);
}

/*
 * Pruning after a restart must not drop what the restart is for.  Every
 * entry is for a task the new daemon does not know; only age may take
 * those.  A known, finished task's entry goes at once, and a running
 * one's stays.
 */
static void
test_pruning_keeps_what_a_restart_needs(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(ClawtTaskSources) sources = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	dir = g_dir_make_tmp("clawt-task-sources-XXXXXX", NULL);
	path = g_build_filename(dir, "task-sources.tsv", NULL);
	sources = clawt_task_sources_new(path);

	g_assert_true(clawt_task_sources_note(sources, "t-live",
		CLAWT_USAGE_ORIGIN_ROUTINE, "r"));
	g_assert_true(clawt_task_sources_note(sources, "t-done",
		CLAWT_USAGE_ORIGIN_ROUTINE, "r"));

	/* Unknown and recent: kept. */
	g_assert_cmpuint(clawt_task_sources_prune(sources, knows_nothing, NULL,
		now), ==, 0);
	g_assert_cmpuint(clawt_task_sources_get_count(sources), ==, 2);

	/* Unknown and older than the limit: dropped. */
	g_assert_cmpuint(clawt_task_sources_prune(sources, knows_nothing, NULL,
		now + CLAWT_TASK_SOURCES_MAX_AGE_SECONDS + 1), ==, 2);
	g_assert_cmpuint(clawt_task_sources_get_count(sources), ==, 0);

	/* Known: the finished one goes, the live one stays. */
	g_assert_true(clawt_task_sources_note(sources, "t-live",
		CLAWT_USAGE_ORIGIN_ROUTINE, "r"));
	g_assert_true(clawt_task_sources_note(sources, "t-done",
		CLAWT_USAGE_ORIGIN_ROUTINE, "r"));
	g_assert_cmpuint(clawt_task_sources_prune(sources, only_one_live, NULL,
		now), ==, 1);
	g_assert_nonnull(clawt_task_sources_lookup(sources, "t-live"));
	g_assert_null(clawt_task_sources_lookup(sources, "t-done"));

	/* And the file says so: the next daemon sees the pruned table. */
	{
		g_autoptr(ClawtTaskSources) after = clawt_task_sources_new(path);

		g_assert_cmpuint(clawt_task_sources_get_count(after), ==, 1);
	}

	g_assert_cmpuint(clawt_task_sources_prune(sources, all_finished, NULL,
		now), ==, 1);

	g_clear_object(&sources);
	clawt_test_remove_tree(dir);
}

/*
 * What the file cannot hold is refused, and a damaged line costs that
 * line only.
 */
static void
test_bad_input_is_refused_and_a_bad_line_skipped(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(ClawtTaskSources) sources = NULL;

	dir = g_dir_make_tmp("clawt-task-sources-XXXXXX", NULL);
	path = g_build_filename(dir, "task-sources.tsv", NULL);

	g_assert_true(g_file_set_contents(path,
		"t-1\troutine\tdigest\t100\n"
		"garbage\n"
		"t-2\tagent\toryx\t100\n"
		"t-3\ttrigger\thook\t100\n", -1, NULL));

	/* The two skipped lines warn; that is the behaviour under test. */
	g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*line 2*");
	g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*line 3*");
	sources = clawt_task_sources_new(path);
	g_test_assert_expected_messages();

	g_assert_cmpuint(clawt_task_sources_get_count(sources), ==, 2);
	g_assert_nonnull(clawt_task_sources_lookup(sources, "t-1"));
	g_assert_nonnull(clawt_task_sources_lookup(sources, "t-3"));

	g_assert_false(clawt_task_sources_note(sources, "t\t4",
		CLAWT_USAGE_ORIGIN_ROUTINE, "r"));
	g_assert_false(clawt_task_sources_note(sources, "t-4",
		CLAWT_USAGE_ORIGIN_ROUTINE, "r\nx"));
	g_assert_false(clawt_task_sources_note(sources, "t-4",
		CLAWT_USAGE_ORIGIN_AGENT, "oryx"));
	g_assert_cmpuint(clawt_task_sources_get_count(sources), ==, 2);

	g_clear_object(&sources);
	clawt_test_remove_tree(dir);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/task-sources/a-run-survives-a-restart",
		test_a_run_survives_a_restart);
	g_test_add_func("/task-sources/pruning-keeps-what-a-restart-needs",
		test_pruning_keeps_what_a_restart_needs);
	g_test_add_func("/task-sources/bad-input-refused-bad-line-skipped",
		test_bad_input_is_refused_and_a_bad_line_skipped);

	return g_test_run();
}
