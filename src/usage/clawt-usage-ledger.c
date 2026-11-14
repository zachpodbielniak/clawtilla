/*
 * clawt-usage-ledger.c - What the fleet spent, by model, by origin, by day
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * libreclaw writes one `token_usage` row per AI turn: when, which model,
 * the tokens and the cost.  What it cannot write is *why* the turn
 * happened -- the operator, a peer, a routine at six in the morning --
 * because that is clawtilla's knowledge, held in the turn state of the
 * agent that ran it.  So the cost stays where libreclaw keeps it and is
 * read through lc_database_*, and clawtilla keeps a tag per row in a
 * file of its own beside it.  The two are joined on the row id when the
 * ledger is read; a row with no tag is reported as untagged rather than
 * guessed at.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#include "usage/clawt-usage-ledger.h"

/* ── Origins ─────────────────────────────────────────────────────── */

/* No `default:`: a kind added later is a -Wswitch warning here. */
const gchar *
clawt_usage_origin_kind_nick(
	ClawtUsageOriginKind kind
){
	switch (kind) {
	case CLAWT_USAGE_ORIGIN_UNKNOWN:
		return "untagged";
	case CLAWT_USAGE_ORIGIN_OPERATOR:
		return "operator";
	case CLAWT_USAGE_ORIGIN_AGENT:
		return "agent";
	case CLAWT_USAGE_ORIGIN_ROUTINE:
		return "routine";
	case CLAWT_USAGE_ORIGIN_TRIGGER:
		return "trigger";
	case CLAWT_USAGE_ORIGIN_TASK:
		return "task";
	case CLAWT_USAGE_ORIGIN_CHANNEL:
		return "channel";
	case CLAWT_USAGE_ORIGIN_OTHER:
		return "other";
	}
	return "untagged";
}

ClawtUsageOriginKind
clawt_usage_origin_kind_from_nick(
	const gchar *nick
){
	gint kind;

	if (nick == NULL)
		return CLAWT_USAGE_ORIGIN_UNKNOWN;

	/* walked from the nick function, so the two cannot disagree */
	for (kind = (gint)CLAWT_USAGE_ORIGIN_UNKNOWN;
	     kind <= (gint)CLAWT_USAGE_ORIGIN_OTHER; kind++) {
		if (g_strcmp0(nick, clawt_usage_origin_kind_nick(
				(ClawtUsageOriginKind)kind)) == 0)
			return (ClawtUsageOriginKind)kind;
	}

	return CLAWT_USAGE_ORIGIN_UNKNOWN;
}

static gboolean
is_set(
	const gchar *value
){
	return value != NULL && *value != '\0';
}

ClawtUsageOriginKind
clawt_usage_origin_classify(
	const gchar  *sender,
	gboolean      sender_is_agent,
	const gchar  *task_id,
	const gchar  *routine_id,
	const gchar  *trigger_id,
	const gchar  *channel,
	const gchar **out_id
){
	const gchar *id = NULL;
	ClawtUsageOriginKind kind;

	/*
	 * The run that began the work wins over whoever delivered this
	 * turn of it: a routine that delegates to three agents is one bill,
	 * and charged to the delegating agent it would vanish from the
	 * answer to "which routine costs me most".
	 */
	if (is_set(routine_id)) {
		kind = CLAWT_USAGE_ORIGIN_ROUTINE;
		id = routine_id;
	} else if (is_set(trigger_id)) {
		kind = CLAWT_USAGE_ORIGIN_TRIGGER;
		id = trigger_id;
	} else if (!is_set(sender)) {
		/*
		 * No delivery set this turn up.  A turn on one of the agent's
		 * own channels never went through the router, and saying which
		 * channel is an answer; on clawtilla's own it is a turn the
		 * state lost, and a guess would be worse than "untagged".
		 */
		if (is_set(channel) && g_strcmp0(channel, "clawtilla") != 0) {
			kind = CLAWT_USAGE_ORIGIN_CHANNEL;
			id = channel;
		} else {
			kind = CLAWT_USAGE_ORIGIN_UNKNOWN;
		}
	} else if (g_strcmp0(sender, "user") == 0) {
		kind = CLAWT_USAGE_ORIGIN_OPERATOR;
	} else if (sender_is_agent) {
		kind = CLAWT_USAGE_ORIGIN_AGENT;
		id = sender;
	} else if (is_set(task_id)) {
		/* "routine" or "trigger" whose run the daemon no longer knows */
		kind = CLAWT_USAGE_ORIGIN_TASK;
		id = task_id;
	} else {
		kind = CLAWT_USAGE_ORIGIN_OTHER;
		id = sender;
	}

	if (out_id != NULL)
		*out_id = id;

	return kind;
}

gchar *
clawt_usage_origin_label(
	ClawtUsageOriginKind kind,
	const gchar         *id
){
	if (kind == CLAWT_USAGE_ORIGIN_OPERATOR || kind == CLAWT_USAGE_ORIGIN_UNKNOWN ||
	    !is_set(id))
		return g_strdup(clawt_usage_origin_kind_nick(kind));

	return g_strdup_printf("%s:%s", clawt_usage_origin_kind_nick(kind), id);
}

ClawtUsageOrigin *
clawt_usage_origin_new(
	ClawtUsageOriginKind kind,
	const gchar         *id
){
	ClawtUsageOrigin *self;

	self = g_new0(ClawtUsageOrigin, 1);
	self->kind = kind;
	self->id = g_strdup(id);

	return self;
}

ClawtUsageOrigin *
clawt_usage_origin_copy(
	const ClawtUsageOrigin *self
){
	ClawtUsageOrigin *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = clawt_usage_origin_new(self->kind, self->id);
	copy->sender = g_strdup(self->sender);
	copy->task_id = g_strdup(self->task_id);
	copy->provider = g_strdup(self->provider);
	copy->model = g_strdup(self->model);

	return copy;
}

void
clawt_usage_origin_free(
	ClawtUsageOrigin *self
){
	if (self == NULL)
		return;

	g_free(self->id);
	g_free(self->sender);
	g_free(self->task_id);
	g_free(self->provider);
	g_free(self->model);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtUsageOrigin, clawt_usage_origin,
		    clawt_usage_origin_copy, clawt_usage_origin_free)

/* ── Prices ──────────────────────────────────────────────────────── */

gboolean
clawt_usage_turn_is_priced(
	gint64 cost_micros,
	gint64 input_tokens,
	gint64 output_tokens
){
	if (cost_micros < 0)
		return FALSE;

	/*
	 * libreclaw writes 0 for a turn whose provider reported no cost when
	 * it has no price table to estimate from.  A turn that moved tokens
	 * and recorded nothing is one nobody priced, not one that was free.
	 */
	if (cost_micros == 0 && (input_tokens > 0 || output_tokens > 0))
		return FALSE;

	return TRUE;
}

/* ── The ledger file ─────────────────────────────────────────────── */

#define LEDGER_FILE "clawtilla-ledger.db"

gchar *
clawt_usage_ledger_path_beside(
	const gchar *db_path
){
	g_autofree gchar *dir = NULL;

	g_return_val_if_fail(db_path != NULL, NULL);

	dir = g_path_get_dirname(db_path);
	return g_build_filename(dir, LEDGER_FILE, NULL);
}

gchar *
clawt_usage_ledger_path(
	const gchar *state_dir
){
	g_autofree gchar *db_path = NULL;

	g_return_val_if_fail(state_dir != NULL, NULL);

	db_path = clawt_usage_database_path(state_dir);
	return clawt_usage_ledger_path_beside(db_path);
}

static void
set_sqlite_error(
	GError     **error,
	sqlite3     *db,
	const gchar *doing
){
	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "usage ledger: %s: %s",
		    doing, db != NULL ? sqlite3_errmsg(db) : "out of memory");
}

static gboolean
has_column(
	sqlite3     *db,
	const gchar *column
){
	sqlite3_stmt *stmt = NULL;
	gboolean found = FALSE;

	if (sqlite3_prepare_v2(db, "PRAGMA table_info(turn_origin)", -1, &stmt,
			       NULL) != SQLITE_OK)
		return FALSE;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (g_strcmp0((const gchar *)sqlite3_column_text(stmt, 1),
			      column) == 0) {
			found = TRUE;
			break;
		}
	}

	sqlite3_finalize(stmt);
	return found;
}

/*
 * The table, and the columns that bring an older file up to it.
 *
 * The key columns are created with the table and everything else is
 * added if missing, one ALTER at a time -- the same additive shape the
 * mailbox uses -- because a CREATE TABLE IF NOT EXISTS does nothing to a
 * file that already has the table, and a column a fleet's existing
 * files lack would fail their first write.
 */
static gboolean
apply_schema(
	sqlite3 *db,
	GError **error
){
	static const gchar *const columns[] = {
		"room", "origin_id", "sender", "task_id", "provider", "model"
	};
	guint i;

	if (sqlite3_exec(db,
			 "CREATE TABLE IF NOT EXISTS turn_origin ("
			 "  usage_id    INTEGER PRIMARY KEY,"
			 "  recorded_at INTEGER NOT NULL,"
			 "  origin_kind TEXT NOT NULL)",
			 NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "creating the table");
		return FALSE;
	}

	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		g_autofree gchar *sql = NULL;

		if (has_column(db, columns[i]))
			continue;

		sql = g_strdup_printf("ALTER TABLE turn_origin ADD COLUMN %s TEXT",
				      columns[i]);
		if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
			set_sqlite_error(error, db, "adding a column");
			return FALSE;
		}
	}

	return TRUE;
}

static void
bind_text_or_null(
	sqlite3_stmt *stmt,
	gint          index,
	const gchar  *value
){
	if (value != NULL)
		sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, index);
}

gboolean
clawt_usage_ledger_record(
	const gchar            *ledger_path,
	gint64                  usage_id,
	gint64                  recorded_at,
	const gchar            *room,
	const ClawtUsageOrigin *origin,
	GError                **error
){
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	gboolean ok = FALSE;

	g_return_val_if_fail(ledger_path != NULL, FALSE);
	g_return_val_if_fail(origin != NULL, FALSE);

	if (sqlite3_open(ledger_path, &db) != SQLITE_OK) {
		set_sqlite_error(error, db, "opening");
		goto out;
	}

	/* the agent's own sqlite connection writes beside this one */
	sqlite3_busy_timeout(db, 2000);

	if (!apply_schema(db, error))
		goto out;

	if (sqlite3_prepare_v2(db,
			       "INSERT OR REPLACE INTO turn_origin"
			       " (usage_id, recorded_at, origin_kind, room,"
			       "  origin_id, sender, task_id, provider, model)"
			       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
			       -1, &stmt, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "preparing a write");
		goto out;
	}

	sqlite3_bind_int64(stmt, 1, usage_id);
	sqlite3_bind_int64(stmt, 2, recorded_at);
	sqlite3_bind_text(stmt, 3, clawt_usage_origin_kind_nick(origin->kind), -1,
			  SQLITE_STATIC);
	bind_text_or_null(stmt, 4, room);
	bind_text_or_null(stmt, 5, origin->id);
	bind_text_or_null(stmt, 6, origin->sender);
	bind_text_or_null(stmt, 7, origin->task_id);
	bind_text_or_null(stmt, 8, origin->provider);
	bind_text_or_null(stmt, 9, origin->model);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		set_sqlite_error(error, db, "writing a tag");
		goto out;
	}

	ok = TRUE;

out:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	if (db != NULL)
		sqlite3_close(db);
	return ok;
}

/* One row's tag, as the reader needs it. */
typedef struct {
	gchar *label;
	gchar *model;
} Tag;

static void
tag_free(
	gpointer data
){
	Tag *tag = data;

	g_free(tag->label);
	g_free(tag->model);
	g_free(tag);
}

/*
 * The tags for rows recorded at or after @since, by row id.
 *
 * A file that is not there is no tags and is not created: opening one
 * to answer a question about spending would leave a file behind every
 * agent that has never run, exactly as the libreclaw database reader
 * refuses to.
 */
static GHashTable *
read_tags(
	const gchar *ledger_path,
	gint64       since,
	GError     **error
){
	g_autoptr(GHashTable) tags = NULL;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	gint rc;

	tags = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, tag_free);

	if (!g_file_test(ledger_path, G_FILE_TEST_EXISTS))
		return (GHashTable *)g_steal_pointer(&tags);

	if (sqlite3_open_v2(ledger_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "opening to read");
		sqlite3_close(db);
		return NULL;
	}

	sqlite3_busy_timeout(db, 2000);

	/* a file older than a column reads it as absent rather than failing */
	if (sqlite3_prepare_v2(db,
			       has_column(db, "model")
			       ? "SELECT usage_id, origin_kind, origin_id, model"
				 " FROM turn_origin WHERE recorded_at >= ?"
			       : "SELECT usage_id, origin_kind, origin_id, NULL"
				 " FROM turn_origin WHERE recorded_at >= ?",
			       -1, &stmt, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "preparing a read");
		sqlite3_close(db);
		return NULL;
	}

	sqlite3_bind_int64(stmt, 1, since);

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		gint64 *key = g_new(gint64, 1);
		Tag *tag = g_new0(Tag, 1);
		ClawtUsageOriginKind kind = clawt_usage_origin_kind_from_nick(
			(const gchar *)sqlite3_column_text(stmt, 1));

		*key = sqlite3_column_int64(stmt, 0);
		tag->label = clawt_usage_origin_label(
			kind, (const gchar *)sqlite3_column_text(stmt, 2));
		tag->model = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
		g_hash_table_replace(tags, key, tag);
	}

	if (rc != SQLITE_DONE) {
		set_sqlite_error(error, db, "reading");
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return NULL;
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);

	return (GHashTable *)g_steal_pointer(&tags);
}

/* ── Grouping ────────────────────────────────────────────────────── */

static const struct {
	const gchar *nick;
	const gchar *label;
} usage_groups[] = {
	{ "agent",  "Agent" },
	{ "model",  "Model" },
	{ "origin", "Origin" },
	{ "day",    "Day" }
};

guint
clawt_usage_group_count(void)
{
	return G_N_ELEMENTS(usage_groups);
}

const gchar *
clawt_usage_group_nth(
	guint n
){
	return usage_groups[MIN(n, G_N_ELEMENTS(usage_groups) - 1)].nick;
}

const gchar *
clawt_usage_group_label(
	guint n
){
	return usage_groups[MIN(n, G_N_ELEMENTS(usage_groups) - 1)].label;
}

gint
clawt_usage_group_index(
	const gchar *nick
){
	guint i;

	for (i = 0; nick != NULL && i < G_N_ELEMENTS(usage_groups); i++)
		if (g_strcmp0(usage_groups[i].nick, nick) == 0)
			return (gint)i;

	return -1;
}

/* One group's running figures. */
typedef struct {
	gchar  *key;
	gint64  turns;
	gint64  unpriced_turns;
	gint64  input_tokens;
	gint64  output_tokens;
	gint64  cost_micros;
} Group;

static void
group_free(
	gpointer data
){
	Group *group = data;

	g_free(group->key);
	g_free(group);
}

struct _ClawtUsageLedger {
	GObject     parent_instance;
	gchar      *by;
	GHashTable *groups;   /* key -> Group* */
	Group       total;
};

G_DEFINE_FINAL_TYPE(ClawtUsageLedger, clawt_usage_ledger, G_TYPE_OBJECT)

ClawtUsageLedger *
clawt_usage_ledger_new(
	const gchar *by
){
	ClawtUsageLedger *self;

	self = g_object_new(CLAWT_TYPE_USAGE_LEDGER, NULL);
	self->by = g_strdup(clawt_usage_group_index(by) >= 0 ? by : "agent");

	return self;
}

const gchar *
clawt_usage_ledger_get_by(
	ClawtUsageLedger *self
){
	g_return_val_if_fail(CLAWT_IS_USAGE_LEDGER(self), NULL);

	return self->by;
}

static void
count_into(
	Group  *group,
	gint64  input_tokens,
	gint64  output_tokens,
	gint64  cost_micros
){
	group->turns += 1;
	group->input_tokens += MAX(input_tokens, 0);
	group->output_tokens += MAX(output_tokens, 0);

	/* counted, and kept out of the sum: unknown is never free */
	if (clawt_usage_turn_is_priced(cost_micros, input_tokens, output_tokens))
		group->cost_micros += cost_micros;
	else
		group->unpriced_turns += 1;
}

void
clawt_usage_ledger_add_turn(
	ClawtUsageLedger *self,
	const gchar      *agent_id,
	gint64            recorded_at,
	const gchar      *model,
	const gchar      *origin_label,
	gint64            input_tokens,
	gint64            output_tokens,
	gint64            cost_micros
){
	g_autofree gchar *key = NULL;
	Group *group;

	g_return_if_fail(CLAWT_IS_USAGE_LEDGER(self));

	if (g_strcmp0(self->by, "model") == 0) {
		key = g_strdup(is_set(model) ? model : "(unknown)");
	} else if (g_strcmp0(self->by, "origin") == 0) {
		key = g_strdup(is_set(origin_label)
			       ? origin_label
			       : clawt_usage_origin_kind_nick(CLAWT_USAGE_ORIGIN_UNKNOWN));
	} else if (g_strcmp0(self->by, "day") == 0) {
		g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(recorded_at);

		/* the person's day, not UTC's: "Tuesday" means their Tuesday */
		key = (at != NULL) ? g_date_time_format(at, "%Y-%m-%d")
				   : g_strdup("(unknown)");
	} else {
		key = g_strdup(is_set(agent_id) ? agent_id : "(unknown)");
	}

	group = g_hash_table_lookup(self->groups, key);
	if (group == NULL) {
		group = g_new0(Group, 1);
		group->key = g_strdup(key);
		g_hash_table_insert(self->groups, group->key, group);
	}

	count_into(group, input_tokens, output_tokens, cost_micros);
	count_into(&self->total, input_tokens, output_tokens, cost_micros);
}

gboolean
clawt_usage_ledger_add_agent(
	ClawtUsageLedger *self,
	const gchar      *agent_id,
	const gchar      *state_dir,
	gint64            since,
	GError          **error
){
	g_autofree gchar *db_path = NULL;
	g_autofree gchar *ledger_path = NULL;
	g_autoptr(GHashTable) tags = NULL;
	g_autoptr(LcDatabase) db = NULL;
	GPtrArray *rows;
	guint i;

	g_return_val_if_fail(CLAWT_IS_USAGE_LEDGER(self), FALSE);
	g_return_val_if_fail(agent_id != NULL, FALSE);
	g_return_val_if_fail(state_dir != NULL, FALSE);

	db_path = clawt_usage_database_path(state_dir);
	ledger_path = clawt_usage_ledger_path_beside(db_path);

	/* opening creates the file: an agent that never ran has no history */
	if (!g_file_test(db_path, G_FILE_TEST_EXISTS))
		return TRUE;

	tags = read_tags(ledger_path, since, error);
	if (tags == NULL)
		return FALSE;

	db = LC_DATABASE(lc_sqlite_database_new());
	if (!lc_database_open(db, db_path, error))
		return FALSE;

	rows = lc_database_query_token_usage(db, NULL, NULL, since, 0, error);
	if (rows == NULL) {
		lc_database_close(db);
		return FALSE;
	}

	for (i = 0; i < rows->len; i++) {
		LcDbTokenUsage *row = g_ptr_array_index(rows, i);
		Tag *tag = g_hash_table_lookup(tags, &row->id);
		const gchar *model = row->model;

		if (!is_set(model) && tag != NULL)
			model = tag->model;

		clawt_usage_ledger_add_turn(self, agent_id, row->recorded_at, model,
					    tag != NULL ? tag->label : NULL,
					    row->input_tokens, row->output_tokens,
					    row->cost_usd_micros);
	}

	g_ptr_array_unref(rows);
	lc_database_close(db);

	return TRUE;
}

static gint
group_by_cost(
	gconstpointer a,
	gconstpointer b
){
	const Group *x = *(const Group *const *)a;
	const Group *y = *(const Group *const *)b;

	if (x->cost_micros != y->cost_micros)
		return x->cost_micros > y->cost_micros ? -1 : 1;
	/* an all-unpriced group is not cheap, it is unknown: keep it in view */
	if (x->unpriced_turns != y->unpriced_turns)
		return x->unpriced_turns > y->unpriced_turns ? -1 : 1;
	return g_strcmp0(x->key, y->key);
}

static gint
group_by_key(
	gconstpointer a,
	gconstpointer b
){
	const Group *x = *(const Group *const *)a;
	const Group *y = *(const Group *const *)b;

	return g_strcmp0(x->key, y->key);
}

static void
add_figures(
	JsonBuilder *builder,
	const Group *group
){
	json_builder_set_member_name(builder, "turns");
	json_builder_add_int_value(builder, group->turns);
	json_builder_set_member_name(builder, "unpriced_turns");
	json_builder_add_int_value(builder, group->unpriced_turns);
	json_builder_set_member_name(builder, "input_tokens");
	json_builder_add_int_value(builder, group->input_tokens);
	json_builder_set_member_name(builder, "output_tokens");
	json_builder_add_int_value(builder, group->output_tokens);
	json_builder_set_member_name(builder, "cost_micros");
	json_builder_add_int_value(builder, group->cost_micros);
}

JsonNode *
clawt_usage_ledger_to_json(
	ClawtUsageLedger *self
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) order = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_return_val_if_fail(CLAWT_IS_USAGE_LEDGER(self), NULL);

	g_hash_table_iter_init(&iter, self->groups);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(order, value);

	/* a calendar reads oldest first; everything else, most spent first */
	g_ptr_array_sort(order, g_strcmp0(self->by, "day") == 0 ? group_by_key
							       : group_by_cost);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "by");
	json_builder_add_string_value(builder, self->by);
	json_builder_set_member_name(builder, "groups");
	json_builder_begin_array(builder);

	for (i = 0; i < order->len; i++) {
		const Group *group = g_ptr_array_index(order, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, group->key);
		add_figures(builder, group);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "total");
	json_builder_begin_object(builder);
	add_figures(builder, &self->total);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static void
clawt_usage_ledger_finalize(
	GObject *object
){
	ClawtUsageLedger *self = CLAWT_USAGE_LEDGER(object);

	g_clear_pointer(&self->by, g_free);
	g_clear_pointer(&self->groups, g_hash_table_unref);

	G_OBJECT_CLASS(clawt_usage_ledger_parent_class)->finalize(object);
}

static void
clawt_usage_ledger_class_init(
	ClawtUsageLedgerClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_usage_ledger_finalize;
}

static void
clawt_usage_ledger_init(
	ClawtUsageLedger *self
){
	/* the Group owns its key; the table borrows it */
	self->groups = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					     group_free);
}

/* ── The reply, as every client reads it ─────────────────────────── */

JsonNode *
clawt_usage_ledger_payload(
	gint64       since,
	const gchar *by,
	const gchar *agent
){
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "since");
	json_builder_add_int_value(builder, since);
	json_builder_set_member_name(builder, "by");
	json_builder_add_string_value(builder,
				      clawt_usage_group_index(by) >= 0 ? by : "agent");
	if (is_set(agent)) {
		json_builder_set_member_name(builder, "agent");
		json_builder_add_string_value(builder, agent);
	}
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* A member that is an array, or NULL -- a type check is not a pointer check */
static JsonArray *
member_array(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node;

	if (object == NULL || !json_object_has_member(object, name))
		return NULL;
	node = json_object_get_member(object, name);
	return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

static JsonObject *
member_object(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node;

	if (object == NULL || !json_object_has_member(object, name))
		return NULL;
	node = json_object_get_member(object, name);
	return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

gchar *
clawt_usage_ledger_cost_label(
	JsonObject *group
){
	gint64 turns = clawt_ipc_payload_int(group, "turns", 0);
	gint64 unpriced = clawt_ipc_payload_int(group, "unpriced_turns", 0);

	if (unpriced > 0 && unpriced >= turns)
		return g_strdup("unpriced");

	return clawt_usage_format_cost(clawt_ipc_payload_int(group, "cost_micros", 0));
}

/* 4210 -> "4,210": a column of token counts is read, not parsed */
static gchar *
grouped_digits(
	gint64 value
){
	g_autofree gchar *plain = g_strdup_printf("%" G_GINT64_FORMAT, ABS(value));
	GString *out = g_string_new(value < 0 ? "-" : "");
	gsize len = strlen(plain);
	gsize i;

	for (i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, plain[i]);
	}

	return g_string_free(out, FALSE);
}

gchar *
clawt_usage_ledger_row_subtitle(
	JsonObject *group
){
	gint64 turns = clawt_ipc_payload_int(group, "turns", 0);
	gint64 unpriced = clawt_ipc_payload_int(group, "unpriced_turns", 0);
	g_autofree gchar *out_tokens = grouped_digits(
		clawt_ipc_payload_int(group, "output_tokens", 0));
	GString *out = g_string_new(NULL);

	g_string_append_printf(out, "%" G_GINT64_FORMAT " turn%s", turns,
			       turns == 1 ? "" : "s");
	if (unpriced > 0)
		g_string_append_printf(out, " \xc2\xb7 %" G_GINT64_FORMAT " unpriced",
				       unpriced);
	g_string_append_printf(out, " \xc2\xb7 %s tokens out", out_tokens);

	return g_string_free(out, FALSE);
}

gchar *
clawt_usage_ledger_unpriced_line(
	JsonObject *reply
){
	gint64 unpriced = clawt_ipc_payload_int(member_object(reply, "total"),
						"unpriced_turns", 0);

	if (unpriced <= 0)
		return NULL;

	return g_strdup_printf("%" G_GINT64_FORMAT " turn%s no recorded price and "
			       "%s not in the total -- unknown, not free",
			       unpriced, unpriced == 1 ? " has" : "s have",
			       unpriced == 1 ? "is" : "are");
}

gchar *
clawt_usage_csv_cell(
	const gchar *value
){
	GString *cell;
	gboolean quote;
	const gchar *p;
	gsize len;

	if (value == NULL)
		value = "";

	cell = g_string_new(NULL);

	/*
	 * A spreadsheet evaluates a cell that opens like a formula, and a
	 * model name, a routine id or a channel is text somebody else chose.
	 * The apostrophe is what every spreadsheet reads as "this is text".
	 */
	if (strchr("=+-@\t\r", value[0]) != NULL && value[0] != '\0')
		g_string_append_c(cell, '\'');
	g_string_append(cell, value);

	len = strlen(value);
	quote = strpbrk(value, ",\"\r\n") != NULL ||
		(len > 0 && (value[0] == ' ' || value[len - 1] == ' '));

	if (!quote)
		return g_string_free(cell, FALSE);

	{
		GString *quoted = g_string_new("\"");

		for (p = cell->str; *p != '\0'; p++) {
			if (*p == '"')
				g_string_append_c(quoted, '"');
			g_string_append_c(quoted, *p);
		}
		g_string_append_c(quoted, '"');
		g_string_free(cell, TRUE);

		return g_string_free(quoted, FALSE);
	}
}

/* micro-dollars as a plain decimal, without a float's rounding */
static gchar *
dollars_exact(
	gint64 micros
){
	gint64 whole = ABS(micros) / 1000000;
	gint64 frac = ABS(micros) % 1000000;

	return g_strdup_printf("%s%" G_GINT64_FORMAT ".%06" G_GINT64_FORMAT,
			       micros < 0 ? "-" : "", whole, frac);
}

gchar *
clawt_usage_ledger_csv(
	JsonObject *reply
){
	JsonArray *groups = member_array(reply, "groups");
	const gchar *by = clawt_ipc_payload_string(reply, "by");
	GString *out = g_string_new(NULL);
	g_autofree gchar *head = clawt_usage_csv_cell(
		clawt_usage_group_index(by) >= 0 ? by : "agent");
	guint i;

	g_string_append_printf(out, "%s,turns,unpriced_turns,input_tokens,"
			       "output_tokens,cost_usd\r\n", head);

	for (i = 0; groups != NULL && i < json_array_get_length(groups); i++) {
		JsonObject *group = json_array_get_object_element(groups, i);
		g_autofree gchar *key = clawt_usage_csv_cell(
			clawt_ipc_payload_string(group, "key"));
		gint64 turns = clawt_ipc_payload_int(group, "turns", 0);
		gint64 unpriced = clawt_ipc_payload_int(group, "unpriced_turns", 0);
		g_autofree gchar *cost = NULL;

		/* empty, not 0.000000: nobody knows what these turns cost */
		cost = (unpriced > 0 && unpriced >= turns)
			? g_strdup("")
			: dollars_exact(clawt_ipc_payload_int(group, "cost_micros", 0));

		g_string_append_printf(out,
				       "%s,%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
				       ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
				       ",%s\r\n",
				       key, turns, unpriced,
				       clawt_ipc_payload_int(group, "input_tokens", 0),
				       clawt_ipc_payload_int(group, "output_tokens", 0),
				       cost);
	}

	return g_string_free(out, FALSE);
}

gchar *
clawt_usage_ledger_csv_name(
	JsonObject *reply,
	gint64      now
){
	const gchar *by = clawt_ipc_payload_string(reply, "by");
	g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(now);
	g_autofree gchar *date = (at != NULL) ? g_date_time_format(at, "%Y-%m-%d")
					      : g_strdup("export");

	/* the nick from the table, never the reply's text, into a file name */
	return g_strdup_printf("clawtilla-spending-by-%s-%s.csv",
			       clawt_usage_group_nth((guint)MAX(
				       clawt_usage_group_index(by), 0)),
			       date);
}
