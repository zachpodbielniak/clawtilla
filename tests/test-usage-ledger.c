/*
 * test-usage-ledger.c - What the fleet spent, by model, by origin, by day
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The costs are libreclaw's rows, written here through libreclaw's own
 * API exactly as test-usage.c does, so the reader is tested against the
 * schema it actually reads.  The origins are clawtilla's: classified by
 * one rule, written beside the rows, and joined back on the row id.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#include "clawt-test-util.h"

/* For the daemon's own origin lookup and its task-source table. */
#include "core/clawt-daemon-private.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

typedef struct {
	gchar *dir;
} Scratch;

static void
scratch_setup(
	Scratch *scratch
){
	scratch->dir = g_dir_make_tmp("clawt-ledger-XXXXXX", NULL);
	g_assert_nonnull(scratch->dir);
}

static void
scratch_teardown(
	Scratch *scratch
){
	if (scratch->dir != NULL)
		clawt_test_remove_tree(scratch->dir);
	g_clear_pointer(&scratch->dir, g_free);
}

/* An agent's state directory with its sessions/ directory made. */
static gchar *
make_state_dir(
	const gchar *root,
	const gchar *agent
){
	gchar *state_dir = g_build_filename(root, agent, NULL);
	g_autofree gchar *sessions = g_build_filename(state_dir, "sessions", NULL);

	g_assert_cmpint(g_mkdir_with_parents(sessions, 0700), ==, 0);
	return state_dir;
}

/* Appends one libreclaw row and returns its id. */
static gint64
add_row(
	const gchar *state_dir,
	const gchar *room,
	const gchar *channel,
	const gchar *model,
	gint64       input_tokens,
	gint64       output_tokens,
	gint64       cost_micros,
	gint64       recorded_at
){
	g_autofree gchar *db_path = clawt_usage_database_path(state_dir);
	g_autoptr(LcDatabase) db = LC_DATABASE(lc_sqlite_database_new());
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	gint64 id = 0;
	guint i;

	g_assert_true(lc_database_open(db, db_path, &error));
	g_assert_no_error(error);
	g_assert_true(lc_database_add_token_usage(db, "key", channel, room, model,
						  model, input_tokens, output_tokens,
						  cost_micros, recorded_at, &error));
	g_assert_no_error(error);

	rows = lc_database_query_token_usage(db, NULL, NULL, 0, 0, &error);
	g_assert_no_error(error);
	for (i = 0; i < rows->len; i++) {
		LcDbTokenUsage *row = g_ptr_array_index(rows, i);

		id = MAX(id, row->id);
	}
	g_ptr_array_unref(rows);
	lc_database_close(db);

	return id;
}

static void
tag_row(
	const gchar         *state_dir,
	gint64               usage_id,
	gint64               recorded_at,
	ClawtUsageOriginKind kind,
	const gchar         *id
){
	g_autofree gchar *path = clawt_usage_ledger_path(state_dir);
	g_autoptr(ClawtUsageOrigin) origin = clawt_usage_origin_new(kind, id);
	g_autoptr(GError) error = NULL;

	g_assert_true(clawt_usage_ledger_record(path, usage_id, recorded_at, NULL,
						origin, &error));
	g_assert_no_error(error);
}

/* The group in a reply whose key is @key, or NULL. */
static JsonObject *
group_named(
	JsonObject  *reply,
	const gchar *key
){
	JsonArray *groups = json_object_get_array_member(reply, "groups");
	guint i;

	for (i = 0; i < json_array_get_length(groups); i++) {
		JsonObject *group = json_array_get_object_element(groups, i);

		if (g_strcmp0(json_object_get_string_member(group, "key"), key) == 0)
			return group;
	}

	return NULL;
}

static gint64
int_of(
	JsonObject  *object,
	const gchar *name
){
	g_assert_nonnull(object);
	return json_object_get_int_member_with_default(object, name, -999);
}

/* ── The origin rule ─────────────────────────────────────────────── */

/*
 * Every kind, through the one function the daemon calls -- and the
 * label each one is grouped under, because a label is what a person
 * reads and what the CSV carries.
 */
static void
test_each_origin_kind_is_classified(void)
{
	const gchar *id = NULL;
	g_autofree gchar *label = NULL;

	/* the operator */
	g_assert_cmpint(clawt_usage_origin_classify("user", FALSE, NULL, NULL, NULL,
						    "clawtilla", &id),
			==, CLAWT_USAGE_ORIGIN_OPERATOR);
	g_assert_null(id);
	label = clawt_usage_origin_label(CLAWT_USAGE_ORIGIN_OPERATOR, id);
	g_assert_cmpstr(label, ==, "operator");
	g_clear_pointer(&label, g_free);

	/* a peer agent */
	g_assert_cmpint(clawt_usage_origin_classify("chief", TRUE, NULL, NULL, NULL,
						    "clawtilla", &id),
			==, CLAWT_USAGE_ORIGIN_AGENT);
	g_assert_cmpstr(id, ==, "chief");
	label = clawt_usage_origin_label(CLAWT_USAGE_ORIGIN_AGENT, id);
	g_assert_cmpstr(label, ==, "agent:chief");
	g_clear_pointer(&label, g_free);

	/*
	 * A routine wins over the agent that delivered its delegated work:
	 * the question is which routine costs most, and a routine that
	 * hands its job to a peer would otherwise vanish from the answer.
	 */
	g_assert_cmpint(clawt_usage_origin_classify("chief", TRUE, "task-9",
						    "morning", NULL, "clawtilla",
						    &id),
			==, CLAWT_USAGE_ORIGIN_ROUTINE);
	g_assert_cmpstr(id, ==, "morning");
	label = clawt_usage_origin_label(CLAWT_USAGE_ORIGIN_ROUTINE, id);
	g_assert_cmpstr(label, ==, "routine:morning");
	g_clear_pointer(&label, g_free);

	/* a trigger, the same way */
	g_assert_cmpint(clawt_usage_origin_classify("user", FALSE, "task-3", NULL,
						    "github-push", NULL, &id),
			==, CLAWT_USAGE_ORIGIN_TRIGGER);
	g_assert_cmpstr(id, ==, "github-push");

	/* a routine's task whose run the daemon no longer remembers */
	g_assert_cmpint(clawt_usage_origin_classify("routine", FALSE, "task-7",
						    NULL, NULL, "clawtilla", &id),
			==, CLAWT_USAGE_ORIGIN_TASK);
	g_assert_cmpstr(id, ==, "task-7");
	label = clawt_usage_origin_label(CLAWT_USAGE_ORIGIN_TASK, id);
	g_assert_cmpstr(label, ==, "task:task-7");
	g_clear_pointer(&label, g_free);

	/* the agent's own Matrix room, which clawtilla never routed */
	g_assert_cmpint(clawt_usage_origin_classify(NULL, FALSE, NULL, NULL, NULL,
						    "matrix", &id),
			==, CLAWT_USAGE_ORIGIN_CHANNEL);
	g_assert_cmpstr(id, ==, "matrix");

	/* a sender known by name only */
	g_assert_cmpint(clawt_usage_origin_classify("grok:helper", FALSE, NULL,
						    NULL, NULL, "clawtilla", &id),
			==, CLAWT_USAGE_ORIGIN_OTHER);
	g_assert_cmpstr(id, ==, "grok:helper");

	/* and nothing on record on clawtilla's own channel is not a guess */
	g_assert_cmpint(clawt_usage_origin_classify(NULL, FALSE, NULL, NULL, NULL,
						    "clawtilla", &id),
			==, CLAWT_USAGE_ORIGIN_UNKNOWN);
	g_assert_null(id);
	label = clawt_usage_origin_label(CLAWT_USAGE_ORIGIN_UNKNOWN, id);
	g_assert_cmpstr(label, ==, "untagged");
}

/* The nick is what the ledger file stores; it has to come back. */
static void
test_origin_kind_nicks_round_trip(void)
{
	gint kind;

	for (kind = (gint)CLAWT_USAGE_ORIGIN_UNKNOWN;
	     kind <= (gint)CLAWT_USAGE_ORIGIN_OTHER; kind++)
		g_assert_cmpint(clawt_usage_origin_kind_from_nick(
					clawt_usage_origin_kind_nick(
						(ClawtUsageOriginKind)kind)),
				==, kind);

	g_assert_cmpint(clawt_usage_origin_kind_from_nick("nonsense"), ==,
			CLAWT_USAGE_ORIGIN_UNKNOWN);
	g_assert_cmpint(clawt_usage_origin_kind_from_nick(NULL), ==,
			CLAWT_USAGE_ORIGIN_UNKNOWN);
}

/* ── Prices ──────────────────────────────────────────────────────── */

static void
test_unpriced_turns_are_recognised(void)
{
	/* -1 is unknown all the way through */
	g_assert_false(clawt_usage_turn_is_priced(-1, 10, 100));
	/* libreclaw's 0 for "no figure and no price table" */
	g_assert_false(clawt_usage_turn_is_priced(0, 10, 100));
	/* a real price, however small */
	g_assert_true(clawt_usage_turn_is_priced(1, 10, 100));
	/* nothing moved and nothing charged is a real zero */
	g_assert_true(clawt_usage_turn_is_priced(0, 0, 0));
}

/* ── Grouping ────────────────────────────────────────────────────── */

static void
test_group_enumeration(void)
{
	guint i;

	g_assert_cmpuint(clawt_usage_group_count(), ==, 4);
	g_assert_cmpstr(clawt_usage_group_nth(0), ==, "agent");
	g_assert_cmpstr(clawt_usage_group_label(0), ==, "Agent");

	for (i = 0; i < clawt_usage_group_count(); i++)
		g_assert_cmpint(clawt_usage_group_index(clawt_usage_group_nth(i)), ==,
				(gint)i);

	g_assert_cmpint(clawt_usage_group_index("provider"), ==, -1);
	g_assert_cmpint(clawt_usage_group_index(NULL), ==, -1);
}

/*
 * The same five turns, four ways.  Unpriced turns are counted in every
 * grouping and added to no cost, and the total agrees with the groups.
 */
static void
test_grouping_and_unpriced_handling(void)
{
	static const gchar *const bys[] = { "agent", "model", "origin", "day" };
	g_autoptr(GDateTime) day_one = g_date_time_new_local(2026, 11, 10, 9, 0, 0);
	g_autoptr(GDateTime) day_two = g_date_time_new_local(2026, 11, 11, 23, 30, 0);
	gint64 one = g_date_time_to_unix(day_one);
	gint64 two = g_date_time_to_unix(day_two);
	guint b;

	for (b = 0; b < G_N_ELEMENTS(bys); b++) {
		g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new(bys[b]);
		g_autoptr(JsonNode) node = NULL;
		JsonObject *reply;
		JsonObject *total;
		JsonArray *groups;
		gint64 sum = 0;
		gint64 unpriced = 0;
		guint i;

		clawt_usage_ledger_add_turn(ledger, "chief", one, "sonnet",
					    "operator", 10, 100, 20000);
		clawt_usage_ledger_add_turn(ledger, "chief", one, "sonnet",
					    "routine:morning", 10, 100, 30000);
		clawt_usage_ledger_add_turn(ledger, "worker", two, "opus",
					    "routine:morning", 10, 100, 500000);
		/* two with no price: -1, and libreclaw's 0-with-tokens */
		clawt_usage_ledger_add_turn(ledger, "worker", two, "local",
					    NULL, 10, 100, -1);
		clawt_usage_ledger_add_turn(ledger, "worker", two, "local",
					    NULL, 10, 100, 0);

		node = clawt_usage_ledger_to_json(ledger);
		reply = json_node_get_object(node);
		total = json_object_get_object_member(reply, "total");
		groups = json_object_get_array_member(reply, "groups");

		g_assert_cmpstr(json_object_get_string_member(reply, "by"), ==, bys[b]);
		g_assert_cmpint(int_of(total, "turns"), ==, 5);
		g_assert_cmpint(int_of(total, "unpriced_turns"), ==, 2);
		/* the -1 did not subtract a micro-dollar */
		g_assert_cmpint(int_of(total, "cost_micros"), ==, 550000);

		for (i = 0; i < json_array_get_length(groups); i++) {
			JsonObject *group = json_array_get_object_element(groups, i);

			sum += int_of(group, "cost_micros");
			unpriced += int_of(group, "unpriced_turns");
		}
		g_assert_cmpint(sum, ==, 550000);
		g_assert_cmpint(unpriced, ==, 2);
	}

	/* by model: most expensive first, unpriced kept apart */
	{
		g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("model");
		g_autoptr(JsonNode) node = NULL;
		JsonObject *reply;
		JsonArray *groups;
		g_autofree gchar *cost = NULL;

		clawt_usage_ledger_add_turn(ledger, "a", one, "sonnet", NULL, 1, 1, 100);
		clawt_usage_ledger_add_turn(ledger, "a", one, "opus", NULL, 1, 1, 900);
		clawt_usage_ledger_add_turn(ledger, "a", one, "local", NULL, 1, 1, -1);
		clawt_usage_ledger_add_turn(ledger, "a", one, NULL, NULL, 1, 1, 5);

		node = clawt_usage_ledger_to_json(ledger);
		reply = json_node_get_object(node);
		groups = json_object_get_array_member(reply, "groups");

		g_assert_cmpstr(json_object_get_string_member(
					json_array_get_object_element(groups, 0), "key"),
				==, "opus");
		g_assert_cmpint(int_of(group_named(reply, "local"), "unpriced_turns"),
				==, 1);
		g_assert_cmpint(int_of(group_named(reply, "local"), "cost_micros"), ==, 0);
		g_assert_nonnull(group_named(reply, "(unknown)"));

		/* "$0.00" would say free; nobody knows */
		cost = clawt_usage_ledger_cost_label(group_named(reply, "local"));
		g_assert_cmpstr(cost, ==, "unpriced");
	}

	/* by origin: untagged rows are named, not dropped */
	{
		g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("origin");
		g_autoptr(JsonNode) node = NULL;
		JsonObject *reply;

		clawt_usage_ledger_add_turn(ledger, "a", one, "m", "routine:morning",
					    1, 1, 100);
		clawt_usage_ledger_add_turn(ledger, "b", one, "m", "routine:morning",
					    1, 1, 200);
		clawt_usage_ledger_add_turn(ledger, "a", one, "m", NULL, 1, 1, 50);

		node = clawt_usage_ledger_to_json(ledger);
		reply = json_node_get_object(node);
		g_assert_cmpint(int_of(group_named(reply, "routine:morning"),
				       "cost_micros"), ==, 300);
		g_assert_cmpint(int_of(group_named(reply, "routine:morning"), "turns"),
				==, 2);
		g_assert_cmpint(int_of(group_named(reply, "untagged"), "cost_micros"),
				==, 50);
	}

	/* by day: the person's local dates, oldest first */
	{
		g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("day");
		g_autoptr(JsonNode) node = NULL;
		JsonArray *groups;

		clawt_usage_ledger_add_turn(ledger, "a", two, "m", NULL, 1, 1, 900);
		clawt_usage_ledger_add_turn(ledger, "a", one, "m", NULL, 1, 1, 100);

		node = clawt_usage_ledger_to_json(ledger);
		groups = json_object_get_array_member(json_node_get_object(node),
						      "groups");
		g_assert_cmpuint(json_array_get_length(groups), ==, 2);
		g_assert_cmpstr(json_object_get_string_member(
					json_array_get_object_element(groups, 0), "key"),
				==, "2026-11-10");
		g_assert_cmpstr(json_object_get_string_member(
					json_array_get_object_element(groups, 1), "key"),
				==, "2026-11-11");
	}

	/* an unknown grouping is the default, not a crash */
	{
		g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("nonsense");

		g_assert_cmpstr(clawt_usage_ledger_get_by(ledger), ==, "agent");
	}
}

/* The words, which every client draws. */
static void
test_row_wording(void)
{
	g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("agent");
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *subtitle = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *cost = NULL;
	JsonObject *reply;

	clawt_usage_ledger_add_turn(ledger, "chief", 1, "m", NULL, 1, 4210, 20000);
	clawt_usage_ledger_add_turn(ledger, "chief", 1, "m", NULL, 1, 0, -1);

	node = clawt_usage_ledger_to_json(ledger);
	reply = json_node_get_object(node);

	subtitle = clawt_usage_ledger_row_subtitle(group_named(reply, "chief"));
	g_assert_cmpstr(subtitle, ==,
			"2 turns \xc2\xb7 1 unpriced \xc2\xb7 4,210 tokens out");

	/* a mixed group shows what is known; the unpriced part is beside it */
	cost = clawt_usage_ledger_cost_label(group_named(reply, "chief"));
	g_assert_cmpstr(cost, ==, "$0.02");

	line = clawt_usage_ledger_unpriced_line(reply);
	g_assert_nonnull(line);
	g_assert_nonnull(strstr(line, "1 turn has no recorded price"));
	g_assert_nonnull(strstr(line, "not free"));

	/* and nothing to say when every turn has a price */
	{
		g_autoptr(ClawtUsageLedger) priced = clawt_usage_ledger_new("agent");
		g_autoptr(JsonNode) priced_node = NULL;

		clawt_usage_ledger_add_turn(priced, "chief", 1, "m", NULL, 1, 1, 5);
		priced_node = clawt_usage_ledger_to_json(priced);
		g_assert_null(clawt_usage_ledger_unpriced_line(
				      json_node_get_object(priced_node)));
	}
}

/* ── The ledger file ─────────────────────────────────────────────── */

/*
 * Costs from libreclaw's rows, origins from clawtilla's tags, joined on
 * the row id -- with the window applied to both, and a row with no tag
 * reported as untagged.
 */
static void
test_add_agent_joins_tags_to_rows(void)
{
	Scratch scratch = { 0 };
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *missing = NULL;
	g_autofree gchar *missing_db = NULL;
	g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("origin");
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *reply;
	gint64 first;
	gint64 second;

	scratch_setup(&scratch);
	state_dir = make_state_dir(scratch.dir, "chief");

	add_row(state_dir, "old", "clawtilla", "sonnet", 1, 1, 7000, 1000);
	first = add_row(state_dir, "dm:chief:user", "clawtilla", "sonnet",
			10, 100, 20000, 5000);
	second = add_row(state_dir, "routine:morning", "clawtilla", "opus",
			 10, 100, 40000, 5001);
	add_row(state_dir, "!abc:matrix", "matrix", "sonnet", 10, 100, 1000, 5002);

	tag_row(state_dir, first, 5000, CLAWT_USAGE_ORIGIN_OPERATOR, NULL);
	tag_row(state_dir, second, 5001, CLAWT_USAGE_ORIGIN_ROUTINE, "morning");

	g_assert_true(clawt_usage_ledger_add_agent(ledger, "chief", state_dir, 2000,
						   &error));
	g_assert_no_error(error);

	node = clawt_usage_ledger_to_json(ledger);
	reply = json_node_get_object(node);

	g_assert_cmpint(int_of(group_named(reply, "operator"), "cost_micros"), ==,
			20000);
	g_assert_cmpint(int_of(group_named(reply, "routine:morning"), "cost_micros"),
			==, 40000);
	g_assert_cmpint(int_of(group_named(reply, "untagged"), "turns"), ==, 1);
	/* the row before the window is in no group */
	g_assert_cmpint(int_of(json_object_get_object_member(reply, "total"),
			       "turns"), ==, 3);

	/* an agent that never ran: no history, and no file made to say so */
	missing = make_state_dir(scratch.dir, "never");
	missing_db = clawt_usage_database_path(missing);
	g_assert_true(clawt_usage_ledger_add_agent(ledger, "never", missing, 0,
						   &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(missing_db, G_FILE_TEST_EXISTS));

	scratch_teardown(&scratch);
}

/*
 * A ledger file written before a column existed is brought up to date
 * rather than refused, and a second tag for a row replaces the first.
 */
static void
test_ledger_migrates_additively(void)
{
	Scratch scratch = { 0 };
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(ClawtUsageOrigin) origin = NULL;
	g_autoptr(GError) error = NULL;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	gint columns = 0;

	scratch_setup(&scratch);
	state_dir = make_state_dir(scratch.dir, "chief");
	path = clawt_usage_ledger_path(state_dir);

	/* the oldest shape: the key columns and nothing else */
	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db,
				     "CREATE TABLE turn_origin (usage_id INTEGER"
				     " PRIMARY KEY, recorded_at INTEGER NOT NULL,"
				     " origin_kind TEXT NOT NULL)",
				     NULL, NULL, NULL), ==, SQLITE_OK);
	sqlite3_close(db);
	db = NULL;

	origin = clawt_usage_origin_new(CLAWT_USAGE_ORIGIN_AGENT, "chief");
	origin->sender = g_strdup("chief");
	origin->task_id = g_strdup("t-1");
	origin->provider = g_strdup("claude-code");
	origin->model = g_strdup("opus");
	g_assert_true(clawt_usage_ledger_record(path, 1, 10, "room", origin, &error));
	g_assert_no_error(error);

	/* replaced, not duplicated */
	g_assert_true(clawt_usage_ledger_record(path, 1, 10, "room", origin, &error));
	g_assert_no_error(error);

	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA table_info(turn_origin)", -1,
					   &stmt, NULL), ==, SQLITE_OK);
	while (sqlite3_step(stmt) == SQLITE_ROW)
		columns++;
	sqlite3_finalize(stmt);
	g_assert_cmpint(columns, ==, 9);

	g_assert_cmpint(sqlite3_prepare_v2(db,
					   "SELECT count(*), provider, task_id"
					   " FROM turn_origin", -1, &stmt, NULL),
			==, SQLITE_OK);
	g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
	g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 1);
	g_assert_cmpstr((const gchar *)sqlite3_column_text(stmt, 1), ==,
			"claude-code");
	g_assert_cmpstr((const gchar *)sqlite3_column_text(stmt, 2), ==, "t-1");
	sqlite3_finalize(stmt);
	sqlite3_close(db);

	scratch_teardown(&scratch);
}

/* What the tagger saw, and what it answers. */
typedef struct {
	guint  calls;
	gchar *room;
	gchar *channel;
} Tagger;

static ClawtUsageOrigin *
tag_as_operator(
	const gchar *agent_id,
	const gchar *room,
	const gchar *channel,
	gpointer     user_data
){
	Tagger *tagger = user_data;

	tagger->calls++;
	g_free(tagger->room);
	g_free(tagger->channel);
	tagger->room = g_strdup(room);
	tagger->channel = g_strdup(channel);

	return clawt_usage_origin_new(CLAWT_USAGE_ORIGIN_OPERATOR, NULL);
}

/*
 * The drain tags each new turn once, with the room libreclaw recorded,
 * and leaves history from before the baseline untagged -- it began
 * under a daemon that did not see it start.
 */
static void
test_drain_tags_new_turns_only(void)
{
	Scratch scratch = { 0 };
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *db_path = NULL;
	g_autoptr(ClawtUsage) usage = clawt_usage_new();
	g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("origin");
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	Tagger tagger = { 0, NULL, NULL };
	JsonObject *reply;

	scratch_setup(&scratch);
	state_dir = make_state_dir(scratch.dir, "chief");
	db_path = clawt_usage_database_path(state_dir);

	add_row(state_dir, "before", "clawtilla", "sonnet", 1, 1, 9000, 100);
	g_assert_true(clawt_usage_prime(usage, "chief", db_path, &error));
	g_assert_no_error(error);

	add_row(state_dir, "dm:chief:user", "clawtilla", "sonnet", 1, 1, 2000, 200);
	g_assert_cmpint(clawt_usage_drain_tagged(usage, "chief", db_path,
						 tag_as_operator, &tagger), ==, 2000);
	g_assert_cmpuint(tagger.calls, ==, 1);
	g_assert_cmpstr(tagger.room, ==, "dm:chief:user");
	g_assert_cmpstr(tagger.channel, ==, "clawtilla");

	/* nothing new: nothing tagged again */
	g_assert_cmpint(clawt_usage_drain_tagged(usage, "chief", db_path,
						 tag_as_operator, &tagger), ==, 0);
	g_assert_cmpuint(tagger.calls, ==, 1);

	g_assert_true(clawt_usage_ledger_add_agent(ledger, "chief", state_dir, 0,
						   &error));
	g_assert_no_error(error);
	node = clawt_usage_ledger_to_json(ledger);
	reply = json_node_get_object(node);
	g_assert_cmpint(int_of(group_named(reply, "operator"), "cost_micros"), ==,
			2000);
	g_assert_cmpint(int_of(group_named(reply, "untagged"), "cost_micros"), ==,
			9000);

	g_free(tagger.room);
	g_free(tagger.channel);
	scratch_teardown(&scratch);
}

/* ── CSV ─────────────────────────────────────────────────────────── */

static void
test_csv_cells_are_quoted_and_neutralised(void)
{
	static const struct {
		const gchar *in;
		const gchar *out;
	} cases[] = {
		{ "sonnet",            "sonnet" },
		{ NULL,                "" },
		{ "",                  "" },
		{ "a,b",               "\"a,b\"" },
		{ "say \"hi\"",        "\"say \"\"hi\"\"\"" },
		{ "two\nlines",        "\"two\nlines\"" },
		{ " padded",           "\" padded\"" },
		/* each opener a spreadsheet would evaluate */
		{ "=1+1",              "'=1+1" },
		{ "+cmd",              "'+cmd" },
		{ "-2",                "'-2" },
		{ "@SUM(A1)",          "'@SUM(A1)" },
		{ "\tx",               "'\tx" },
		{ "\rx",               "\"'\rx\"" },
		/* neutralised first, then quoted, with the quote doubled */
		{ "=HYPERLINK(\"x\",\"y\")", "\"'=HYPERLINK(\"\"x\"\",\"\"y\"\")\"" },
		/* not an opener anywhere but the start */
		{ "a=b",               "a=b" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *cell = clawt_usage_csv_cell(cases[i].in);

		g_assert_cmpstr(cell, ==, cases[i].out);
	}
}

static void
test_csv_document(void)
{
	g_autoptr(ClawtUsageLedger) ledger = clawt_usage_ledger_new("model");
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GDateTime) when = g_date_time_new_local(2026, 11, 14, 12, 0, 0);
	JsonObject *reply;

	clawt_usage_ledger_add_turn(ledger, "a", 1, "=evil()", NULL, 3, 4, 1230000);
	clawt_usage_ledger_add_turn(ledger, "a", 1, "local,model", NULL, 5, 6, -1);

	node = clawt_usage_ledger_to_json(ledger);
	reply = json_node_get_object(node);
	csv = clawt_usage_ledger_csv(reply);

	/*
	 * Exact dollars from integer micro-dollars; an empty cost for the
	 * group nobody priced; CRLF; no total line to double a sum.
	 */
	g_assert_cmpstr(csv, ==,
			"model,turns,unpriced_turns,input_tokens,output_tokens,cost_usd\r\n"
			"'=evil(),1,0,3,4,1.230000\r\n"
			"\"local,model\",1,1,5,6,\r\n");

	name = clawt_usage_ledger_csv_name(reply, g_date_time_to_unix(when));
	g_assert_cmpstr(name, ==, "clawtilla-spending-by-model-2026-11-14.csv");
}

static void
test_ledger_payload(void)
{
	g_autoptr(JsonNode) payload = clawt_usage_ledger_payload(42, "origin",
								 "chief");
	g_autoptr(JsonNode) fallback = clawt_usage_ledger_payload(0, "bogus", NULL);
	JsonObject *object = json_node_get_object(payload);

	g_assert_cmpint(json_object_get_int_member(object, "since"), ==, 42);
	g_assert_cmpstr(json_object_get_string_member(object, "by"), ==, "origin");
	g_assert_cmpstr(json_object_get_string_member(object, "agent"), ==, "chief");

	object = json_node_get_object(fallback);
	g_assert_cmpstr(json_object_get_string_member(object, "by"), ==, "agent");
	g_assert_false(json_object_has_member(object, "agent"));
}

/* ── Through the daemon ──────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	gchar        *config_path;
	ClawtDaemon  *daemon;
	GMainContext *context;
} DaemonFixture;

static void
daemon_setup(
	DaemonFixture *fixture
){
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;
	gboolean started;

	fixture->dir = g_dir_make_tmp("clawt-ledger-daemon-XXXXXX", NULL);
	fixture->config_path = g_build_filename(fixture->dir, "config.yaml", NULL);

	/* every path the daemon would otherwise take from $HOME, pinned */
	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"agents:\n"
		"  - id: chief\n"
		"  - id: worker\n"
		"routines:\n"
		"  - id: morning\n"
		"    agent: chief\n"
		"    instructions: \"brief me\"\n"
		"    schedule: manual\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, fixture->dir);
	g_file_set_contents(fixture->config_path, yaml, -1, &error);
	g_assert_no_error(error);

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(fixture->config_path, fixture->context);
	started = clawt_daemon_start(fixture->daemon, &error);
	g_assert_no_error(error);
	g_assert_true(started);
}

static void
daemon_teardown(
	DaemonFixture *fixture
){
	if (fixture->daemon != NULL) {
		clawt_daemon_stop(fixture->daemon);
		g_clear_object(&fixture->daemon);
	}

	/* a stopped listener finishes on the next iteration */
	if (fixture->context != NULL)
		while (g_main_context_iteration(fixture->context, FALSE))
			;

	g_clear_pointer(&fixture->context, g_main_context_unref);
	if (fixture->dir != NULL)
		clawt_test_remove_tree(fixture->dir);
	g_clear_pointer(&fixture->dir, g_free);
	g_clear_pointer(&fixture->config_path, g_free);
}

static JsonNode *
daemon_request(
	DaemonFixture *fixture,
	const gchar   *kind,
	const gchar   *payload_json
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
	clawt_ipc_frame_set_payload(frame,
				    json_node_copy(json_parser_get_root(parser)));

	return clawt_daemon_handle_request(fixture->daemon, frame);
}

/* The label the daemon gives a turn in @room of @agent_id. */
static gchar *
daemon_label(
	DaemonFixture *fixture,
	const gchar   *agent_id,
	const gchar   *room,
	const gchar   *channel
){
	g_autoptr(ClawtUsageOrigin) origin = clawt_daemon_usage_origin(
		fixture->daemon, agent_id, room, channel);

	return clawt_usage_origin_label(origin->kind, origin->id);
}

/*
 * Each origin kind, from the turn state the daemon actually keeps --
 * with two agents and two rooms, since a turn table read by the wrong
 * key looks right with one of each.
 */
static void
test_daemon_tags_each_origin_from_turn_state(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) ran = NULL;
	ClawtAgent *chief;
	ClawtAgent *worker;
	ClawtTask *child;
	const gchar *routine_task;
	gchar *label;

	daemon_setup(&fixture);
	chief = clawt_agent_manager_get(fixture.daemon->agents, "chief");
	worker = clawt_agent_manager_get(fixture.daemon->agents, "worker");
	g_assert_nonnull(chief);
	g_assert_nonnull(worker);

	/* the operator, in their room */
	clawt_agent_deliver_turn(chief, "dm:chief:user", 0, TRUE, "user", NULL);
	clawt_agent_note_typing(chief, "dm:chief:user", TRUE);
	label = daemon_label(&fixture, "chief", "dm:chief:user", "clawtilla");
	g_assert_cmpstr(label, ==, "operator");
	g_free(label);

	/* a peer, in another room of the same agent */
	clawt_agent_deliver_turn(chief, "dm:chief:worker", 1, TRUE, "worker", NULL);
	clawt_agent_note_typing(chief, "dm:chief:worker", TRUE);
	label = daemon_label(&fixture, "chief", "dm:chief:worker", "clawtilla");
	g_assert_cmpstr(label, ==, "agent:worker");
	g_free(label);

	/* and the first room still answers for itself, not for the last */
	label = daemon_label(&fixture, "chief", "dm:chief:user", "clawtilla");
	g_assert_cmpstr(label, ==, "operator");
	g_free(label);

	/* a routine's run, through the frame a client sends */
	ran = daemon_request(&fixture, "routine.run", "{\"id\":\"morning\"}");
	g_assert_false(clawt_ipc_frame_is_error(ran));
	routine_task = clawt_ipc_payload_string(clawt_ipc_frame_get_payload(ran),
						"task");
	g_assert_nonnull(routine_task);

	clawt_agent_deliver_turn(chief, "dm:chief:user", 0, TRUE, "user",
				 routine_task);
	clawt_agent_note_typing(chief, "dm:chief:user", FALSE);
	clawt_agent_note_typing(chief, "dm:chief:user", TRUE);
	label = daemon_label(&fixture, "chief", "dm:chief:user", "clawtilla");
	g_assert_cmpstr(label, ==, "routine:morning");
	g_free(label);

	/* work the routine handed on is still the routine's bill */
	child = clawt_task_manager_create(fixture.daemon->tasks, "chief", "worker",
					  "do the part", routine_task, NULL);
	g_assert_nonnull(child);
	clawt_agent_deliver_turn(worker, "dm:chief:worker", 1, TRUE, "chief",
				 clawt_task_get_id(child));
	clawt_agent_note_typing(worker, "dm:chief:worker", TRUE);
	label = daemon_label(&fixture, "worker", "dm:chief:worker", "clawtilla");
	g_assert_cmpstr(label, ==, "routine:morning");
	g_free(label);

	/* a trigger's run, noted where run_trigger() notes it */
	{
		ClawtTask *fired = clawt_task_manager_create(
			fixture.daemon->tasks, "trigger", "worker", "on push", NULL,
			NULL);

		clawt_daemon_note_task_source(fixture.daemon,
					      clawt_task_get_id(fired),
					      CLAWT_USAGE_ORIGIN_TRIGGER, "push");
		clawt_agent_deliver_turn(worker, "trigger:push", 0, TRUE, "trigger",
					 clawt_task_get_id(fired));
		clawt_agent_note_typing(worker, "trigger:push", TRUE);
		label = daemon_label(&fixture, "worker", "trigger:push", "clawtilla");
		g_assert_cmpstr(label, ==, "trigger:push");
		g_free(label);
	}

	/* a task whose run is not on record names the task */
	{
		ClawtTask *orphan = clawt_task_manager_create(
			fixture.daemon->tasks, "routine", "worker", "x", NULL, NULL);
		g_autofree gchar *expected = g_strdup_printf(
			"task:%s", clawt_task_get_id(orphan));

		clawt_agent_deliver_turn(worker, "routine:gone", 0, TRUE, "routine",
					 clawt_task_get_id(orphan));
		clawt_agent_note_typing(worker, "routine:gone", TRUE);
		label = daemon_label(&fixture, "worker", "routine:gone", "clawtilla");
		g_assert_cmpstr(label, ==, expected);
		g_free(label);
	}

	/* the agent's own channel, and a room with nothing on record */
	label = daemon_label(&fixture, "worker", "!room:matrix", "matrix");
	g_assert_cmpstr(label, ==, "channel:matrix");
	g_free(label);
	label = daemon_label(&fixture, "worker", "nowhere", "clawtilla");
	g_assert_cmpstr(label, ==, "untagged");
	g_free(label);

	daemon_teardown(&fixture);
}

/* `usage.ledger` over the wire: grouped, filtered, and strict. */
static void
test_daemon_answers_usage_ledger(void)
{
	DaemonFixture fixture = { 0 };
	g_autofree gchar *chief_dir = NULL;
	g_autofree gchar *worker_dir = NULL;
	g_autoptr(JsonNode) by_origin = NULL;
	g_autoptr(JsonNode) one_agent = NULL;
	g_autoptr(JsonNode) bad_by = NULL;
	g_autoptr(JsonNode) bad_agent = NULL;
	JsonObject *reply;
	gint64 row;

	daemon_setup(&fixture);
	chief_dir = clawt_config_agent_state_dir(fixture.daemon->config, "chief");
	worker_dir = clawt_config_agent_state_dir(fixture.daemon->config, "worker");
	g_assert_cmpint(g_mkdir_with_parents(chief_dir, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(worker_dir, 0700), ==, 0);
	{
		g_autofree gchar *s1 = g_build_filename(chief_dir, "sessions", NULL);
		g_autofree gchar *s2 = g_build_filename(worker_dir, "sessions", NULL);

		g_assert_cmpint(g_mkdir_with_parents(s1, 0700), ==, 0);
		g_assert_cmpint(g_mkdir_with_parents(s2, 0700), ==, 0);
	}

	row = add_row(chief_dir, "r", "clawtilla", "sonnet", 1, 1, 3000, 100);
	tag_row(chief_dir, row, 100, CLAWT_USAGE_ORIGIN_ROUTINE, "morning");
	row = add_row(worker_dir, "r", "clawtilla", "opus", 1, 1, 4000, 100);
	tag_row(worker_dir, row, 100, CLAWT_USAGE_ORIGIN_ROUTINE, "morning");
	add_row(worker_dir, "r", "clawtilla", "opus", 1, 1, -1, 100);

	by_origin = daemon_request(&fixture, "usage.ledger", "{\"by\":\"origin\"}");
	g_assert_false(clawt_ipc_frame_is_error(by_origin));
	reply = clawt_ipc_frame_get_payload(by_origin);
	g_assert_cmpstr(json_object_get_string_member(reply, "by"), ==, "origin");
	g_assert_cmpint(int_of(group_named(reply, "routine:morning"),
			       "cost_micros"), ==, 7000);
	g_assert_cmpint(int_of(json_object_get_object_member(reply, "total"),
			       "unpriced_turns"), ==, 1);

	one_agent = daemon_request(&fixture, "usage.ledger",
				   "{\"by\":\"model\",\"agent\":\"chief\"}");
	reply = clawt_ipc_frame_get_payload(one_agent);
	g_assert_cmpint(int_of(json_object_get_object_member(reply, "total"),
			       "turns"), ==, 1);
	g_assert_nonnull(group_named(reply, "sonnet"));
	g_assert_null(group_named(reply, "opus"));

	/* refused, rather than drawn under the wrong heading */
	bad_by = daemon_request(&fixture, "usage.ledger", "{\"by\":\"provider\"}");
	g_assert_true(clawt_ipc_frame_is_error(bad_by));
	bad_agent = daemon_request(&fixture, "usage.ledger",
				   "{\"agent\":\"nobody\"}");
	g_assert_true(clawt_ipc_frame_is_error(bad_agent));

	daemon_teardown(&fixture);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/usage-ledger/origin-kinds",
			test_each_origin_kind_is_classified);
	g_test_add_func("/usage-ledger/origin-nicks",
			test_origin_kind_nicks_round_trip);
	g_test_add_func("/usage-ledger/unpriced", test_unpriced_turns_are_recognised);
	g_test_add_func("/usage-ledger/group-enumeration", test_group_enumeration);
	g_test_add_func("/usage-ledger/grouping",
			test_grouping_and_unpriced_handling);
	g_test_add_func("/usage-ledger/row-wording", test_row_wording);
	g_test_add_func("/usage-ledger/join", test_add_agent_joins_tags_to_rows);
	g_test_add_func("/usage-ledger/migrate", test_ledger_migrates_additively);
	g_test_add_func("/usage-ledger/drain-tags", test_drain_tags_new_turns_only);
	g_test_add_func("/usage-ledger/csv-cells",
			test_csv_cells_are_quoted_and_neutralised);
	g_test_add_func("/usage-ledger/csv-document", test_csv_document);
	g_test_add_func("/usage-ledger/payload", test_ledger_payload);
	g_test_add_func("/usage-ledger/daemon-origins",
			test_daemon_tags_each_origin_from_turn_state);
	g_test_add_func("/usage-ledger/daemon-ledger",
			test_daemon_answers_usage_ledger);

	return g_test_run();
}
