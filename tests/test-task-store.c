/*
 * test-task-store.c - Tasks and incidents outlive the daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Tasks and incidents were held in memory, and a restart forgot them: a
 * delegator could no longer ask what became of its work, a parent forgot
 * its children, and the operator's lists came back empty.  These tests
 * pin the store underneath them:
 *
 *   - every field of a task survives a save and a reopen, owner history,
 *     completion hold and retry count included;
 *   - the journal records what happened, in order, and only when
 *     something did;
 *   - the live set, paging and pruning select the right rows;
 *   - the manager writes through, restores silently, forgets finished
 *     work from memory without losing it, and keeps its children index;
 *   - a restarted daemon keeps the tasks something is still carrying,
 *     fails the ones nothing is, and restores incidents without
 *     reporting them again;
 *   - what a task already spent is read back from the agents' ledgers.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

typedef struct {
	gchar          *dir;
	gchar          *path;
	ClawtTaskStore *store;
} StoreFixture;

static void
store_setup(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;

	(void)data;

	fixture->dir = g_dir_make_tmp("clawt-task-store-XXXXXX", NULL);
	fixture->path = g_build_filename(fixture->dir, "state", "tasks.db", NULL);
	fixture->store = clawt_task_store_new(fixture->path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->store);
}

static void
store_teardown(
	StoreFixture  *fixture,
	gconstpointer  data
){
	(void)data;

	g_clear_object(&fixture->store);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->path);
	g_free(fixture->dir);
}

/* Closes the store and opens the file again: what a restart sees. */
static void
reopen(
	StoreFixture *fixture
){
	g_autoptr(GError) error = NULL;

	g_clear_object(&fixture->store);
	fixture->store = clawt_task_store_new(fixture->path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->store);
}

/* A task with every field set to something other than its zero value. */
static ClawtTask *
full_task(void)
{
	ClawtTask *task = clawt_task_new("chief", "scout", "## Find the bug\n"
	                                 "in the export path");

	clawt_task_set_room(task, "dm:chief:scout");
	clawt_task_set_parent_id(task, "task-parent");
	clawt_task_set_reason(task, "it broke");
	clawt_task_set_model(task, "sonnet");
	clawt_task_set_result(task, "found it");
	clawt_task_set_progress_note(task, "halfway");
	clawt_task_set_cancelled_by(task, "user");
	clawt_task_set_thread_room(task, "task:x");
	clawt_task_set_consult(task, TRUE);
	clawt_task_set_depth(task, 2);
	clawt_task_set_title(task, "Export bug");
	clawt_task_set_reviewer(task, "chief");
	clawt_task_set_review_round(task, 2);
	clawt_task_set_result_inferred(task, TRUE);
	clawt_task_hold_completion(task);
	clawt_task_transfer_owner(task, "builder");
	clawt_task_set_state(task, CLAWT_TASK_RUNNING);

	return task;
}

static guint
handler_calls;

static void
count_changed(
	ClawtTaskManager *manager,
	const gchar      *task_id,
	gint              state,
	gpointer          user_data
){
	(void)manager;
	(void)task_id;
	(void)state;
	(void)user_data;

	handler_calls++;
}

/* ── ClawtTask on its own ────────────────────────────────────────── */

/* The JSON a task is stored as carries every field back. */
static void
test_json_round_trip_keeps_every_field(void)
{
	g_autoptr(ClawtTask) task = full_task();
	g_autoptr(JsonNode) node = clawt_task_to_json(task);
	g_autoptr(ClawtTask) back = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *owners;

	back = clawt_task_new_from_json(node, &error);
	g_assert_no_error(error);
	g_assert_nonnull(back);

	g_assert_cmpstr(clawt_task_get_id(back), ==, clawt_task_get_id(task));
	g_assert_cmpstr(clawt_task_get_origin(back), ==, "chief");
	g_assert_cmpstr(clawt_task_get_assignee(back), ==, "builder");
	g_assert_cmpstr(clawt_task_get_prompt(back), ==,
	                clawt_task_get_prompt(task));
	g_assert_cmpstr(clawt_task_get_room(back), ==, "dm:chief:scout");
	g_assert_cmpstr(clawt_task_get_parent_id(back), ==, "task-parent");
	g_assert_cmpstr(clawt_task_get_reason(back), ==, "it broke");
	g_assert_cmpstr(clawt_task_get_model(back), ==, "sonnet");
	g_assert_cmpstr(clawt_task_get_result(back), ==, "found it");
	g_assert_cmpstr(clawt_task_get_progress_note(back), ==, "halfway");
	g_assert_cmpstr(clawt_task_get_cancelled_by(back), ==, "user");
	g_assert_cmpstr(clawt_task_get_thread_room(back), ==, "task:x");
	g_assert_cmpstr(clawt_task_get_title(back), ==, "Export bug");
	g_assert_cmpstr(clawt_task_get_reviewer(back), ==, "chief");
	g_assert_cmpuint(clawt_task_get_review_round(back), ==, 2);
	g_assert_cmpstr(clawt_task_get_session_key(back), ==,
	                clawt_task_get_session_key(task));
	g_assert_true(clawt_task_get_consult(back));
	g_assert_true(clawt_task_get_result_inferred(back));
	g_assert_true(clawt_task_get_holds_completion(back));
	g_assert_cmpint(clawt_task_get_depth(back), ==, 2);
	g_assert_cmpint(clawt_task_get_state(back), ==, CLAWT_TASK_RUNNING);
	g_assert_cmpint(clawt_task_get_created_at(back), ==,
	                clawt_task_get_created_at(task));
	g_assert_cmpint(clawt_task_get_started_at(back), ==,
	                clawt_task_get_started_at(task));
	g_assert_cmpint(clawt_task_get_started_at(back), >, 0);

	/* the history, in order, with the assignee last */
	owners = clawt_task_get_owner_history(back);
	g_assert_cmpuint(owners->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(owners, 0), ==, "scout");
	g_assert_cmpstr(g_ptr_array_index(owners, 1), ==, "builder");
}

/*
 * A retry count and a finish time survive too, and an unknown state is
 * read as failed -- never as pending, which would be work run twice.
 */
static void
test_json_unknown_state_reads_as_failed(void)
{
	g_autoptr(ClawtTask) task = clawt_task_new("a", "b", "work");
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(ClawtTask) back = NULL;
	g_autoptr(ClawtTask) retried = NULL;
	g_autoptr(JsonNode) retried_node = NULL;

	clawt_task_set_state(task, CLAWT_TASK_FAILED);
	clawt_task_reopen(task, "user");
	clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
	retried_node = clawt_task_to_json(task);
	retried = clawt_task_new_from_json(retried_node, NULL);
	g_assert_cmpuint(clawt_task_get_retries(retried), ==, 1);
	g_assert_cmpstr(clawt_task_get_retried_by(retried), ==, "user");
	g_assert_cmpint(clawt_task_get_finished_at(retried), >, 0);

	node = clawt_task_to_json(task);
	json_object_set_string_member(json_node_get_object(node), "state",
	                              "pondering");

	g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
	                      "*pondering*reading it as failed*");
	back = clawt_task_new_from_json(node, NULL);
	g_test_assert_expected_messages();

	g_assert_cmpint(clawt_task_get_state(back), ==, CLAWT_TASK_FAILED);
}

/* A node that cannot be a task is refused, not repaired. */
static void
test_json_refuses_a_task_with_no_id_or_prompt(void)
{
	g_autoptr(JsonNode) node = json_from_string("{\"prompt\": \"x\"}", NULL);
	g_autoptr(JsonNode) array = json_from_string("[]", NULL);
	g_autoptr(GError) error = NULL;

	g_assert_null(clawt_task_new_from_json(node, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_null(clawt_task_new_from_json(array, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
}

/* The first line that says anything, without markdown markers. */
static void
test_title_from_prompt(void)
{
	g_autofree gchar *heading = clawt_task_title_from_prompt(
		"\n\n## Review the release notes\nthen post them");
	g_autofree gchar *bullet = clawt_task_title_from_prompt("- fix it");
	g_autofree gchar *empty = clawt_task_title_from_prompt("\n  \n");
	g_autofree gchar *none = clawt_task_title_from_prompt(NULL);
	g_autofree gchar *long_prompt = NULL;
	g_autofree gchar *cut = NULL;
	g_autoptr(ClawtTask) task = clawt_task_new("a", "b", "First line\nmore");

	g_assert_cmpstr(heading, ==, "Review the release notes");
	g_assert_cmpstr(bullet, ==, "fix it");
	g_assert_cmpstr(empty, ==, "");
	g_assert_cmpstr(none, ==, "");

	/* cut on a character boundary, never mid-sequence */
	long_prompt = g_strnfill(200, 'x');
	long_prompt[118] = '\0';
	{
		g_autofree gchar *with_wide = g_strconcat(long_prompt, "é", "tail",
		                                          NULL);

		cut = clawt_task_title_from_prompt(with_wide);
	}
	g_assert_true(g_utf8_validate(cut, -1, NULL));
	g_assert_cmpuint(strlen(cut), <=, 120);

	g_assert_cmpstr(clawt_task_get_title(task), ==, "First line");
	clawt_task_set_title(task, "Given");
	g_assert_cmpstr(clawt_task_get_title(task), ==, "Given");
	clawt_task_set_title(task, "");
	g_assert_cmpstr(clawt_task_get_title(task), ==, "First line");
}

/* ── The store ───────────────────────────────────────────────────── */

/* Saved, closed, reopened: the same task comes back. */
static void
test_store_survives_a_reopen(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(ClawtTask) task = full_task();
	g_autoptr(ClawtTask) back = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;

	g_assert_true(clawt_task_store_save(fixture->store, task, "made", &error));
	g_assert_no_error(error);

	reopen(fixture);

	back = clawt_task_store_get(fixture->store, clawt_task_get_id(task));
	g_assert_nonnull(back);
	g_assert_cmpstr(clawt_task_get_assignee(back), ==, "builder");
	g_assert_cmpstr(clawt_task_get_result(back), ==, "found it");
	g_assert_true(clawt_task_get_holds_completion(back));
	g_assert_cmpuint(clawt_task_get_owner_history(back)->len, ==, 2);

	g_assert_null(clawt_task_store_get(fixture->store, "task-nope"));
	g_assert_null(clawt_task_store_get(fixture->store, NULL));
}

/*
 * The journal: a row per state change and per named event, none for a
 * save that changed nothing the state machine cares about.
 */
static void
test_journal_records_what_happened(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(ClawtTask) task = clawt_task_new("chief", "scout", "work");
	g_autoptr(JsonNode) journal = NULL;
	JsonArray *rows;

	(void)data;

	g_assert_true(clawt_task_store_save(fixture->store, task, "created",
	                                    NULL));

	/* a field changed, no state change and no event: no row */
	clawt_task_set_model(task, "opus");
	g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));

	clawt_task_set_state(task, CLAWT_TASK_RUNNING);
	g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));

	clawt_task_set_reason(task, "the disk filled");
	clawt_task_set_state(task, CLAWT_TASK_FAILED);
	g_assert_true(clawt_task_store_save(fixture->store, task,
	                                    "the disk filled", NULL));

	journal = clawt_task_store_journal(fixture->store,
	                                   clawt_task_get_id(task), NULL);
	g_assert_nonnull(journal);
	rows = json_node_get_array(journal);
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);

	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "state"), ==, "pending");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "what"), ==, "created");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 1), "state"), ==, "running");
	g_assert_false(json_object_has_member(
		json_array_get_object_element(rows, 1), "what"));
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 2), "state"), ==, "failed");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 2), "what"), ==,
	                "the disk filled");

	/* a task with no history is an empty array, not an error */
	g_clear_pointer(&journal, json_node_unref);
	journal = clawt_task_store_journal(fixture->store, "task-none", NULL);
	g_assert_nonnull(journal);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(journal)), ==,
	                 0);
}

/* Writes a task that finished @seconds ago (or has not, at 0). */
static gchar *
save_aged(
	ClawtTaskStore *store,
	const gchar    *assignee,
	ClawtTaskState  state,
	gint64          created_at,
	gint64          finished_at
){
	g_autoptr(ClawtTask) task = clawt_task_new("chief", assignee, "work");
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(ClawtTask) aged = NULL;
	JsonObject *object;

	clawt_task_set_state(task, state);
	node = clawt_task_to_json(task);
	object = json_node_get_object(node);
	json_object_set_int_member(object, "created_at", created_at);
	json_object_set_int_member(object, "finished_at", finished_at);
	aged = clawt_task_new_from_json(node, NULL);

	g_assert_true(clawt_task_store_save(store, aged, NULL, NULL));
	return g_strdup(clawt_task_get_id(aged));
}

/* Live: unfinished at any age, finished only since the boundary. */
static void
test_live_set_and_pruning(
	StoreFixture  *fixture,
	gconstpointer  data
){
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	g_autofree gchar *old_open = save_aged(fixture->store, "a",
		CLAWT_TASK_PENDING, now - 400 * 86400, 0);
	g_autofree gchar *old_done = save_aged(fixture->store, "a",
		CLAWT_TASK_COMPLETED, now - 400 * 86400, now - 399 * 86400);
	g_autofree gchar *week_done = save_aged(fixture->store, "a",
		CLAWT_TASK_COMPLETED, now - 8 * 86400, now - 7 * 86400);
	g_autofree gchar *fresh_done = save_aged(fixture->store, "a",
		CLAWT_TASK_FAILED, now - 3600, now - 60);
	g_autoptr(GPtrArray) live = NULL;
	g_autoptr(JsonNode) journal = NULL;
	guint removed;

	(void)data;

	live = clawt_task_store_load_live(fixture->store, now - 86400, NULL);
	g_assert_nonnull(live);
	g_assert_cmpuint(live->len, ==, 2);

	/* oldest first: the order the manager lists from */
	g_assert_cmpstr(clawt_task_get_id(g_ptr_array_index(live, 0)), ==,
	                old_open);
	g_assert_cmpstr(clawt_task_get_id(g_ptr_array_index(live, 1)), ==,
	                fresh_done);

	/* a 180-day window takes the 400-day-old finished task only */
	removed = clawt_task_store_prune(fixture->store, now - 180 * 86400, NULL);
	g_assert_cmpuint(removed, ==, 1);
	g_assert_null(clawt_task_store_get(fixture->store, old_done));

	/* never an unfinished one, however old */
	{
		g_autoptr(ClawtTask) still_open = clawt_task_store_get(fixture->store,
		                                                       old_open);
		g_autoptr(ClawtTask) recent = clawt_task_store_get(fixture->store,
		                                                   week_done);

		g_assert_nonnull(still_open);
		g_assert_nonnull(recent);
	}

	/* its journal went with it */
	journal = clawt_task_store_journal(fixture->store, old_done, NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(journal)), ==,
	                 0);
}

/* Newest first, filtered, a page at a time. */
static void
test_list_pages_and_filters(
	StoreFixture  *fixture,
	gconstpointer  data
){
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	ClawtTaskQuery query = { NULL, -1, 0, 0 };
	g_autoptr(GPtrArray) first = NULL;
	g_autoptr(GPtrArray) second = NULL;
	g_autoptr(GPtrArray) failed = NULL;
	g_autoptr(GPtrArray) scouts = NULL;
	guint i;

	(void)data;

	for (i = 0; i < 5; i++) {
		g_autofree gchar *id = save_aged(
			fixture->store, (i % 2 == 0) ? "scout" : "builder",
			(i == 3) ? CLAWT_TASK_FAILED : CLAWT_TASK_COMPLETED,
			now - 1000 + (gint64)i * 10, now - 500);
	}

	query.limit = 2;
	first = clawt_task_store_list(fixture->store, &query, NULL);
	g_assert_cmpuint(first->len, ==, 2);
	g_assert_cmpint(clawt_task_get_created_at(g_ptr_array_index(first, 0)),
	                >, clawt_task_get_created_at(g_ptr_array_index(first, 1)));

	query.before = clawt_task_get_created_at(g_ptr_array_index(first, 1));
	second = clawt_task_store_list(fixture->store, &query, NULL);
	g_assert_cmpuint(second->len, ==, 2);
	g_assert_cmpint(clawt_task_get_created_at(g_ptr_array_index(second, 0)),
	                <, query.before);

	query.before = 0;
	query.limit = 0;
	query.state = CLAWT_TASK_FAILED;
	failed = clawt_task_store_list(fixture->store, &query, NULL);
	g_assert_cmpuint(failed->len, ==, 1);

	query.state = -1;
	query.assignee = "scout";
	scouts = clawt_task_store_list(fixture->store, &query, NULL);
	g_assert_cmpuint(scouts->len, ==, 3);
}

/* Incidents: written whole, read back in order, removable. */
static void
test_incidents_round_trip(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(JsonNode) one = json_from_string(
		"{\"id\": \"inc-1\", \"reason\": \"broke\"}", NULL);
	g_autoptr(JsonNode) two = json_from_string(
		"{\"id\": \"inc-2\", \"reason\": \"again\"}", NULL);
	g_autoptr(GPtrArray) back = NULL;

	(void)data;

	g_assert_true(clawt_task_store_save_incident(fixture->store, "inc-2", 20,
	                                             two, NULL));
	g_assert_true(clawt_task_store_save_incident(fixture->store, "inc-1", 10,
	                                             one, NULL));

	reopen(fixture);

	back = clawt_task_store_load_incidents(fixture->store, NULL);
	g_assert_cmpuint(back->len, ==, 2);
	g_assert_cmpstr(json_object_get_string_member(
		json_node_get_object(g_ptr_array_index(back, 0)), "id"), ==, "inc-1");

	g_assert_true(clawt_task_store_delete_incident(fixture->store, "inc-1",
	                                               NULL));
	g_clear_pointer(&back, g_ptr_array_unref);
	back = clawt_task_store_load_incidents(fixture->store, NULL);
	g_assert_cmpuint(back->len, ==, 1);
}

/* ── The manager over a store ────────────────────────────────────── */

/*
 * Every transition is written, a new manager restores the live set
 * without emitting, and the restored tree still cancels as a tree.
 */
static void
test_manager_writes_through_and_restores_silently(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(ClawtTaskManager) first = clawt_task_manager_new();
	g_autoptr(ClawtTaskManager) second = clawt_task_manager_new();
	g_autoptr(JsonNode) journal = NULL;
	g_autofree gchar *parent_id = NULL;
	g_autofree gchar *child_id = NULL;
	ClawtTask *parent;
	ClawtTask *child;
	guint loaded;

	(void)data;

	clawt_task_manager_set_store(first, fixture->store);
	parent = clawt_task_manager_create(first, "chief", "lead", "plan it",
	                                   NULL, NULL);
	parent_id = g_strdup(clawt_task_get_id(parent));
	child = clawt_task_manager_create(first, "lead", "scout", "look",
	                                  parent_id, NULL);
	child_id = g_strdup(clawt_task_get_id(child));
	clawt_task_manager_start(first, child_id);

	/* a field set outside the manager is saved when asked */
	clawt_task_set_room(child, "dm:lead:scout");
	clawt_task_manager_save(first, child, NULL);

	journal = clawt_task_manager_journal(first, child_id, NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(journal)), ==,
	                 2);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(json_node_get_array(journal), 0),
		"what"), ==, "created by lead for scout");

	g_clear_object(&first);
	reopen(fixture);

	clawt_task_manager_set_store(second, fixture->store);
	handler_calls = 0;
	g_signal_connect(second, "task-changed", G_CALLBACK(count_changed), NULL);

	loaded = clawt_task_manager_load(second, 0, NULL);
	g_assert_cmpuint(loaded, ==, 2);
	g_assert_cmpuint(handler_calls, ==, 0);

	child = clawt_task_manager_get(second, child_id);
	g_assert_nonnull(child);
	g_assert_cmpint(clawt_task_get_state(child), ==, CLAWT_TASK_RUNNING);
	g_assert_cmpstr(clawt_task_get_room(child), ==, "dm:lead:scout");
	g_assert_cmpint(clawt_task_get_depth(child), ==, 1);

	/* the children index was rebuilt: the parent counts its child */
	g_assert_cmpuint(clawt_task_manager_count_unfinished_children(
		second, parent_id), ==, 1);

	/* and cancelling the parent takes the child with it */
	g_assert_cmpuint(clawt_task_manager_cancel(second, parent_id, "stop",
	                                           "user"), ==, 2);
	g_assert_cmpint(clawt_task_get_state(clawt_task_manager_get(
		second, child_id)), ==, CLAWT_TASK_CANCELLED);
	g_assert_cmpuint(handler_calls, ==, 2);

	/* loading again adds nothing already held */
	g_assert_cmpuint(clawt_task_manager_load(second, 0, NULL), ==, 0);
}

/*
 * Forgetting finished work takes it out of the live listings only: it is
 * still answerable by id, and still in the history.
 */
static void
test_forgotten_tasks_stay_answerable(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
	g_autoptr(GPtrArray) live = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autofree gchar *done_id = NULL;
	g_autofree gchar *open_id = NULL;
	ClawtTask *task;
	gint64 future = g_get_real_time() / G_USEC_PER_SEC + 10;

	(void)data;

	clawt_task_manager_set_store(manager, fixture->store);
	task = clawt_task_manager_create(manager, "chief", "scout", "done work",
	                                 NULL, NULL);
	done_id = g_strdup(clawt_task_get_id(task));
	clawt_task_manager_complete(manager, done_id, "the answer");

	task = clawt_task_manager_create(manager, "chief", "scout", "open work",
	                                 NULL, NULL);
	open_id = g_strdup(clawt_task_get_id(task));

	g_assert_cmpuint(clawt_task_manager_forget_finished(manager, future), ==,
	                 1);

	live = clawt_task_manager_list(manager, NULL, TRUE);
	g_assert_cmpuint(live->len, ==, 1);
	g_assert_cmpstr(clawt_task_get_id(g_ptr_array_index(live, 0)), ==,
	                open_id);

	/* by id, from the store */
	task = clawt_task_manager_get(manager, done_id);
	g_assert_nonnull(task);
	g_assert_cmpstr(clawt_task_get_result(task), ==, "the answer");

	/* and asking did not put it back on the live list */
	g_clear_pointer(&live, g_ptr_array_unref);
	live = clawt_task_manager_list(manager, NULL, TRUE);
	g_assert_cmpuint(live->len, ==, 1);

	history = clawt_task_manager_list_history(manager, NULL, NULL);
	g_assert_cmpuint(history->len, ==, 2);
}

/* Without a store nothing is forgotten: there is nowhere to read it back. */
static void
test_without_a_store_nothing_is_forgotten(void)
{
	g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
	g_autoptr(GPtrArray) history = NULL;
	ClawtTask *task = clawt_task_manager_create(manager, "a", "b", "w", NULL,
	                                            NULL);

	clawt_task_manager_complete(manager, clawt_task_get_id(task), "r");
	g_assert_cmpuint(clawt_task_manager_forget_finished(
		manager, G_MAXINT64), ==, 0);
	g_assert_nonnull(clawt_task_manager_get(manager,
	                                        clawt_task_get_id(task)));

	history = clawt_task_manager_list_history(manager, NULL, NULL);
	g_assert_cmpuint(history->len, ==, 1);
	g_assert_null(clawt_task_manager_journal(manager,
	                                         clawt_task_get_id(task), NULL));
}

/* ── What a task already spent ───────────────────────────────────── */

/*
 * Two turns tagged with a task in the ledger, one with another task and
 * one untagged: only the first two are that task's spend.
 */
static void
test_task_spend_is_read_back(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-spend-XXXXXX", NULL);
	g_autofree gchar *sessions = g_build_filename(dir, "sessions", NULL);
	g_autofree gchar *db_path = g_build_filename(sessions, "libreclaw.db",
	                                             NULL);
	g_autofree gchar *ledger = NULL;
	g_autoptr(LcDatabase) db = LC_DATABASE(lc_sqlite_database_new());
	g_autoptr(GHashTable) ids = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GHashTable) spend = g_hash_table_new_full(g_str_hash,
	                                                    g_str_equal, g_free,
	                                                    NULL);
	g_autoptr(GError) error = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 costs[] = { 250000, 750000, 5000000, 1000000 };
	const gchar *tags[] = { "task-a", "task-a", "task-b", NULL };
	guint i;

	g_assert_cmpint(g_mkdir_with_parents(sessions, 0700), ==, 0);
	g_assert_true(lc_database_open(db, db_path, &error));

	for (i = 0; i < G_N_ELEMENTS(costs); i++)
		g_assert_true(lc_database_add_token_usage(db, "s", "clawtilla", "r",
			"test", "test", 1, 1, costs[i], now, &error));

	lc_database_close(db);
	ledger = clawt_usage_ledger_path_beside(db_path);

	/* row ids start at one, in the order they were added */
	for (i = 0; i < G_N_ELEMENTS(tags); i++) {
		g_autoptr(ClawtUsageOrigin) origin = NULL;

		if (tags[i] == NULL)
			continue;

		origin = clawt_usage_origin_new(CLAWT_USAGE_ORIGIN_TASK, tags[i]);
		g_free(origin->task_id);
		origin->task_id = g_strdup(tags[i]);
		g_assert_true(clawt_usage_ledger_record(ledger, (gint64)i + 1, now,
		                                        "r", origin, &error));
	}

	g_hash_table_add(ids, (gpointer)"task-a");
	g_assert_true(clawt_usage_task_spend(db_path, ids, spend, &error));
	g_assert_no_error(error);

	g_assert_cmpint((gint64)GPOINTER_TO_SIZE(g_hash_table_lookup(
		spend, "task-a")), ==, 1000000);
	g_assert_false(g_hash_table_contains(spend, "task-b"));

	/* an agent that never ran has nothing to add, and that is not a fault */
	g_hash_table_remove_all(spend);
	g_assert_true(clawt_usage_task_spend("/nonexistent/libreclaw.db", ids,
	                                     spend, &error));
	g_assert_cmpuint(g_hash_table_size(spend), ==, 0);

	clawt_test_remove_tree(dir);
}

/* ── A restarted daemon ──────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	gchar        *config;
	GMainContext *context;
	ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_start(
	DaemonFixture *fixture
){
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(fixture->config, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_stop(
	DaemonFixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;

	g_clear_pointer(&fixture->context, g_main_context_unref);
}

static void
daemon_setup(
	DaemonFixture *fixture
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-task-restart-XXXXXX", NULL);
	fixture->config = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: watcher\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir);
	g_assert_true(g_file_set_contents(fixture->config, yaml, -1, &error));
	daemon_start(fixture);
}

static void
daemon_teardown(
	DaemonFixture *fixture
){
	if (fixture->daemon != NULL)
		daemon_stop(fixture);

	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->config);
	g_free(fixture->dir);
}

/* One request straight into the daemon's handler. */
static JsonNode *
ask(
	DaemonFixture *fixture,
	const gchar   *kind,
	JsonNode      *payload
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

	clawt_ipc_frame_set_payload(frame, payload != NULL
		? payload : clawt_ipc_payload_new(NULL, NULL));
	return clawt_daemon_handle_request(fixture->daemon, frame);
}

/*
 * What a restart keeps, and what it fails:
 *
 *   - a parent waiting on its child: kept (the child carries it);
 *   - that child, whose assignee said it is not done: kept;
 *   - a task with nothing carrying it: failed, with a reason that says
 *     why and how to run it again;
 *   - a task that had already finished: as it was.
 */
static void
test_a_restart_keeps_carried_tasks_and_fails_the_rest(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *parent_id = NULL;
	g_autofree gchar *child_id = NULL;
	g_autofree gchar *orphan_id = NULL;
	g_autofree gchar *done_id = NULL;
	g_autofree gchar *thread_room = NULL;
	ClawtTask *task;

	daemon_setup(&fixture);

	task = clawt_task_manager_create(fixture.daemon->tasks, "user", "watcher",
	                                 "plan the release", NULL, NULL);
	parent_id = g_strdup(clawt_task_get_id(task));
	task = clawt_task_manager_create(fixture.daemon->tasks, "watcher",
	                                 "watcher", "check the notes",
	                                 parent_id, NULL);
	child_id = g_strdup(clawt_task_get_id(task));
	g_assert_true(clawt_task_manager_note_progress(fixture.daemon->tasks,
	                                               child_id,
	                                               "waiting on CI"));

	task = clawt_task_manager_create(fixture.daemon->tasks, "user", "watcher",
	                                 "nobody will do this", NULL, NULL);
	orphan_id = g_strdup(clawt_task_get_id(task));

	task = clawt_task_manager_create(fixture.daemon->tasks, "user", "watcher",
	                                 "already done", NULL, NULL);
	done_id = g_strdup(clawt_task_get_id(task));
	clawt_task_manager_complete(fixture.daemon->tasks, done_id, "yes");

	/* the parent works in a room of its own, which must come back */
	{
		ClawtTask *parent = clawt_task_manager_get(fixture.daemon->tasks,
		                                           parent_id);
		ClawtRoom *room = clawt_room_manager_get_task(fixture.daemon->rooms,
		                                              parent_id, "user",
		                                              "watcher");

		g_assert_nonnull(room);
		thread_room = g_strdup(clawt_room_get_id(room));
		clawt_task_set_thread_room(parent, thread_room);
		clawt_task_manager_save(fixture.daemon->tasks, parent, NULL);
	}

	daemon_stop(&fixture);
	daemon_start(&fixture);

	task = clawt_task_manager_get(fixture.daemon->tasks, parent_id);
	g_assert_nonnull(task);
	g_assert_false(clawt_task_is_finished(task));
	g_assert_cmpstr(clawt_task_get_thread_room(task), ==, thread_room);
	g_assert_nonnull(clawt_room_manager_get(fixture.daemon->rooms,
	                                        thread_room));

	task = clawt_task_manager_get(fixture.daemon->tasks, child_id);
	g_assert_nonnull(task);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);
	g_assert_cmpstr(clawt_task_get_progress_note(task), ==, "waiting on CI");

	task = clawt_task_manager_get(fixture.daemon->tasks, orphan_id);
	g_assert_nonnull(task);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_FAILED);
	g_assert_nonnull(strstr(clawt_task_get_reason(task),
	                        "the daemon restarted"));
	g_assert_true(clawt_task_state_is_retryable(clawt_task_get_state(task)));

	task = clawt_task_manager_get(fixture.daemon->tasks, done_id);
	g_assert_nonnull(task);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
	g_assert_cmpstr(clawt_task_get_result(task), ==, "yes");

	daemon_teardown(&fixture);
}

/*
 * task.list says the list is kept, and how long; history pages through
 * the store; task.show carries the journal when asked.
 */
static void
test_the_ipc_surface(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) live = NULL;
	g_autoptr(JsonNode) history = NULL;
	g_autoptr(JsonNode) shown = NULL;
	g_autoptr(JsonNode) bad = NULL;
	g_autofree gchar *task_id = NULL;
	JsonObject *root;
	JsonObject *object;
	ClawtTask *task;

	daemon_setup(&fixture);

	task = clawt_task_manager_create(fixture.daemon->tasks, "user", "watcher",
	                                 "## Ship it\nwith notes", NULL, NULL);
	task_id = g_strdup(clawt_task_get_id(task));
	clawt_task_manager_fail(fixture.daemon->tasks, task_id, "no notes");

	live = ask(&fixture, "task.list", NULL);
	g_assert_false(clawt_ipc_frame_is_error(live));
	root = clawt_ipc_frame_get_payload(live);
	g_assert_true(clawt_ipc_payload_boolean(root, "kept", FALSE));
	g_assert_cmpint(clawt_ipc_payload_int(root, "retention_days", 0), ==, 180);

	object = json_array_get_object_element(
		json_object_get_array_member(root, "tasks"), 0);
	g_assert_cmpstr(json_object_get_string_member(object, "title"), ==,
	                "Ship it");
	g_assert_cmpint(json_object_get_int_member(object, "created_at"), >, 0);
	g_assert_cmpint(json_object_get_int_member(object, "finished_at"), >, 0);

	history = ask(&fixture, "task.list", clawt_ipc_payload_new(
		"history", "true", "state", "failed", NULL));
	g_assert_false(clawt_ipc_frame_is_error(history));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		clawt_ipc_frame_get_payload(history), "tasks")), ==, 1);

	bad = ask(&fixture, "task.list", clawt_ipc_payload_new(
		"history", "true", "state", "pondering", NULL));
	g_assert_true(clawt_ipc_frame_is_error(bad));

	shown = ask(&fixture, "task.show", clawt_ipc_payload_new(
		"task", task_id, "journal", "true", NULL));
	g_assert_false(clawt_ipc_frame_is_error(shown));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		clawt_ipc_frame_get_payload(shown), "journal")), ==, 2);

	/* and the agent is told, in its TOOLS.org region, that tasks are kept */
	{
		g_autofree gchar *region = clawt_mcp_tools_describe_for_agent(
			fixture.daemon->mcp_tools, "watcher");

		g_assert_nonnull(strstr(region, "* Tasks are kept"));
		g_assert_nonnull(strstr(region, "clawtilla_task_status"));
	}

	daemon_teardown(&fixture);
}

/*
 * Without a store the region says nothing about tasks being kept: an
 * agent told they are would trust an id a restart already forgot.
 */
static void
test_no_store_no_promise(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *region = NULL;

	daemon_setup(&fixture);

	clawt_task_manager_set_store(fixture.daemon->tasks, NULL);
	region = clawt_mcp_tools_describe_for_agent(fixture.daemon->mcp_tools,
	                                            "watcher");
	g_assert_null(strstr(region, "* Tasks are kept"));

	daemon_teardown(&fixture);
}

/* The caption: kept, how long, and the in-memory sentence without a store. */
static void
test_the_list_caption(void)
{
	g_autofree gchar *kept = clawt_task_list_caption(1000, TRUE, 180, 2000);
	g_autofree gchar *forever = clawt_task_list_caption(1000, TRUE, 0, 2000);
	g_autofree gchar *one = clawt_task_list_caption(1000, TRUE, 1, 2000);
	g_autofree gchar *memory = clawt_task_list_caption(1000, FALSE, 180, 2000);

	g_assert_nonnull(strstr(kept, "finished in the last day"));
	g_assert_nonnull(strstr(kept, "for 180 days"));
	g_assert_nonnull(strstr(kept, "--history"));
	g_assert_nonnull(strstr(forever, "with no age limit"));
	g_assert_nonnull(strstr(one, "for 1 day "));
	g_assert_nonnull(strstr(memory, "held in memory"));
}

/*
 * An incident survives the restart as it was -- reported stays
 * reported -- and the desk does not report it again.
 */
static void
test_incidents_are_restored_without_a_second_report(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(GPtrArray) before = NULL;
	g_autoptr(GPtrArray) after = NULL;
	g_autofree gchar *incident_id = NULL;
	g_autofree gchar *task_id = NULL;
	gint64 reported_at;
	ClawtIncident *incident;
	ClawtTask *task;

	daemon_setup(&fixture);

	task = clawt_task_manager_create(fixture.daemon->tasks, "user", "watcher",
	                                 "break", NULL, NULL);
	task_id = g_strdup(clawt_task_get_id(task));
	clawt_task_manager_fail(fixture.daemon->tasks, task_id, "it broke");

	before = clawt_incident_desk_list(fixture.daemon->incidents);
	g_assert_cmpuint(before->len, ==, 1);
	incident = g_ptr_array_index(before, 0);
	incident_id = g_strdup(clawt_incident_get_id(incident));
	reported_at = clawt_incident_get_delivered_at(incident);

	daemon_stop(&fixture);
	daemon_start(&fixture);

	after = clawt_incident_desk_list(fixture.daemon->incidents);
	g_assert_cmpuint(after->len, ==, 1);
	incident = g_ptr_array_index(after, 0);
	g_assert_cmpstr(clawt_incident_get_id(incident), ==, incident_id);
	g_assert_cmpstr(clawt_incident_get_task_id(incident), ==, task_id);
	g_assert_cmpstr(clawt_incident_get_reason(incident), ==, "it broke");
	g_assert_cmpuint(clawt_incident_get_occurrences(incident), ==, 1);
	g_assert_cmpint(clawt_incident_get_delivered_at(incident), ==,
	                reported_at);

	daemon_teardown(&fixture);
}

/* ── The operator's own tasks (A2) ───────────────────────────────── */

/* Notes are kept, in order, and pruned with their task. */
static void
test_notes_are_kept_and_pruned(
	StoreFixture  *fixture,
	gconstpointer  data
){
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	g_autofree gchar *old = save_aged(fixture->store, "a",
		CLAWT_TASK_COMPLETED, now - 400 * 86400, now - 399 * 86400);
	g_autoptr(JsonNode) notes = NULL;
	JsonArray *rows;

	(void)data;

	g_assert_cmpint(clawt_task_store_add_note(fixture->store, old, "user",
	                                          CLAWT_TASK_NOTE_COMMENT,
	                                          "first", NULL), >, 0);
	g_assert_cmpint(clawt_task_store_add_note(fixture->store, old, NULL,
	                                          CLAWT_TASK_NOTE_RESULT,
	                                          "second", NULL), >, 0);

	notes = clawt_task_store_notes(fixture->store, old, NULL);
	rows = json_node_get_array(notes);
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "author"), ==, "user");
	g_assert_false(json_object_has_member(
		json_array_get_object_element(rows, 1), "author"));

	g_assert_cmpuint(clawt_task_store_prune(fixture->store,
	                                        now - 180 * 86400, NULL), ==, 1);
	g_clear_pointer(&notes, json_node_unref);
	notes = clawt_task_store_notes(fixture->store, old, NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(notes)), ==,
	                 0);
}

/* The manager writes progress and results into the thread itself. */
static void
test_progress_and_results_are_noted(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
	g_autoptr(JsonNode) notes = NULL;
	g_autofree gchar *id = NULL;
	JsonArray *rows;
	ClawtTask *task;

	(void)data;

	clawt_task_manager_set_store(manager, fixture->store);
	task = clawt_task_manager_create(manager, "chief", "scout", "look",
	                                 NULL, NULL);
	id = g_strdup(clawt_task_get_id(task));
	clawt_task_manager_note_progress(manager, id, "halfway");
	clawt_task_manager_complete(manager, id, "found it");

	notes = clawt_task_manager_notes(manager, id, NULL);
	rows = json_node_get_array(notes);
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "kind"), ==, "progress");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 1), "kind"), ==, "result");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 1), "author"), ==, "scout");

	/* an empty note is refused, an unknown task too */
	g_assert_cmpint(clawt_task_manager_add_note(manager, id, "user",
	                                            "comment", "", NULL), ==, 0);
	g_assert_cmpint(clawt_task_manager_add_note(manager, "task-none", "user",
	                                            "comment", "x", NULL), ==, 0);
}

/* How many queued items in @agent's mailbox carry @task_id. */
static guint
queued_for(
	DaemonFixture *fixture,
	const gchar   *agent,
	const gchar   *task_id
){
	ClawtAgent *found = clawt_agent_manager_get(fixture->daemon->agents,
	                                            agent);
	ClawtMailboxFilter filter = { -1, 0, TRUE };
	g_autoptr(GPtrArray) items = clawt_mailbox_list(
		clawt_agent_get_mailbox(found), &filter);
	guint n = 0;
	guint i;

	for (i = 0; items != NULL && i < items->len; i++)
		if (g_strcmp0(clawt_mailbox_item_get_task_id(
		                  g_ptr_array_index(items, i)), task_id) == 0)
			n++;

	return n;
}

/*
 * task.create, task.assign and task.comment end to end: the backlog,
 * the handover from "user" with the task's id on it, the comment kept
 * and heard, and the backlog surviving a restart untouched.
 */
static void
test_operator_tasks(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) created = NULL;
	g_autoptr(JsonNode) ghost = NULL;
	g_autoptr(JsonNode) direct = NULL;
	g_autoptr(JsonNode) assigned = NULL;
	g_autoptr(JsonNode) again = NULL;
	g_autoptr(JsonNode) comment = NULL;
	g_autoptr(JsonNode) backlog_comment = NULL;
	g_autoptr(JsonNode) shown = NULL;
	g_autofree gchar *backlog_id = NULL;
	g_autofree gchar *direct_id = NULL;
	g_autofree gchar *stays_id = NULL;
	JsonObject *task;
	ClawtTask *kept;

	daemon_setup(&fixture);

	/* the backlog */
	created = ask(&fixture, "task.create", clawt_ipc_payload_new(
		"prompt", "Write the release notes", "title", "Notes", NULL));
	g_assert_false(clawt_ipc_frame_is_error(created));
	task = json_object_get_object_member(
		clawt_ipc_frame_get_payload(created), "task");
	backlog_id = g_strdup(json_object_get_string_member(task, "id"));
	g_assert_cmpstr(json_object_get_string_member(task, "state"), ==,
	                "unassigned");
	g_assert_cmpstr(json_object_get_string_member(task, "title"), ==,
	                "Notes");
	g_assert_false(json_object_has_member(task, "assignee"));

	ghost = ask(&fixture, "task.create", clawt_ipc_payload_new(
		"prompt", "x", "agent", "ghost", NULL));
	g_assert_true(clawt_ipc_frame_is_error(ghost));

	/* straight to an agent: pending, and in its mailbox with the id */
	direct = ask(&fixture, "task.create", clawt_ipc_payload_new(
		"prompt", "Check the build", "agent", "watcher", NULL));
	g_assert_false(clawt_ipc_frame_is_error(direct));
	direct_id = g_strdup(json_object_get_string_member(
		json_object_get_object_member(clawt_ipc_frame_get_payload(direct),
		                              "task"), "id"));
	g_assert_cmpuint(queued_for(&fixture, "watcher", direct_id), ==, 1);

	/* assigned off the backlog, and delivered */
	assigned = ask(&fixture, "task.assign", clawt_ipc_payload_new(
		"task", backlog_id, "agent", "watcher", NULL));
	g_assert_false(clawt_ipc_frame_is_error(assigned));
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(
			clawt_ipc_frame_get_payload(assigned), "task"), "state"), ==,
		"pending");
	g_assert_cmpuint(queued_for(&fixture, "watcher", backlog_id), ==, 1);

	again = ask(&fixture, "task.assign", clawt_ipc_payload_new(
		"task", backlog_id, "agent", "watcher", NULL));
	g_assert_true(clawt_ipc_frame_is_error(again));

	/* a comment on work with its assignee is kept and heard */
	comment = ask(&fixture, "task.comment", clawt_ipc_payload_new(
		"task", direct_id, "text", "use the staging DB", NULL));
	g_assert_false(clawt_ipc_frame_is_error(comment));
	g_assert_true(clawt_ipc_payload_boolean(
		clawt_ipc_frame_get_payload(comment), "delivered", FALSE));
	g_assert_cmpuint(queued_for(&fixture, "watcher", direct_id), ==, 2);

	shown = ask(&fixture, "task.show", clawt_ipc_payload_new(
		"task", direct_id, NULL));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		clawt_ipc_frame_get_payload(shown), "notes")), ==, 1);

	/* and an agent asking about it sees the operator's word */
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(JsonNode) response = NULL;
		g_autofree gchar *request = g_strdup_printf(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
			"\"params\":{\"name\":\"clawtilla_task_status\","
			"\"arguments\":{\"task_id\":\"%s\"}}}", direct_id);
		JsonObject *result;
		const gchar *text;

		g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
		response = clawt_mcp_tools_call(fixture.daemon->mcp_tools, "watcher",
		                                NULL, json_parser_get_root(parser));
		result = json_object_get_object_member(
			json_node_get_object(response), "result");
		text = json_object_get_string_member(json_array_get_object_element(
			json_object_get_array_member(result, "content"), 0), "text");
		g_assert_false(json_object_get_boolean_member(result, "isError"));
		g_assert_nonnull(strstr(text, "the operator: use the staging DB"));
	}

	/* a backlog task keeps a comment and tells nobody */
	{
		g_autoptr(JsonNode) more = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Someday", NULL));

		stays_id = g_strdup(json_object_get_string_member(
			json_object_get_object_member(
				clawt_ipc_frame_get_payload(more), "task"), "id"));
	}

	backlog_comment = ask(&fixture, "task.comment", clawt_ipc_payload_new(
		"task", stays_id, "text", "when there is time", NULL));
	g_assert_false(clawt_ipc_payload_boolean(
		clawt_ipc_frame_get_payload(backlog_comment), "delivered", TRUE));

	/* a restart leaves the backlog as it was */
	daemon_stop(&fixture);
	daemon_start(&fixture);
	kept = clawt_task_manager_get(fixture.daemon->tasks, stays_id);
	g_assert_nonnull(kept);
	g_assert_cmpint(clawt_task_get_state(kept), ==, CLAWT_TASK_UNASSIGNED);

	daemon_teardown(&fixture);
}

/* ── Blockers (B1) ───────────────────────────────────────────────── */

/* Links: no self, no cycle, nothing to wait for once ended; what holds. */
static void
test_links_and_what_holds(
	StoreFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
	g_autoptr(GPtrArray) open = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	ClawtTask *task;

	(void)data;

	clawt_task_manager_set_store(manager, fixture->store);
	a = g_strdup(clawt_task_get_id(clawt_task_manager_create(
		manager, "chief", "scout", "first", NULL, NULL)));
	b = g_strdup(clawt_task_get_id(clawt_task_manager_create(
		manager, "chief", "builder", "second", NULL, NULL)));
	c = g_strdup(clawt_task_get_id(clawt_task_manager_create(
		manager, "chief", "builder", "third", NULL, NULL)));

	g_assert_true(clawt_task_manager_link(manager, b, a, &error));
	g_assert_no_error(error);
	g_assert_true(clawt_task_manager_link(manager, c, b, &error));

	/* a cycle, directly or further back, is refused by name */
	g_assert_false(clawt_task_manager_link(manager, a, c, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(clawt_task_manager_link(manager, a, a, &error));
	g_clear_error(&error);
	g_assert_false(clawt_task_manager_link(manager, a, "task-none", &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* held until completed: a failure still holds */
	open = clawt_task_manager_waiting_on(manager, b);
	g_assert_cmpuint(open->len, ==, 1);
	g_clear_pointer(&open, g_ptr_array_unref);

	clawt_task_manager_fail(manager, a, "broke");
	open = clawt_task_manager_waiting_on(manager, b);
	g_assert_cmpuint(open->len, ==, 1);
	g_clear_pointer(&open, g_ptr_array_unref);

	clawt_task_manager_retry(manager, a, "user", NULL);
	clawt_task_manager_complete(manager, a, "done");
	open = clawt_task_manager_waiting_on(manager, b);
	g_assert_cmpuint(open->len, ==, 0);
	g_clear_pointer(&open, g_ptr_array_unref);

	/* a finished task waits for nothing new */
	g_assert_false(clawt_task_manager_link(manager, a, c, &error));
	g_clear_error(&error);

	/* block and unblock: back to pending with an assignee */
	g_assert_true(clawt_task_manager_block(manager, c));
	task = clawt_task_manager_get(manager, c);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_BLOCKED);
	g_assert_true(clawt_task_manager_unblock(manager, c));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);
	g_assert_false(clawt_task_manager_unblock(manager, c));

	/* a held delivery is taken once */
	{
		g_autoptr(JsonNode) held = clawt_task_build_held_delivery(
			"router", "user", "builder", "do it", 0, 0, NULL);
		g_autoptr(JsonNode) first = NULL;
		g_autoptr(JsonNode) second = NULL;

		g_assert_true(clawt_task_manager_hold_delivery(manager, c, held,
		                                               NULL));
		first = clawt_task_manager_take_delivery(manager, c);
		second = clawt_task_manager_take_delivery(manager, c);
		g_assert_nonnull(first);
		g_assert_null(second);
	}

	/* unlinking takes it away */
	g_assert_true(clawt_task_manager_unlink(manager, c, b, NULL));
	{
		g_autoptr(GPtrArray) left = clawt_task_manager_blockers(manager, c);

		g_assert_cmpuint(left->len, ==, 0);
	}
}

/* The operator's task held for a blocker, then released, then failed. */
static void
test_operator_blockers(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) held = NULL;
	g_autoptr(JsonNode) backlog = NULL;
	g_autoptr(JsonNode) linked = NULL;
	g_autoptr(JsonNode) refused = NULL;
	g_autofree gchar *blocker = NULL;
	g_autofree gchar *waiter = NULL;
	g_autofree gchar *later = NULL;
	ClawtTask *task;

	daemon_setup(&fixture);

	first = ask(&fixture, "task.create", clawt_ipc_payload_new(
		"prompt", "Build it", "agent", "watcher", NULL));
	blocker = g_strdup(json_object_get_string_member(
		json_object_get_object_member(clawt_ipc_frame_get_payload(first),
		                              "task"), "id"));

	held = ask(&fixture, "task.create", clawt_ipc_payload_new(
		"prompt", "Ship it", "agent", "watcher", "after", blocker, NULL));
	g_assert_false(clawt_ipc_frame_is_error(held));
	waiter = g_strdup(json_object_get_string_member(
		json_object_get_object_member(clawt_ipc_frame_get_payload(held),
		                              "task"), "id"));

	task = clawt_task_manager_get(fixture.daemon->tasks, waiter);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_BLOCKED);
	g_assert_cmpuint(queued_for(&fixture, "watcher", waiter), ==, 0);

	/* the wire says what holds it */
	{
		g_autoptr(JsonNode) shown = ask(&fixture, "task.show",
			clawt_ipc_payload_new("task", waiter, NULL));
		JsonObject *object = json_object_get_object_member(
			clawt_ipc_frame_get_payload(shown), "task");

		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			object, "waiting_on")), ==, 1);
	}

	/* a restart keeps it held */
	daemon_stop(&fixture);
	daemon_start(&fixture);
	task = clawt_task_manager_get(fixture.daemon->tasks, waiter);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_BLOCKED);

	/* the blocker completes: it is handed over, as the operator's */
	clawt_task_manager_complete(fixture.daemon->tasks, blocker, "built");
	task = clawt_task_manager_get(fixture.daemon->tasks, waiter);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);
	g_assert_cmpuint(queued_for(&fixture, "watcher", waiter), ==, 1);

	/* work already handed over cannot be held back */
	refused = ask(&fixture, "task.link", clawt_ipc_payload_new(
		"task", waiter, "after", blocker, NULL));
	g_assert_true(clawt_ipc_frame_is_error(refused));

	/* a backlog task waits; its blocker failing leaves a note */
	backlog = ask(&fixture, "task.create", clawt_ipc_payload_new(
		"prompt", "Someday", NULL));
	later = g_strdup(json_object_get_string_member(
		json_object_get_object_member(clawt_ipc_frame_get_payload(backlog),
		                              "task"), "id"));
	linked = ask(&fixture, "task.link", clawt_ipc_payload_new(
		"task", later, "after", waiter, NULL));
	g_assert_false(clawt_ipc_frame_is_error(linked));
	task = clawt_task_manager_get(fixture.daemon->tasks, later);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_BLOCKED);

	clawt_task_manager_fail(fixture.daemon->tasks, waiter, "it broke");
	{
		g_autoptr(JsonNode) notes = clawt_task_manager_notes(
			fixture.daemon->tasks, later, NULL);

		g_assert_cmpuint(json_array_get_length(json_node_get_array(notes)),
		                 ==, 1);
		g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_BLOCKED);
	}

	/* taking the wait away sends it back to the backlog */
	{
		g_autoptr(JsonNode) unlinked = ask(&fixture, "task.unlink",
			clawt_ipc_payload_new("task", later, "after", waiter, NULL));

		g_assert_false(clawt_ipc_frame_is_error(unlinked));
		g_assert_cmpint(clawt_task_get_state(task), ==,
		                CLAWT_TASK_UNASSIGNED);
	}

	daemon_teardown(&fixture);
}

/* A tool call, as an agent: the same, plus who is told, and remove. */
static gchar *
call_as(
	DaemonFixture *fixture,
	const gchar   *agent,
	const gchar   *tool,
	const gchar   *arguments,
	gboolean      *is_error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *request = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);
	JsonObject *result;

	g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
	response = clawt_mcp_tools_call(fixture->daemon->mcp_tools, agent, NULL,
	                                json_parser_get_root(parser));
	result = json_object_get_object_member(json_node_get_object(response),
	                                       "result");
	*is_error = json_object_get_boolean_member(result, "isError");

	return g_strdup(json_object_get_string_member(
		json_array_get_object_element(
			json_object_get_array_member(result, "content"), 0), "text"));
}

static void
test_delegation_waits(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *blocker = NULL;
	g_autofree gchar *waiter = NULL;
	gboolean is_error = TRUE;
	ClawtTask *task;
	GPtrArray *all;
	guint i;

	fixture.dir = g_dir_make_tmp("clawt-blockers-XXXXXX", NULL);
	fixture.config = g_build_filename(fixture.dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: chief\n    chief_of_staff: true\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n"
		"  - id: watcher\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n",
		fixture.dir, fixture.dir, fixture.dir, fixture.dir);
	g_assert_true(g_file_set_contents(fixture.config, yaml, -1, &error));
	daemon_start(&fixture);

	blocker = g_strdup(clawt_task_get_id(clawt_task_manager_create(
		fixture.daemon->tasks, "user", "watcher", "build", NULL, NULL)));

	/* an unknown id refuses, and makes nothing */
	reply = call_as(&fixture, "chief", "clawtilla_delegate",
		"{\"agent_id\":\"watcher\",\"task\":\"ship\","
		"\"after\":\"task-none\"}", &is_error);
	g_assert_true(is_error);
	g_assert_nonnull(strstr(reply, "Nothing was delegated"));
	g_clear_pointer(&reply, g_free);

	args = g_strdup_printf("{\"agent_id\":\"watcher\",\"task\":\"ship it\","
	                       "\"after\":\"%s\"}", blocker);
	reply = call_as(&fixture, "chief", "clawtilla_delegate", args, &is_error);
	g_assert_false(is_error);
	g_assert_nonnull(strstr(reply, "held until"));

	/* the held task is the chief's newest */
	all = clawt_task_manager_list(fixture.daemon->tasks, "watcher", TRUE);
	for (i = 0; i < all->len; i++) {
		task = g_ptr_array_index(all, i);
		if (clawt_task_get_state(task) == CLAWT_TASK_BLOCKED)
			waiter = g_strdup(clawt_task_get_id(task));
	}
	g_ptr_array_unref(all);
	g_assert_nonnull(waiter);
	g_assert_cmpuint(queued_for(&fixture, "watcher", waiter), ==, 0);

	/* the blocker is cancelled: the chief is told, the task still waits */
	clawt_task_manager_cancel(fixture.daemon->tasks, blocker, "no", "user");
	task = clawt_task_manager_get(fixture.daemon->tasks, waiter);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_BLOCKED);
	g_assert_cmpuint(queued_for(&fixture, "chief", waiter), ==, 1);

	/* and stops waiting with the tool, which hands it over now */
	g_clear_pointer(&reply, g_free);
	g_clear_pointer(&args, g_free);
	args = g_strdup_printf("{\"task_id\":\"%s\",\"after\":\"%s\","
	                       "\"remove\":true}", waiter, blocker);
	reply = call_as(&fixture, "chief", "clawtilla_task_link", args, &is_error);
	g_assert_false(is_error);
	g_assert_nonnull(strstr(reply, "handed to watcher"));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);
	g_assert_cmpuint(queued_for(&fixture, "watcher", waiter), ==, 1);

	/* not on somebody else's task */
	g_clear_pointer(&reply, g_free);
	reply = call_as(&fixture, "watcher", "clawtilla_task_link", args,
	                &is_error);
	g_assert_true(is_error);

	daemon_teardown(&fixture);
}

/* ── Review (B2) ─────────────────────────────────────────────────── */

/* Who a review spelling names, and what it refuses. */
static void
test_reviewer_resolution(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
		"teams:\n  - id: build\n"
		"agents:\n"
		"  - id: chief\n    chief_of_staff: true\n"
		"  - id: lead\n    team: build\n    team_role: lead\n"
		"  - id: hand\n    team: build\n", &error);
	gchar *who;

	g_assert_no_error(error);

	g_assert_null(clawt_task_resolve_reviewer(config, NULL, "chief", "hand",
	                                          NULL));
	g_assert_null(clawt_task_resolve_reviewer(config, "none", "chief", "hand",
	                                          NULL));

	who = clawt_task_resolve_reviewer(config, "operator", "chief", "hand",
	                                  NULL);
	g_assert_cmpstr(who, ==, "user");
	g_free(who);

	who = clawt_task_resolve_reviewer(config, "delegator", "chief", "hand",
	                                  NULL);
	g_assert_cmpstr(who, ==, "chief");
	g_free(who);

	who = clawt_task_resolve_reviewer(config, "lead", "chief", "hand", NULL);
	g_assert_cmpstr(who, ==, "lead");
	g_free(who);

	/* the operator delegated it: the operator reviews it */
	who = clawt_task_resolve_reviewer(config, "delegator", "user", "hand",
	                                  NULL);
	g_assert_cmpstr(who, ==, "user");
	g_free(who);

	/* landing on the assignee by role means no review, quietly */
	g_assert_null(clawt_task_resolve_reviewer(config, "delegator", "hand",
	                                          "hand", &error));
	g_assert_no_error(error);

	/* naming the assignee or nobody outright is refused */
	g_assert_null(clawt_task_resolve_reviewer(config, "hand", "chief", "hand",
	                                          &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_null(clawt_task_resolve_reviewer(config, "ghost", "chief",
	                                          "hand", &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
}

/* The verdicts, the rounds and the escalation, in the manager. */
static void
test_review_rounds(void)
{
	g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
	g_autoptr(GError) error = NULL;
	ClawtTask *task = clawt_task_manager_create(tasks, "chief", "hand",
	                                            "build it", NULL, NULL);
	const gchar *id = clawt_task_get_id(task);

	clawt_task_set_reviewer(task, "chief");

	g_assert_true(clawt_task_manager_complete(tasks, id, "built"));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_REVIEW);
	g_assert_cmpstr(clawt_task_get_result(task), ==, "built");

	/* nobody else's verdict, and changes need notes */
	g_assert_cmpint(clawt_task_manager_review(tasks, id, "hand", TRUE, NULL,
	                                          2, &error), ==,
	                CLAWT_TASK_REVIEW_REFUSED);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(clawt_task_manager_review(tasks, id, "chief", FALSE, "",
	                                          2, &error), ==,
	                CLAWT_TASK_REVIEW_REFUSED);
	g_clear_error(&error);

	/* round one: back to its assignee */
	g_assert_cmpint(clawt_task_manager_review(tasks, id, "chief", FALSE,
	                                          "add a test", 2, &error), ==,
	                CLAWT_TASK_REVIEW_CHANGES);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);
	g_assert_cmpuint(clawt_task_get_review_round(task), ==, 1);

	/* round two is the limit: the operator decides now */
	g_assert_true(clawt_task_manager_complete(tasks, id, "built, tested"));
	g_assert_cmpint(clawt_task_manager_review(tasks, id, "chief", FALSE,
	                                          "and docs", 2, &error), ==,
	                CLAWT_TASK_REVIEW_ESCALATED);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_REVIEW);
	g_assert_cmpstr(clawt_task_get_reviewer(task), ==, "user");

	/* and only an accept completes it */
	g_assert_cmpint(clawt_task_manager_review(tasks, id, "user", TRUE, NULL,
	                                          2, &error), ==,
	                CLAWT_TASK_REVIEW_ACCEPTED);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
}

/* How many items in @agent's mailbox carry @task_id and say @needle. */
static guint
said_to(
	DaemonFixture *fixture,
	const gchar   *agent,
	const gchar   *task_id,
	const gchar   *needle
){
	ClawtAgent *found = clawt_agent_manager_get(fixture->daemon->agents,
	                                            agent);
	ClawtMailboxFilter filter = { -1, 0, TRUE };
	g_autoptr(GPtrArray) items = clawt_mailbox_list(
		clawt_agent_get_mailbox(found), &filter);
	guint n = 0;
	guint i;

	for (i = 0; items != NULL && i < items->len; i++) {
		ClawtMailboxItem *item = g_ptr_array_index(items, i);

		if (g_strcmp0(clawt_mailbox_item_get_task_id(item), task_id) == 0 &&
		    strstr(clawt_mailbox_item_get_body(item), needle) != NULL)
			n++;
	}

	return n;
}

/*
 * A delegation reviewed by its delegator, end to end through the tools;
 * then an operator review through the inbox and the IPC frame.
 */
static void
test_review_end_to_end(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *op_id = NULL;
	gboolean is_error = TRUE;
	ClawtTask *task;

	fixture.dir = g_dir_make_tmp("clawt-review-XXXXXX", NULL);
	fixture.config = g_build_filename(fixture.dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: chief\n    chief_of_staff: true\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n"
		"  - id: watcher\n    review_by: operator\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n",
		fixture.dir, fixture.dir, fixture.dir, fixture.dir);
	g_assert_true(g_file_set_contents(fixture.config, yaml, -1, &error));
	daemon_start(&fixture);

	reply = call_as(&fixture, "chief", "clawtilla_delegate",
		"{\"agent_id\":\"watcher\",\"task\":\"write it\","
		"\"review\":\"delegator\"}", &is_error);
	g_assert_false(is_error);
	{
		GPtrArray *all = clawt_task_manager_list(fixture.daemon->tasks,
		                                         "watcher", TRUE);

		id = g_strdup(clawt_task_get_id(g_ptr_array_index(all, 0)));
		g_ptr_array_unref(all);
	}
	task = clawt_task_manager_get(fixture.daemon->tasks, id);
	g_assert_cmpstr(clawt_task_get_reviewer(task), ==, "chief");

	/* finished: to the chief for review, not done */
	clawt_task_manager_complete(fixture.daemon->tasks, id, "written");
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_REVIEW);
	g_assert_cmpuint(said_to(&fixture, "chief", id, "waiting for your "
	                         "review"), ==, 1);

	/* a comment while it waits does not ask the reviewer again */
	{
		g_autoptr(JsonNode) commented = ask(&fixture, "task.comment",
			clawt_ipc_payload_new("task", id, "text", "looks close", NULL));

		g_assert_false(clawt_ipc_frame_is_error(commented));
		g_assert_cmpuint(said_to(&fixture, "chief", id, "waiting for your "
		                         "review"), ==, 1);
	}

	/* the assignee cannot review it; the chief sends it back */
	g_clear_pointer(&reply, g_free);
	args = g_strdup_printf("{\"task_id\":\"%s\",\"accept\":true}", id);
	reply = call_as(&fixture, "watcher", "clawtilla_task_review", args,
	                &is_error);
	g_assert_true(is_error);

	g_clear_pointer(&reply, g_free);
	g_clear_pointer(&args, g_free);
	args = g_strdup_printf("{\"task_id\":\"%s\",\"accept\":false,"
	                       "\"notes\":\"add a test\"}", id);
	reply = call_as(&fixture, "chief", "clawtilla_task_review", args,
	                &is_error);
	g_assert_false(is_error);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);
	g_assert_cmpuint(said_to(&fixture, "watcher", id, "add a test"), ==, 1);

	/* again, and accepted: no settle notice to the chief, who accepted */
	clawt_task_manager_complete(fixture.daemon->tasks, id, "written, tested");
	g_clear_pointer(&reply, g_free);
	g_clear_pointer(&args, g_free);
	args = g_strdup_printf("{\"task_id\":\"%s\",\"accept\":true}", id);
	reply = call_as(&fixture, "chief", "clawtilla_task_review", args,
	                &is_error);
	g_assert_false(is_error);
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
	g_assert_cmpuint(said_to(&fixture, "chief", id, "You delegated it"), ==, 0);

	/* the operator's review, from watcher's own review_by */
	{
		g_autoptr(JsonNode) made = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Check it", "agent", "watcher",
			                      NULL));
		g_autoptr(JsonNode) answered = NULL;
		g_autoptr(JsonNode) accepted = NULL;
		g_autofree gchar *decision = NULL;
		g_autoptr(ClawtDecision) open = NULL;

		op_id = g_strdup(json_object_get_string_member(
			json_object_get_object_member(clawt_ipc_frame_get_payload(made),
			                              "task"), "id"));
		clawt_task_manager_complete(fixture.daemon->tasks, op_id, "checked");
		task = clawt_task_manager_get(fixture.daemon->tasks, op_id);
		g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_REVIEW);

		decision = g_strconcat("review-", op_id, NULL);
		open = clawt_decision_store_get(fixture.daemon->decisions, decision);
		g_assert_nonnull(open);

		/* anything but accept in the inbox is the notes */
		answered = ask(&fixture, "decision.answer", clawt_ipc_payload_new(
			"decision", decision, "answer", "check the edge cases", NULL));
		g_assert_false(clawt_ipc_frame_is_error(answered));
		g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_PENDING);
		g_assert_cmpuint(said_to(&fixture, "watcher", op_id,
		                         "check the edge cases"), ==, 1);

		/* finished again; accepted from a client this time */
		clawt_task_manager_complete(fixture.daemon->tasks, op_id, "done");
		accepted = ask(&fixture, "task.review", clawt_ipc_payload_new(
			"task", op_id, "verdict", "accept", NULL));
		g_assert_false(clawt_ipc_frame_is_error(accepted));
		g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
	}

	daemon_teardown(&fixture);
}

/* ── Checkouts (C1) ──────────────────────────────────────────────── */

/* A one-commit repository under @dir, made without the user's config. */
static gchar *
checkout_repository(
	const gchar *dir
){
	gchar *repo = g_build_filename(dir, "src", "proj", NULL);
	g_autofree gchar *command = g_strdup_printf(
		"sh -c 'cd \"%s\" && export GIT_CONFIG_GLOBAL=/dev/null "
		"GIT_CONFIG_NOSYSTEM=1 && git -c init.defaultBranch=main init -q && "
		"echo one > README && git add README && git -c user.name=t "
		"-c user.email=t@example.org -c commit.gpgsign=false commit -q "
		"-m first'", repo);
	gint status = 1;

	g_assert_cmpint(g_mkdir_with_parents(repo, 0700), ==, 0);
	g_assert_true(g_spawn_command_line_sync(command, NULL, NULL, &status,
	                                        NULL));
	g_assert_cmpint(status, ==, 0);

	return repo;
}

/* The text of @agent's queued item for @task_id, or NULL. */
static gchar *
queued_body(
	DaemonFixture *fixture,
	const gchar   *agent,
	const gchar   *task_id
){
	ClawtAgent *found = clawt_agent_manager_get(fixture->daemon->agents,
	                                            agent);
	ClawtMailboxFilter filter = { -1, 0, TRUE };
	g_autoptr(GPtrArray) items = clawt_mailbox_list(
		clawt_agent_get_mailbox(found), &filter);
	guint i;

	for (i = 0; items != NULL && i < items->len; i++) {
		ClawtMailboxItem *item = g_ptr_array_index(items, i);

		if (g_strcmp0(clawt_mailbox_item_get_task_id(item), task_id) == 0)
			return g_strdup(clawt_mailbox_item_get_body(item));
	}

	return NULL;
}

/*
 * An operator's task with a repository, end to end: the checkout in the
 * assignee's workspace, the assignment saying where with the contract
 * still last, the wire object, removal refused while the work is under
 * way and done once it is not, and the hourly sweep.
 */
static void
test_operator_checkouts(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *git = g_find_program_in_path("git");
	g_autofree gchar *repo = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *later = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	ClawtTask *task;

	if (git == NULL) {
		g_test_skip("git is not installed");
		return;
	}

	daemon_setup(&fixture);
	repo = checkout_repository(fixture.dir);

	/* a repository without an assignee, or that is not one, refuses */
	{
		g_autoptr(JsonNode) backlog = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Fix it", "repo", repo, NULL));
		g_autoptr(JsonNode) missing = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Fix it", "agent", "watcher",
			                      "repo", "/nowhere/at/all", NULL));

		g_assert_true(clawt_ipc_frame_is_error(backlog));
		g_assert_true(clawt_ipc_frame_is_error(missing));
	}

	{
		g_autoptr(JsonNode) made = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Fix the build", "agent",
			                      "watcher", "repo", repo, NULL));
		JsonObject *object;
		JsonObject *checkout;

		g_assert_false(clawt_ipc_frame_is_error(made));
		object = json_object_get_object_member(
			clawt_ipc_frame_get_payload(made), "task");
		id = g_strdup(json_object_get_string_member(object, "id"));
		checkout = json_object_get_object_member(object, "checkout");
		g_assert_nonnull(checkout);
		g_assert_cmpstr(json_object_get_string_member(checkout, "mode"), ==,
		                "worktree");
		g_assert_true(json_object_get_boolean_member(checkout, "present"));
		path = g_strdup(json_object_get_string_member(checkout, "path"));
	}

	g_assert_true(g_str_has_suffix(path, id + 5));
	g_assert_nonnull(strstr(path, "/agents/watcher/tasks/"));
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_DIR));

	/* the assignment says where, and the contract still ends it */
	body = queued_body(&fixture, "watcher", id);
	g_assert_nonnull(body);
	g_assert_nonnull(strstr(body, path));
	{
		g_autofree gchar *contract = clawt_task_assignment_guidance(id);

		g_assert_true(g_str_has_suffix(body, contract));
	}

	/* not while it is being worked on */
	{
		g_autoptr(JsonNode) early = ask(&fixture, "task.checkout_remove",
			clawt_ipc_payload_new("task", id, NULL));

		g_assert_true(clawt_ipc_frame_is_error(early));
		g_assert_true(g_file_test(path, G_FILE_TEST_IS_DIR));
	}

	/* a restart keeps it */
	daemon_stop(&fixture);
	daemon_start(&fixture);
	task = clawt_task_manager_get(fixture.daemon->tasks, id);
	g_assert_cmpstr(clawt_task_get_checkout_path(task), ==, path);

	/* finished, then removed: gone, branch kept, said in its history */
	clawt_task_manager_complete(fixture.daemon->tasks, id, "fixed");
	{
		g_autoptr(JsonNode) removed = ask(&fixture, "task.checkout_remove",
			clawt_ipc_payload_new("task", id, NULL));
		g_autoptr(JsonNode) journal = clawt_task_manager_journal(
			fixture.daemon->tasks, id, NULL);
		JsonArray *rows = json_node_get_array(journal);
		gboolean said = FALSE;
		guint i;

		g_assert_false(clawt_ipc_frame_is_error(removed));
		g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
		g_assert_false(json_object_get_boolean_member(
			json_object_get_object_member(
				json_object_get_object_member(
					clawt_ipc_frame_get_payload(removed), "task"),
				"checkout"), "present"));

		for (i = 0; i < json_array_get_length(rows); i++) {
			JsonObject *row = json_array_get_object_element(rows, i);

			if (json_object_has_member(row, "what") &&
			    strstr(json_object_get_string_member(row, "what"),
			           "checkout removed") != NULL)
				said = TRUE;
		}
		g_assert_true(said);
	}

	/* the sweep: clean and old is removed; dirty is kept */
	{
		g_autoptr(JsonNode) made = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Tidy", "agent", "watcher",
			                      "repo", repo, NULL));
		g_autofree gchar *dirty_file = NULL;
		const gchar *kept;

		later = g_strdup(json_object_get_string_member(
			json_object_get_object_member(clawt_ipc_frame_get_payload(made),
			                              "task"), "id"));
		task = clawt_task_manager_get(fixture.daemon->tasks, later);
		kept = clawt_task_get_checkout_path(task);
		clawt_task_manager_complete(fixture.daemon->tasks, later, "tidied");

		/* not old enough */
		clawt_daemon_tasks_sweep_checkouts(fixture.daemon,
		                                   g_get_real_time() / G_USEC_PER_SEC);
		g_assert_true(g_file_test(kept, G_FILE_TEST_IS_DIR));

		/* old, but dirty */
		dirty_file = g_build_filename(kept, "SCRATCH", NULL);
		g_assert_true(g_file_set_contents(dirty_file, "x\n", -1, NULL));
		clawt_daemon_tasks_sweep_checkouts(fixture.daemon,
			g_get_real_time() / G_USEC_PER_SEC + 30 * 24 * 60 * 60);
		g_assert_true(g_file_test(kept, G_FILE_TEST_IS_DIR));

		/* old and clean */
		g_assert_cmpint(g_remove(dirty_file), ==, 0);
		clawt_daemon_tasks_sweep_checkouts(fixture.daemon,
			g_get_real_time() / G_USEC_PER_SEC + 30 * 24 * 60 * 60);
		g_assert_false(g_file_test(kept, G_FILE_TEST_EXISTS));
	}

	daemon_teardown(&fixture);
}

/* The agent's door: `repo` on clawtilla_delegate. */
static void
test_delegated_checkout(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *git = g_find_program_in_path("git");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *reply = NULL;
	gboolean is_error = TRUE;
	GPtrArray *all;
	ClawtTask *task;

	if (git == NULL) {
		g_test_skip("git is not installed");
		return;
	}

	fixture.dir = g_dir_make_tmp("clawt-checkout-XXXXXX", NULL);
	fixture.config = g_build_filename(fixture.dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: chief\n    chief_of_staff: true\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n"
		"  - id: boxed\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n      confine: bwrap\n",
		fixture.dir, fixture.dir, fixture.dir, fixture.dir);
	g_assert_true(g_file_set_contents(fixture.config, yaml, -1, &error));
	daemon_start(&fixture);
	repo = checkout_repository(fixture.dir);

	/* not a repository: nothing is delegated */
	reply = call_as(&fixture, "chief", "clawtilla_delegate",
		"{\"agent_id\":\"boxed\",\"task\":\"fix\","
		"\"repo\":\"/nowhere/at/all\"}", &is_error);
	g_assert_true(is_error);
	g_assert_nonnull(strstr(reply, "Not delegated"));
	g_clear_pointer(&reply, g_free);

	/* a confined assignee gets a clone, which its sandbox can follow */
	args = g_strdup_printf("{\"agent_id\":\"boxed\",\"task\":\"fix it\","
	                       "\"repo\":\"%s\",\"base\":\"main\"}", repo);
	reply = call_as(&fixture, "chief", "clawtilla_delegate", args, &is_error);
	g_assert_false(is_error);

	all = clawt_task_manager_list(fixture.daemon->tasks, "boxed", TRUE);
	task = NULL;
	{
		guint i;

		for (i = 0; i < all->len; i++) {
			ClawtTask *candidate = g_ptr_array_index(all, i);

			if (clawt_task_get_checkout_mode(candidate) != CLAWT_CHECKOUT_NONE)
				task = candidate;
		}
	}
	g_ptr_array_unref(all);
	g_assert_nonnull(task);
	g_assert_cmpint(clawt_task_get_checkout_mode(task), ==,
	                CLAWT_CHECKOUT_CLONE);
	g_assert_true(g_file_test(clawt_task_get_checkout_path(task),
	                          G_FILE_TEST_IS_DIR));
	{
		g_autofree gchar *dotgit = g_build_filename(
			clawt_task_get_checkout_path(task), ".git", NULL);

		/* a directory of its own, not a worktree's pointer file */
		g_assert_true(g_file_test(dotgit, G_FILE_TEST_IS_DIR));
	}

	daemon_teardown(&fixture);
}

/* ── Projects (C2) ───────────────────────────────────────────────── */

/* A store written before `project` existed gains the column on open. */
static void
test_store_project_migration(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-store-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "tasks.db", NULL);
	g_autoptr(ClawtTaskStore) store = NULL;
	g_autoptr(ClawtTask) task = clawt_task_new("user", "a", "work");
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	sqlite3 *db = NULL;

	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db,
		"CREATE TABLE tasks (id TEXT PRIMARY KEY, state INTEGER NOT NULL, "
		"assignee TEXT, origin TEXT, parent_id TEXT, title TEXT, "
		"created_at INTEGER NOT NULL, finished_at INTEGER NOT NULL "
		"DEFAULT 0, data TEXT NOT NULL);", NULL, NULL, NULL), ==, SQLITE_OK);
	sqlite3_close(db);

	store = clawt_task_store_new(path, &error);
	g_assert_no_error(error);

	clawt_task_set_project(task, "site", "brief");
	g_assert_true(clawt_task_store_save(store, task, NULL, &error));
	g_assert_no_error(error);

	found = clawt_task_store_project_tasks(store, "site", 0, &error);
	g_assert_cmpuint(found->len, ==, 1);
	g_clear_pointer(&found, g_ptr_array_unref);

	/* only since, and only its own */
	found = clawt_task_store_project_tasks(store, "site",
		clawt_task_get_created_at(task) + 1, &error);
	g_assert_cmpuint(found->len, ==, 0);
	g_clear_pointer(&found, g_ptr_array_unref);
	found = clawt_task_store_project_tasks(store, "other", 0, &error);
	g_assert_cmpuint(found->len, ==, 0);

	clawt_test_remove_tree(dir);
}

/*
 * Operator tasks in projects: named, told the goal, on the wire; a
 * project out of budget with a hard stop refuses new work and a held
 * release; and the project.* frames, which rewrite PROJECTS.org.
 */
static void
test_operator_projects(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *id = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *held = NULL;
	g_autofree gchar *blocker = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	daemon_setup(&fixture);

	/* made over IPC, typed and saved */
	{
		g_autoptr(JsonNode) made = ask(&fixture, "project.create",
			clawt_ipc_payload_new("project", "site", "goal",
			                      "Ship the new site", "monthly_budget_usd",
			                      "10", "budget_hard_stop", "true", NULL));
		g_autoptr(JsonNode) bad = ask(&fixture, "project.create",
			clawt_ipc_payload_new("project", "bad",
			                      "monthly_budget_usd", "lots", NULL));
		g_autoptr(JsonNode) listed = ask(&fixture, "project.list", NULL);
		JsonArray *projects;

		g_assert_false(clawt_ipc_frame_is_error(made));
		g_assert_true(clawt_ipc_frame_is_error(bad));
		projects = json_object_get_array_member(
			clawt_ipc_frame_get_payload(listed), "projects");
		g_assert_cmpuint(json_array_get_length(projects), ==, 1);
		g_assert_false(json_object_get_boolean_member(
			json_array_get_object_element(projects, 0), "stopped"));
	}

	/* PROJECTS.org now lists it */
	{
		g_autofree gchar *file = g_build_filename(fixture.dir, "agents",
			"watcher", "PROJECTS.org", NULL);
		g_autofree gchar *text = NULL;

		g_assert_true(g_file_get_contents(file, &text, NULL, NULL));
		g_assert_nonnull(strstr(text, "# BEGIN clawtilla projects"));
		g_assert_nonnull(strstr(text, "Ship the new site"));
	}

	/* a task named into it: on the wire, and the assignee is told */
	{
		g_autoptr(JsonNode) made = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Fix the header", "agent",
			                      "watcher", "project", "site", NULL));
		g_autoptr(JsonNode) unknown = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "x", "agent", "watcher",
			                      "project", "ghost", NULL));
		JsonObject *object;

		g_assert_false(clawt_ipc_frame_is_error(made));
		g_assert_true(clawt_ipc_frame_is_error(unknown));
		object = json_object_get_object_member(
			clawt_ipc_frame_get_payload(made), "task");
		id = g_strdup(json_object_get_string_member(object, "id"));
		g_assert_cmpstr(json_object_get_string_member(object, "project"), ==,
		                "site");
	}

	body = queued_body(&fixture, "watcher", id);
	g_assert_nonnull(strstr(body, "Its goal: Ship the new site"));
	{
		g_autofree gchar *contract = clawt_task_assignment_guidance(id);

		g_assert_true(g_str_has_suffix(body, contract));
	}

	/* a held task in the project, waiting on another */
	{
		g_autoptr(JsonNode) first = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Build", "agent", "watcher",
			                      NULL));
		g_autoptr(JsonNode) second = NULL;

		blocker = g_strdup(json_object_get_string_member(
			json_object_get_object_member(clawt_ipc_frame_get_payload(first),
			                              "task"), "id"));
		second = ask(&fixture, "task.create", clawt_ipc_payload_new(
			"prompt", "Deploy", "agent", "watcher", "project", "site",
			"after", blocker, NULL));
		g_assert_false(clawt_ipc_frame_is_error(second));
		held = g_strdup(json_object_get_string_member(
			json_object_get_object_member(clawt_ipc_frame_get_payload(second),
			                              "task"), "id"));
	}

	/* the budget is spent: new work refused, and the release fails */
	clawt_daemon_project_remember_spend(fixture.daemon, "site", 11000000, now);
	{
		g_autoptr(JsonNode) refused = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "More", "agent", "watcher",
			                      "project", "site", NULL));
		g_autoptr(JsonNode) listed = ask(&fixture, "project.list", NULL);

		g_assert_true(clawt_ipc_frame_is_error(refused));
		g_assert_nonnull(strstr(json_object_get_string_member(
			json_node_get_object(refused), "error"), "budget_hard_stop"));
		g_assert_true(json_object_get_boolean_member(
			json_array_get_object_element(json_object_get_array_member(
				clawt_ipc_frame_get_payload(listed), "projects"), 0),
			"stopped"));
	}

	clawt_task_manager_complete(fixture.daemon->tasks, blocker, "built");
	{
		ClawtTask *task = clawt_task_manager_get(fixture.daemon->tasks, held);

		g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_FAILED);
		g_assert_cmpuint(queued_for(&fixture, "watcher", held), ==, 0);
	}

	/* work outside the project is not held to its budget */
	{
		g_autoptr(JsonNode) free_work = ask(&fixture, "task.create",
			clawt_ipc_payload_new("prompt", "Elsewhere", "agent", "watcher",
			                      NULL));

		g_assert_false(clawt_ipc_frame_is_error(free_work));
	}

	/* set and remove; PROJECTS.org loses the region */
	{
		g_autoptr(JsonNode) set = ask(&fixture, "project.set",
			clawt_ipc_payload_new("project", "site", "key", "budget_hard_stop",
			                      "value", "false", NULL));
		g_autoptr(JsonNode) removed = ask(&fixture, "project.remove",
			clawt_ipc_payload_new("project", "site", NULL));
		g_autofree gchar *file = g_build_filename(fixture.dir, "agents",
			"watcher", "PROJECTS.org", NULL);
		g_autofree gchar *text = NULL;

		g_assert_false(clawt_ipc_frame_is_error(set));
		g_assert_false(clawt_ipc_frame_is_error(removed));
		g_assert_true(g_file_get_contents(file, &text, NULL, NULL));
		g_assert_null(strstr(text, "# BEGIN clawtilla projects"));
	}

	daemon_teardown(&fixture);
}

/* ── Version history and skill tests (D) ─────────────────────────── */

/* The newest revision id of @kind / @subject, over IPC. */
static gint64
newest_revision(
	DaemonFixture *fixture,
	const gchar   *kind,
	const gchar   *subject
){
	g_autoptr(JsonNode) listed = ask(fixture, "revision.list",
		clawt_ipc_payload_new("kind", kind, "subject", subject, NULL));
	JsonArray *rows = json_object_get_array_member(
		clawt_ipc_frame_get_payload(listed), "revisions");

	if (json_array_get_length(rows) == 0)
		return 0;

	return json_object_get_int_member(json_array_get_object_element(rows, 0),
	                                  "id");
}

static guint
revision_count(
	DaemonFixture *fixture,
	const gchar   *kind,
	const gchar   *subject
){
	g_autoptr(JsonNode) listed = ask(fixture, "revision.list",
		clawt_ipc_payload_new("kind", kind, "subject", subject, NULL));

	return json_array_get_length(json_object_get_array_member(
		clawt_ipc_frame_get_payload(listed), "revisions"));
}

/*
 * What is kept and put back: the config (a save, then a restore that
 * reloads), a persona file (written, compared with now, restored), and
 * a skill written through the library.
 */
static void
test_history(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *baseline = NULL;
	g_autofree gchar *soul = NULL;
	gint64 config_before;
	gint64 soul_before;

	daemon_setup(&fixture);

	/* baselines, on start */
	config_before = newest_revision(&fixture, "config", "config");
	g_assert_cmpint(config_before, >, 0);
	soul_before = newest_revision(&fixture, "persona", "watcher/SOUL.org");
	g_assert_cmpint(soul_before, >, 0);

	/* a save through any door is a version */
	{
		g_autoptr(JsonNode) made = ask(&fixture, "project.create",
			clawt_ipc_payload_new("project", "site", NULL));

		g_assert_false(clawt_ipc_frame_is_error(made));
		g_assert_cmpint(newest_revision(&fixture, "config", "config"), >,
		                config_before);
	}

	/* a persona file written by the operator; compared with now */
	{
		g_autoptr(JsonNode) written = ask(&fixture, "agent.file_write",
			clawt_ipc_payload_new("agent", "watcher", "name", "SOUL.org",
			                      "content", "* Who\nA careful watcher.\n",
			                      NULL));
		g_autofree gchar *from = g_strdup_printf("%" G_GINT64_FORMAT,
		                                         soul_before);
		g_autoptr(JsonNode) diff = ask(&fixture, "revision.diff",
			clawt_ipc_payload_new("from", from, NULL));
		g_autoptr(JsonNode) shown = ask(&fixture, "revision.show",
			clawt_ipc_payload_new("id", from, NULL));

		g_assert_false(clawt_ipc_frame_is_error(written));
		g_assert_cmpint(newest_revision(&fixture, "persona",
		                                "watcher/SOUL.org"), >, soul_before);
		g_assert_nonnull(strstr(json_object_get_string_member(
			clawt_ipc_frame_get_payload(diff), "diff"),
			"+A careful watcher."));
		baseline = g_strdup(json_object_get_string_member(
			clawt_ipc_frame_get_payload(shown), "text"));
	}

	/* restored: the file is back, and the restore is the newest version */
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT,
		                                       soul_before);
		g_autoptr(JsonNode) restored = ask(&fixture, "revision.restore",
			clawt_ipc_payload_new("id", id, NULL));
		g_autofree gchar *path = g_build_filename(fixture.dir, "agents",
		                                          "watcher", "SOUL.org", NULL);
		g_autoptr(JsonNode) listed = ask(&fixture, "revision.list",
			clawt_ipc_payload_new("kind", "persona", "subject",
			                      "watcher/SOUL.org", NULL));
		JsonObject *top;

		g_assert_false(clawt_ipc_frame_is_error(restored));
		g_assert_true(g_file_get_contents(path, &soul, NULL, NULL));
		g_assert_cmpstr(soul, ==, baseline);
		top = json_array_get_object_element(json_object_get_array_member(
			clawt_ipc_frame_get_payload(listed), "revisions"), 0);
		g_assert_nonnull(strstr(json_object_get_string_member(top, "note"),
		                        "restored from"));
	}

	/* the config put back as it was, and taken up: the project is gone */
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT,
		                                       config_before);
		g_autoptr(JsonNode) restored = ask(&fixture, "revision.restore",
			clawt_ipc_payload_new("id", id, NULL));
		g_autoptr(ClawtProjectSpec) gone = NULL;

		g_assert_false(clawt_ipc_frame_is_error(restored));
		gone = clawt_config_get_project(fixture.daemon->config, "site");
		g_assert_null(gone);
	}

	/* nothing to restore */
	{
		g_autoptr(JsonNode) missing = ask(&fixture, "revision.restore",
			clawt_ipc_payload_new("id", "99999", NULL));

		g_assert_true(clawt_ipc_frame_is_error(missing));
	}

	daemon_teardown(&fixture);
}

/*
 * A skill's history and its tests: written through the library (kept),
 * edited (kept again), tested on an agent that has it (a task in a room
 * of its own, checked when it ends) and refused on one that does not.
 */
static void
test_skill_history_and_tests(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *tests_dir = NULL;
	g_autofree gchar *pass_task = NULL;
	g_autofree gchar *fail_task = NULL;
	ClawtTask *task;

	fixture.dir = g_dir_make_tmp("clawt-skilltest-XXXXXX", NULL);
	fixture.config = g_build_filename(fixture.dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"skills:\n  enabled: true\n  dir: \"%s/skills\"\n"
		"agents:\n"
		"  - id: tester\n    skills: [release]\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n"
		"  - id: other\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n",
		fixture.dir, fixture.dir, fixture.dir, fixture.dir, fixture.dir);
	g_assert_true(g_file_set_contents(fixture.config, yaml, -1, &error));

	/* written by hand before the start, so the agent's skill exists */
	{
		g_autofree gchar *skill_dir = g_build_filename(fixture.dir, "skills",
		                                               "release", NULL);
		g_autofree gchar *skill_md = g_build_filename(skill_dir, "SKILL.md",
		                                              NULL);

		g_assert_cmpint(g_mkdir_with_parents(skill_dir, 0700), ==, 0);
		g_assert_true(g_file_set_contents(skill_md,
			"---\nname: release\n"
			"description: Use this when writing release notes\n---\n"
			"Write a ## Fixes section.\n", -1, NULL));
	}

	daemon_start(&fixture);

	/* its baseline, as it was found */
	g_assert_cmpuint(revision_count(&fixture, "skill", "release"), ==, 1);

	g_assert_nonnull(clawt_skill_library_replace(fixture.daemon->skills,
		"release", "Use this when writing release notes",
		"Write a ## Fixes section, then a ## Features one.\n", &error));
	g_assert_cmpuint(revision_count(&fixture, "skill", "release"), ==, 2);

	/* two saved tests */
	tests_dir = g_build_filename(fixture.dir, "skills", "release", "tests",
	                             NULL);
	g_assert_cmpint(g_mkdir_with_parents(tests_dir, 0700), ==, 0);
	{
		g_autofree gchar *a = g_build_filename(tests_dir, "a-basic.yaml",
		                                       NULL);
		g_autofree gchar *b = g_build_filename(tests_dir, "b-strict.yaml",
		                                       NULL);
		g_autofree gchar *bad = g_build_filename(tests_dir, "c-bad.yaml",
		                                         NULL);

		g_assert_true(g_file_set_contents(a,
			"input: \"Notes for 1.2\"\nexpect: [\"## Fixes\"]\n", -1, NULL));
		g_assert_true(g_file_set_contents(b,
			"input: \"Notes for 1.3\"\nexpect: \"## Features\"\n"
			"reject: [TODO]\n", -1, NULL));
		g_assert_true(g_file_set_contents(bad, "expect: [x]\n", -1, NULL));
	}

	/* an agent without the skill, or none named, is refused */
	{
		g_autoptr(JsonNode) wrong = ask(&fixture, "skill.test_run",
			clawt_ipc_payload_new("skill", "release", "agent", "other",
			                      NULL));
		g_autoptr(JsonNode) nobody = ask(&fixture, "skill.test_run",
			clawt_ipc_payload_new("skill", "release", NULL));

		g_assert_true(clawt_ipc_frame_is_error(wrong));
		g_assert_true(clawt_ipc_frame_is_error(nobody));
	}

	/* run both on the agent that has it */
	{
		g_autoptr(JsonNode) ran = ask(&fixture, "skill.test_run",
			clawt_ipc_payload_new("skill", "release", "agent", "tester",
			                      NULL));
		JsonArray *runs;

		g_assert_false(clawt_ipc_frame_is_error(ran));
		runs = json_object_get_array_member(clawt_ipc_frame_get_payload(ran),
		                                    "runs");
		g_assert_cmpuint(json_array_get_length(runs), ==, 2);
		pass_task = g_strdup(json_object_get_string_member(
			json_array_get_object_element(runs, 0), "task"));
		fail_task = g_strdup(json_object_get_string_member(
			json_array_get_object_element(runs, 1), "task"));
	}

	/* in a room of its own, not the operator's */
	task = clawt_task_manager_get(fixture.daemon->tasks, pass_task);
	g_assert_cmpuint(queued_for(&fixture, "tester", pass_task), ==, 1);
	{
		g_autofree gchar *body = queued_body(&fixture, "tester", pass_task);

		g_assert_nonnull(strstr(body, "test a-basic of your skill release"));
	}

	clawt_task_manager_complete(fixture.daemon->tasks, pass_task,
	                            "## Fixes\n- a bug");
	clawt_task_manager_complete(fixture.daemon->tasks, fail_task,
	                            "## Fixes only. TODO features");
	(void)task;

	{
		g_autoptr(JsonNode) listed = ask(&fixture, "skill.tests",
			clawt_ipc_payload_new("skill", "release", NULL));
		JsonObject *payload = clawt_ipc_frame_get_payload(listed);
		JsonArray *tests = json_object_get_array_member(payload, "tests");
		JsonObject *first = json_object_get_object_member(
			json_array_get_object_element(tests, 0), "last");
		JsonObject *second = json_object_get_object_member(
			json_array_get_object_element(tests, 1), "last");

		g_assert_cmpuint(json_array_get_length(tests), ==, 2);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			payload, "problems")), ==, 1);
		g_assert_cmpstr(json_object_get_string_member(first, "state"), ==,
		                "passed");
		g_assert_cmpstr(json_object_get_string_member(second, "state"), ==,
		                "failed");
		g_assert_cmpint(json_object_get_int_member(first, "revision"), ==,
		                newest_revision(&fixture, "skill", "release"));
	}

	daemon_teardown(&fixture);
}

/* ── Fleet export / import (E) ───────────────────────────────────── */

/* A fixture whose config is @agents_yaml under a stock daemon section. */
static void
fleet_start(
	DaemonFixture *fixture,
	const gchar   *body
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-fleet-XXXXXX", NULL);
	fixture->config = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, body);
	g_assert_true(g_file_set_contents(fixture->config, yaml, -1, &error));
	daemon_start(fixture);
}

/*
 * Out of one fleet and into another: what travels and what does not, the
 * preview, collisions skipped by default, a rename, the dangerous item
 * refused until allowed, agents arriving held, persona files in place.
 */
static void
test_fleet_pack_round_trip(void)
{
	DaemonFixture from = { 0 };
	DaemonFixture into = { 0 };
	g_autofree gchar *pack_root = NULL;
	g_autofree gchar *pack = NULL;
	g_autofree gchar *manifest = NULL;

	fleet_start(&from,
		"teams:\n  - id: crew\n    description: does the work\n"
		"projects:\n  - id: site\n    goal: ship it\n"
		"agents:\n"
		"  - id: scout\n    team: crew\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: true\n"
		"    integrations:\n      matrix:\n        enabled: false\n"
		"        access_token: {env: SCOUT_TOKEN}\n"
		"  - id: risky\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n"
		"    browser:\n      allow_scripts: true\n");

	/* a persona the agent wrote for itself */
	{
		g_autofree gchar *soul = g_build_filename(from.dir, "agents", "scout",
		                                          "SOUL.org", NULL);

		g_assert_true(g_file_set_contents(soul, "* Scout\nLooks first.\n",
		                                  -1, NULL));
	}

	pack_root = g_dir_make_tmp("clawt-pack-XXXXXX", NULL);
	pack = g_build_filename(pack_root, "pack", NULL);
	{
		g_autoptr(JsonNode) exported = ask(&from, "fleet.export",
			clawt_ipc_payload_new("dir", pack, NULL));
		g_autoptr(JsonNode) again = ask(&from, "fleet.export",
			clawt_ipc_payload_new("dir", pack, NULL));
		g_autoptr(JsonNode) relative = ask(&from, "fleet.export",
			clawt_ipc_payload_new("dir", "pack", NULL));
		JsonObject *report;
		JsonArray *to_fill;
		gboolean token_listed = FALSE;
		guint i;

		g_assert_false(clawt_ipc_frame_is_error(exported));
		g_assert_true(clawt_ipc_frame_is_error(again));
		g_assert_true(clawt_ipc_frame_is_error(relative));
		report = clawt_ipc_frame_get_payload(exported);
		g_assert_cmpint(json_object_get_int_member(report, "agents"), ==, 2);
		g_assert_cmpint(json_object_get_int_member(report, "teams"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(report, "projects"), ==, 1);

		to_fill = json_object_get_array_member(report, "to_fill");
		for (i = 0; i < json_array_get_length(to_fill); i++) {
			if (strstr(json_array_get_string_element(to_fill, i),
			           "agent scout: integrations.matrix.access_token"))
				token_listed = TRUE;
		}
		g_assert_true(token_listed);
	}

	/* the secret's reference never travels */
	{
		g_autofree gchar *path = g_build_filename(pack, "FLEET.yaml", NULL);
		g_autofree gchar *soul = g_build_filename(pack, "agents", "scout",
		                                          "SOUL.org", NULL);

		g_assert_true(g_file_get_contents(path, &manifest, NULL, NULL));
		g_assert_null(strstr(manifest, "SCOUT_TOKEN"));
		g_assert_null(strstr(manifest, "state_dir"));
		g_assert_nonnull(strstr(manifest, "clawtilla-fleet"));
		g_assert_true(g_file_test(soul, G_FILE_TEST_IS_REGULAR));
	}

	/* the same fleet, previewed: everything already exists */
	{
		g_autoptr(JsonNode) preview = ask(&from, "fleet.import_preview",
			clawt_ipc_payload_new("dir", pack, NULL));
		JsonArray *items = json_object_get_array_member(
			clawt_ipc_frame_get_payload(preview), "items");
		guint i;

		for (i = 0; i < json_array_get_length(items); i++)
			g_assert_true(json_object_get_boolean_member(
				json_array_get_object_element(items, i), "exists"));
	}

	daemon_teardown(&from);

	/* a second fleet that already has a scout, and nothing else */
	fleet_start(&into,
		"agents:\n"
		"  - id: scout\n"
		"    model:\n      provider: claude-code\n"
		"    runtime:\n      autostart: false\n");

	/* previewed: scout exists, risky is dangerous, the rest is new */
	{
		g_autoptr(JsonNode) preview = ask(&into, "fleet.import_preview",
			clawt_ipc_payload_new("dir", pack, NULL));
		JsonArray *items = json_object_get_array_member(
			clawt_ipc_frame_get_payload(preview), "items");
		guint i;

		for (i = 0; i < json_array_get_length(items); i++) {
			JsonObject *item = json_array_get_object_element(items, i);
			const gchar *id = json_object_get_string_member(item, "id");
			JsonArray *dangerous = json_object_get_array_member(item,
			                                                    "dangerous");

			g_assert_cmpint(json_object_get_boolean_member(item, "exists"),
			                ==, g_strcmp0(id, "scout") == 0);
			g_assert_cmpuint(json_array_get_length(dangerous), ==,
			                 g_strcmp0(id, "risky") == 0 ? 1 : 0);
		}
	}

	/* the dangerous one refuses the whole import, and nothing changes */
	{
		g_autoptr(JsonNode) refused = ask(&into, "fleet.import",
			clawt_ipc_payload_new("dir", pack, NULL));

		g_assert_true(clawt_ipc_frame_is_error(refused));
		g_assert_nonnull(strstr(json_object_get_string_member(
			json_node_get_object(refused), "error"),
			"browser.allow_scripts"));
		g_assert_null(clawt_config_get_agent(into.daemon->config, "risky"));
	}

	/* risky skipped, the pack's scout taken in as scout2 */
	{
		g_autoptr(JsonNode) taken = ask(&into, "fleet.import",
			clawt_ipc_payload_new("dir", pack, "resolutions",
			                      "agent:risky=skip,agent:scout=rename:scout2",
			                      NULL));
		ClawtAgentConfig *scout2;
		g_autoptr(ClawtTeamSpec) crew = NULL;
		g_autofree gchar *soul = NULL;
		g_autofree gchar *text = NULL;

		g_assert_false(clawt_ipc_frame_is_error(taken));
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			clawt_ipc_frame_get_payload(taken), "taken")), ==, 3);

		scout2 = clawt_config_get_agent(into.daemon->config, "scout2");
		g_assert_nonnull(scout2);
		g_assert_false(clawt_agent_config_get_boolean(scout2,
		                                              "runtime.autostart"));
		crew = clawt_config_get_team(into.daemon->config, "crew");
		g_assert_nonnull(crew);
		g_assert_null(clawt_config_get_agent(into.daemon->config, "risky"));

		/* its own persona, not the template */
		soul = g_build_filename(into.dir, "agents", "scout2", "SOUL.org",
		                        NULL);
		g_assert_true(g_file_get_contents(soul, &text, NULL, NULL));
		g_assert_cmpstr(text, ==, "* Scout\nLooks first.\n");

		/* and the import is a version in the history */
		{
			g_autoptr(JsonNode) history = ask(&into, "revision.list",
				clawt_ipc_payload_new("kind", "config", NULL));

			g_assert_cmpstr(json_object_get_string_member(
				json_array_get_object_element(json_object_get_array_member(
					clawt_ipc_frame_get_payload(history), "revisions"), 0),
				"author"), ==, "import");
		}
	}

	/* again, with nothing said: what exists is skipped, nothing is added */
	{
		g_autoptr(JsonNode) again = ask(&into, "fleet.import",
			clawt_ipc_payload_new("dir", pack, "resolutions",
			                      "agent:risky=skip", NULL));

		g_assert_false(clawt_ipc_frame_is_error(again));
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			clawt_ipc_frame_get_payload(again), "taken")), ==, 0);
	}

	/* allowed: risky comes in, held */
	{
		g_autoptr(JsonNode) allowed = ask(&into, "fleet.import",
			clawt_ipc_payload_new("dir", pack, "allow_dangerous", "true",
			                      NULL));

		g_assert_false(clawt_ipc_frame_is_error(allowed));
		g_assert_nonnull(clawt_config_get_agent(into.daemon->config,
		                                        "risky"));
	}

	daemon_teardown(&into);
	clawt_test_remove_tree(pack_root);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/task-store/json/round-trip",
	                test_json_round_trip_keeps_every_field);
	g_test_add_func("/task-store/json/unknown-state-is-failed",
	                test_json_unknown_state_reads_as_failed);
	g_test_add_func("/task-store/json/refuses-no-id",
	                test_json_refuses_a_task_with_no_id_or_prompt);
	g_test_add_func("/task-store/title", test_title_from_prompt);

	g_test_add("/task-store/store/reopen", StoreFixture, NULL, store_setup,
	           test_store_survives_a_reopen, store_teardown);
	g_test_add("/task-store/store/journal", StoreFixture, NULL, store_setup,
	           test_journal_records_what_happened, store_teardown);
	g_test_add("/task-store/store/live-and-prune", StoreFixture, NULL,
	           store_setup, test_live_set_and_pruning, store_teardown);
	g_test_add("/task-store/store/list", StoreFixture, NULL, store_setup,
	           test_list_pages_and_filters, store_teardown);
	g_test_add("/task-store/store/incidents", StoreFixture, NULL, store_setup,
	           test_incidents_round_trip, store_teardown);

	g_test_add("/task-store/manager/write-through", StoreFixture, NULL,
	           store_setup, test_manager_writes_through_and_restores_silently,
	           store_teardown);
	g_test_add("/task-store/manager/forget", StoreFixture, NULL, store_setup,
	           test_forgotten_tasks_stay_answerable, store_teardown);
	g_test_add_func("/task-store/manager/no-store",
	                test_without_a_store_nothing_is_forgotten);

	g_test_add_func("/task-store/spend", test_task_spend_is_read_back);

	g_test_add_func("/task-store/daemon/restart",
	                test_a_restart_keeps_carried_tasks_and_fails_the_rest);
	g_test_add_func("/task-store/daemon/ipc", test_the_ipc_surface);
	g_test_add_func("/task-store/daemon/incidents",
	                test_incidents_are_restored_without_a_second_report);
	g_test_add_func("/task-store/daemon/no-store-no-promise",
	                test_no_store_no_promise);
	g_test_add_func("/task-store/caption", test_the_list_caption);

	g_test_add("/task-store/notes/kept-and-pruned", StoreFixture, NULL,
	           store_setup, test_notes_are_kept_and_pruned, store_teardown);
	g_test_add("/task-store/notes/progress-and-results", StoreFixture, NULL,
	           store_setup, test_progress_and_results_are_noted,
	           store_teardown);
	g_test_add_func("/task-store/daemon/operator-tasks", test_operator_tasks);
	g_test_add("/task-store/blockers/links", StoreFixture, NULL, store_setup,
	           test_links_and_what_holds, store_teardown);
	g_test_add_func("/task-store/blockers/operator", test_operator_blockers);
	g_test_add_func("/task-store/blockers/delegation",
	                test_delegation_waits);
	g_test_add_func("/task-store/review/resolution",
	                test_reviewer_resolution);
	g_test_add_func("/task-store/review/rounds", test_review_rounds);
	g_test_add_func("/task-store/review/end-to-end",
	                test_review_end_to_end);
	g_test_add_func("/task-store/checkouts/operator",
	                test_operator_checkouts);
	g_test_add_func("/task-store/checkouts/delegated",
	                test_delegated_checkout);
	g_test_add_func("/task-store/projects/migration",
	                test_store_project_migration);
	g_test_add_func("/task-store/projects/operator", test_operator_projects);
	g_test_add_func("/task-store/history/config-and-persona", test_history);
	g_test_add_func("/task-store/history/skills-and-tests",
	                test_skill_history_and_tests);
	g_test_add_func("/task-store/fleet/round-trip",
	                test_fleet_pack_round_trip);

	return g_test_run();
}
