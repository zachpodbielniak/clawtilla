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

	return g_test_run();
}
