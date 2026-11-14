/*
 * test-memory-journal.c - Every change to a memory, and its undo
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The store half writes and undoes against a real sqlite file in a
 * temporary directory.  The daemon half goes through the IPC handler, as
 * a client would, so the scope rule -- a shared database's row belongs
 * to the agent that made it -- is tested where it is enforced and not
 * where it is drawn.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

typedef struct {
	gchar            *dir;
	ClawtMemoryStore *store;
} StoreFixture;

static void
store_setup(
	StoreFixture *fixture
){
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-journal-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "memory.db", NULL);
	fixture->store = clawt_memory_store_new(path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->store);
}

static void
store_teardown(
	StoreFixture *fixture
){
	g_clear_object(&fixture->store);
	clawt_test_remove_tree(fixture->dir);
	g_clear_pointer(&fixture->dir, g_free);
}

static gchar *
remember(
	ClawtMemoryStore *store,
	const gchar      *text,
	const gchar      *who
){
	g_autoptr(ClawtMemory) memory = clawt_memory_new(text);
	g_autoptr(GError) error = NULL;
	gchar *id;

	memory->source = g_strdup(who);
	id = clawt_memory_store_add(store, memory, &error);
	g_assert_no_error(error);

	return id;
}

/* Whether a memory turns up in an ordinary listing. */
static gboolean
listed(
	ClawtMemoryStore *store,
	const gchar      *id
){
	g_autoptr(GPtrArray) all = clawt_memory_store_list(store, NULL, FALSE,
		100, NULL);
	guint i;

	for (i = 0; all != NULL && i < all->len; i++)
		if (g_strcmp0(((ClawtMemory *)g_ptr_array_index(all, i))->id,
				id) == 0)
			return TRUE;

	return FALSE;
}

static ClawtMemoryJournalEntry *
row(
	GPtrArray *rows,
	guint      n
){
	g_assert_cmpuint(n, <, rows->len);

	return g_ptr_array_index(rows, n);
}

/*
 * Every change lands in the journal, with who made it, newest first --
 * through the plain functions too, because a caller that forgets to say
 * who it is must still leave a row.
 */
static void
test_every_change_is_journaled(
	void
){
	StoreFixture fixture = { 0 };
	g_autofree gchar *id = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;

	store_setup(&fixture);

	id = remember(fixture.store, "The deploy key lives in vault", "chief");
	g_assert_true(clawt_memory_store_pin_as(fixture.store, id, TRUE, "chief",
		&error));
	g_assert_true(clawt_memory_store_pin(fixture.store, id, FALSE, &error));
	g_assert_true(clawt_memory_store_forget_as(fixture.store, id, "chief",
		&error));
	g_assert_no_error(error);

	rows = clawt_memory_store_journal(fixture.store, NULL, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 4);

	g_assert_cmpstr(row(rows, 0)->action, ==, "forget");
	g_assert_cmpstr(row(rows, 1)->action, ==, "unpin");
	g_assert_null(row(rows, 1)->actor);
	g_assert_cmpstr(row(rows, 2)->action, ==, "pin");
	g_assert_cmpstr(row(rows, 3)->action, ==, "add");
	g_assert_cmpstr(row(rows, 3)->actor, ==, "chief");
	g_assert_cmpstr(row(rows, 3)->memory_id, ==, id);
	g_assert_cmpstr(row(rows, 3)->excerpt, ==,
		"The deploy key lives in vault");

	/* The actor filter. */
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = clawt_memory_store_journal(fixture.store, "chief", 0, NULL);
	g_assert_cmpuint(rows->len, ==, 3);

	store_teardown(&fixture);
}

/*
 * Each action's reversal, and an undo is a row of its own that cannot
 * itself be undone.
 */
static void
test_undo_reverses_each_action(
	void
){
	StoreFixture fixture = { 0 };
	g_autofree gchar *kept = NULL;
	g_autofree gchar *added = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMemory) memory = NULL;

	store_setup(&fixture);

	kept = remember(fixture.store, "Operator prefers YAML", "chief");
	added = remember(fixture.store, "Something false", "chief");

	/* Undo of an add archives: out of the listing, still on disk. */
	rows = clawt_memory_store_journal(fixture.store, NULL, 0, NULL);
	g_assert_cmpstr(row(rows, 0)->memory_id, ==, added);
	g_assert_true(row(rows, 0)->undoable);
	g_assert_true(clawt_memory_store_undo(fixture.store, row(rows, 0)->id,
		"operator", &error));
	g_assert_no_error(error);
	g_assert_false(listed(fixture.store, added));
	memory = clawt_memory_store_get(fixture.store, added, NULL);
	g_assert_nonnull(memory);
	g_assert_true(memory->archived);

	/* Forget, then undo the forget: back in the listing. */
	g_assert_true(clawt_memory_store_forget_as(fixture.store, kept, "chief",
		NULL));
	g_assert_false(listed(fixture.store, kept));
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = clawt_memory_store_journal(fixture.store, NULL, 0, NULL);
	g_assert_cmpstr(row(rows, 0)->action, ==, "forget");
	g_assert_true(clawt_memory_store_undo(fixture.store, row(rows, 0)->id,
		"operator", &error));
	g_assert_true(listed(fixture.store, kept));

	/* Pin, then undo: unpinned again. */
	g_assert_true(clawt_memory_store_pin_as(fixture.store, kept, TRUE,
		"chief", NULL));
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = clawt_memory_store_journal(fixture.store, NULL, 0, NULL);
	g_assert_true(clawt_memory_store_undo(fixture.store, row(rows, 0)->id,
		"operator", &error));
	g_clear_pointer(&memory, clawt_memory_free);
	memory = clawt_memory_store_get(fixture.store, kept, NULL);
	g_assert_false(memory->pinned);

	/* The undo rows say who, and are not themselves undoable. */
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = clawt_memory_store_journal(fixture.store, NULL, 0, NULL);
	g_assert_cmpstr(row(rows, 0)->action, ==, "undo");
	g_assert_cmpstr(row(rows, 0)->actor, ==, "operator");
	g_assert_false(row(rows, 0)->undoable);
	g_assert_cmpstr(row(rows, 1)->action, ==, "pin");
	g_assert_cmpint(row(rows, 1)->undone_at, >, 0);
	g_assert_cmpstr(row(rows, 1)->undone_by, ==, "operator");
	g_assert_false(row(rows, 1)->undoable);

	/* Twice is refused, with the reason. */
	g_assert_false(clawt_memory_store_undo(fixture.store, row(rows, 1)->id,
		"operator", &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "already undone"));

	store_teardown(&fixture);
}

/*
 * A later change still standing blocks the older one -- undoing a forget
 * the agent had since followed with a pin would undo the wrong thing --
 * until the later one is undone first.
 */
static void
test_a_later_change_blocks_an_older_undo(
	void
){
	StoreFixture fixture = { 0 };
	g_autofree gchar *id = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint64 forget_row;
	gint64 pin_row;

	store_setup(&fixture);

	id = remember(fixture.store, "A fact", "chief");
	g_assert_true(clawt_memory_store_forget_as(fixture.store, id, "chief",
		NULL));
	g_assert_true(clawt_memory_store_pin_as(fixture.store, id, TRUE, "chief",
		NULL));

	rows = clawt_memory_store_journal(fixture.store, NULL, 0, NULL);
	pin_row = row(rows, 0)->id;
	forget_row = row(rows, 1)->id;
	g_assert_true(row(rows, 0)->undoable);
	g_assert_false(row(rows, 1)->undoable);
	g_assert_nonnull(strstr(row(rows, 1)->why_not, "later change"));
	g_assert_false(row(rows, 2)->undoable);

	g_assert_false(clawt_memory_store_undo(fixture.store, forget_row,
		"operator", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_assert_true(clawt_memory_store_undo(fixture.store, pin_row, "operator",
		&error));
	g_assert_true(clawt_memory_store_undo(fixture.store, forget_row,
		"operator", &error));
	g_assert_no_error(error);
	g_assert_true(listed(fixture.store, id));

	store_teardown(&fixture);
}

/*
 * An add that replaced an earlier version of the same id is refused:
 * the earlier text is not kept, so an undo would only hide the new one.
 */
static void
test_a_replacing_add_is_not_undoable(
	void
){
	StoreFixture fixture = { 0 };
	g_autoptr(ClawtMemory) first = clawt_memory_new("Version one");
	g_autoptr(ClawtMemory) second = clawt_memory_new("Version two");
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	store_setup(&fixture);

	first->id = g_strdup("mem-fixed");
	second->id = g_strdup("mem-fixed");
	a = clawt_memory_store_add(fixture.store, first, NULL);
	b = clawt_memory_store_add(fixture.store, second, NULL);

	rows = clawt_memory_store_journal(fixture.store, NULL, 0, NULL);
	g_assert_cmpuint(rows->len, ==, 2);
	g_assert_false(row(rows, 0)->undoable);
	g_assert_nonnull(strstr(row(rows, 0)->why_not, "earlier version"));

	store_teardown(&fixture);
}

/* memories.journal_keep is reached: the oldest rows go, the newest stay. */
static void
test_the_journal_is_bounded(
	void
){
	StoreFixture fixture = { 0 };
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	store_setup(&fixture);
	clawt_memory_store_set_journal_keep(fixture.store, 5);

	for (i = 0; i < 9; i++) {
		g_autofree gchar *text = g_strdup_printf("fact %u", i);
		g_autofree gchar *id = remember(fixture.store, text, "chief");
	}

	rows = clawt_memory_store_journal(fixture.store, NULL, 100, NULL);
	g_assert_cmpuint(rows->len, ==, 5);
	g_assert_cmpstr(row(rows, 0)->excerpt, ==, "fact 8");
	g_assert_cmpstr(row(rows, 4)->excerpt, ==, "fact 4");

	/* A row that has gone says so when asked for. */
	g_assert_false(clawt_memory_store_undo(fixture.store, 1, "operator",
		&error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);

	store_teardown(&fixture);
}

/* The words: one place, so the three clients say the same thing. */
static void
test_the_labels(
	void
){
	ClawtMemoryJournalEntry entry = { 0 };
	g_autofree gchar *line = NULL;
	g_autofree gchar *undone = NULL;

	g_assert_cmpstr(clawt_memory_journal_action_label("forget"), ==,
		"Forgot");
	g_assert_cmpstr(clawt_memory_journal_undo_label("forget"), ==,
		"Bring it back");
	g_assert_cmpstr(clawt_memory_journal_undo_label("add"), ==,
		"Archive it");
	g_assert_cmpstr(clawt_memory_journal_action_label("future"), ==,
		"future");

	entry.action = (gchar *)"add";
	entry.actor = (gchar *)"chief";
	entry.memory_id = (gchar *)"mem-1";
	entry.excerpt = (gchar *)"The key";
	line = clawt_memory_journal_describe(&entry);
	g_assert_nonnull(strstr(line, "chief remembered"));
	g_assert_nonnull(strstr(line, "The key"));
	g_assert_null(strstr(line, "undone"));

	entry.undone_at = 1;
	entry.undone_by = (gchar *)"operator";
	undone = clawt_memory_journal_describe(&entry);
	g_assert_nonnull(strstr(undone, "undone by operator"));
}

/* ── Through the daemon ──────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	gchar        *config_path;
	ClawtDaemon  *daemon;
	GMainContext *context;
} Fixture;

static void
fixture_setup(
	Fixture *fixture
){
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-journal-d-XXXXXX", NULL);
	fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
		NULL);

	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"teams:\n  - id: ops\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: alpha\n    team: ops\n"
		"  - id: beta\n    team: ops\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir);

	g_file_set_contents(fixture->config_path, yaml, -1, &error);
	g_assert_no_error(error);

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(fixture->config_path,
		fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
fixture_teardown(
	Fixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;

	g_clear_pointer(&fixture->context, g_main_context_unref);
	clawt_test_remove_tree(fixture->dir);
	g_clear_pointer(&fixture->dir, g_free);
	g_clear_pointer(&fixture->config_path, g_free);
}

static JsonNode *
request(
	Fixture     *fixture,
	const gchar *kind,
	const gchar *payload_json
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
	clawt_ipc_frame_set_payload(frame,
		json_node_copy(json_parser_get_root(parser)));

	return clawt_daemon_handle_request(fixture->daemon, frame);
}

static JsonObject *
payload_of(
	JsonNode *reply
){
	JsonObject *frame = json_node_get_object(reply);

	g_assert_true(json_object_has_member(frame, "payload"));

	return json_object_get_object_member(frame, "payload");
}

static gboolean
is_error(
	JsonNode *reply
){
	return json_object_has_member(json_node_get_object(reply), "error");
}

/*
 * The review page's data, end to end: an agent's own database in full,
 * and a shared one only for the changes that agent made.  Two agents on
 * one team, because one key cannot show which rows belong to whom.
 */
static void
test_the_review_frames(
	void
){
	Fixture fixture = { 0 };
	ClawtAgentManager *agents;
	ClawtMemoryStore *own;
	ClawtMemoryStore *team;
	g_autofree gchar *mine = NULL;
	g_autofree gchar *shared_alpha = NULL;
	g_autofree gchar *shared_beta = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(JsonNode) undo = NULL;
	g_autoptr(JsonNode) refused = NULL;
	JsonArray *entries;
	gint64 beta_row = 0;
	guint i;

	fixture_setup(&fixture);
	agents = clawt_daemon_get_agents(fixture.daemon);

	own = clawt_agent_manager_memory_store_for_scope(agents, "alpha",
		CLAWT_MEMORY_SCOPE_AGENT);
	g_assert_nonnull(own);
	mine = remember(own, "alpha's own fact", "alpha");

	/* The team's database exists once somebody writes to it. */
	team = clawt_memory_scopes_open_for_write(
		clawt_agent_manager_get_memory_scopes(agents),
		CLAWT_MEMORY_SCOPE_TEAM, "ops", NULL);
	g_assert_nonnull(team);
	shared_alpha = remember(team, "alpha told the team", "alpha");
	shared_beta = remember(team, "beta told the team", "beta");

	reply = request(&fixture, "memory.journal", "{\"agent\": \"alpha\"}");
	g_assert_false(is_error(reply));
	entries = json_object_get_array_member(payload_of(reply), "entries");
	g_assert_cmpuint(json_array_get_length(entries), ==, 2);

	for (i = 0; i < json_array_get_length(entries); i++) {
		JsonObject *entry = json_array_get_object_element(entries, i);

		g_assert_cmpstr(json_object_get_string_member(entry, "actor"), ==,
			"alpha");
		g_assert_true(json_object_get_boolean_member(entry, "undoable"));
	}

	/* beta's row in the team database: beta's to undo, not alpha's. */
	{
		g_autoptr(GPtrArray) rows = clawt_memory_store_journal(team, "beta",
			0, NULL);

		beta_row = row(rows, 0)->id;
	}

	body = g_strdup_printf("{\"agent\": \"alpha\", \"scope\": \"team\", "
		"\"id\": %" G_GINT64_FORMAT "}", beta_row);
	refused = request(&fixture, "memory.undo", body);
	g_assert_true(is_error(refused));

	g_free(body);
	body = g_strdup_printf("{\"agent\": \"beta\", \"scope\": \"team\", "
		"\"id\": %" G_GINT64_FORMAT "}", beta_row);
	undo = request(&fixture, "memory.undo", body);
	g_assert_false(is_error(undo));
	g_assert_true(json_object_get_boolean_member(payload_of(undo),
		"undone"));
	g_assert_false(listed(team, shared_beta));
	g_assert_true(listed(team, shared_alpha));

	fixture_teardown(&fixture);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/memory-journal/every-change",
		test_every_change_is_journaled);
	g_test_add_func("/memory-journal/undo-each-action",
		test_undo_reverses_each_action);
	g_test_add_func("/memory-journal/later-change-blocks",
		test_a_later_change_blocks_an_older_undo);
	g_test_add_func("/memory-journal/replacing-add",
		test_a_replacing_add_is_not_undoable);
	g_test_add_func("/memory-journal/bounded",
		test_the_journal_is_bounded);
	g_test_add_func("/memory-journal/labels", test_the_labels);
	g_test_add_func("/memory-journal/daemon/review-frames",
		test_the_review_frames);

	return g_test_run();
}
