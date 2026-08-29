/*
 * clawt-handoff-store.c - The handoff queue, and what became of each one
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <sqlite3.h>

#include "clawtilla.h"
#include "task/clawt-handoff-store.h"

/*
 * How two rows made in the same second are ordered.
 *
 * `rowid` is sqlite's own insertion order, which is what "the order they
 * happened" means -- and the alternative that suggests itself, the id,
 * is random, so a tie broke arbitrarily and a task's ownership history
 * could come back saying the wrong agent had it last.  Timestamps here
 * are whole seconds, so ties are ordinary rather than exotic.
 */
#define ORDER_BY_WHEN " ORDER BY created_at ASC, rowid ASC"

/*
 * The columns, in the order every read below expects them.
 *
 * One spelling for both statements: an unqualified `SELECT *` would
 * change meaning the moment a migration adds a column, and a column list
 * written twice differs exactly once.
 */
#define SELECT_COLUMNS \
    "id, task_id, from_agent, to_agent, reason, room, verdict, " \
    "state, attempts, depth, created_at, settled_at"

static const gchar *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS handoffs ("
    "  id TEXT PRIMARY KEY,"
    "  task_id TEXT NOT NULL,"
    "  from_agent TEXT,"
    "  to_agent TEXT NOT NULL,"
    "  reason TEXT,"
    "  room TEXT,"
    "  verdict TEXT,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  attempts INTEGER NOT NULL DEFAULT 0,"
    "  depth INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  settled_at INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS handoffs_queue"
    "  ON handoffs(state, from_agent, created_at);"
    "CREATE INDEX IF NOT EXISTS handoffs_task"
    "  ON handoffs(task_id, created_at);";

struct _ClawtHandoffStore {
    GObject  parent_instance;
    sqlite3 *db;
    gchar   *path;
};

G_DEFINE_FINAL_TYPE(ClawtHandoffStore, clawt_handoff_store, G_TYPE_OBJECT)

static gint64
now_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static void
set_sqlite_error(GError **error, sqlite3 *db, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                "%s: %s", what, sqlite3_errmsg(db));
}

/*
 * Whether a column is already on a table.
 *
 * Asked rather than inferred from a failed ALTER, exactly as
 * clawt-mailbox.c does and for the reason recorded there: "add it and
 * ignore the error" cannot tell a column that is already present from a
 * database that cannot be written, and only one of those is survivable.
 *
 * There are no migrations here yet.  The helper is in place from the
 * first commit because the alternative -- a bare CREATE TABLE IF NOT
 * EXISTS and a column added later -- is the defect that quarantined
 * every mailbox in a live fleet as corrupt, and it is cheaper to have
 * the shape right than to remember the lesson twice.
 */
static gboolean
has_column(sqlite3 *db, const gchar *table, const gchar *column)
{
    g_autofree gchar *sql = NULL;
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    /*
     * PRAGMA takes no parameters, so the table name is spliced in.  It
     * is a literal at every call site and never comes from a config file
     * or a message.
     */
    sql = g_strdup_printf("PRAGMA table_info(%s)", table);

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
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
 * The schema, and the migrations that bring an older file up to it.
 *
 * One function, so a second open path added later cannot skip a
 * migration -- which is how a migration ends up running only when a file
 * happens to be recreated.
 */
static gboolean
apply_schema(sqlite3 *db, GError **error)
{
    if (db == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the handoff store is not open");
        return FALSE;
    }

    if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, db, "creating the handoff schema");
        return FALSE;
    }

    /*
     * Since 0.2.0.  `verdict` is what an agent actually reads, so a file
     * written by a build that predates it would answer every status
     * question with a blank rather than a sentence.
     */
    if (!has_column(db, "handoffs", "verdict") &&
        sqlite3_exec(db, "ALTER TABLE handoffs ADD COLUMN verdict TEXT",
                     NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, db, "adding verdict to the handoff store");
        return FALSE;
    }

    return TRUE;
}

static void
clawt_handoff_store_dispose(GObject *object)
{
    ClawtHandoffStore *self = CLAWT_HANDOFF_STORE(object);

    /*
     * _v2, not the plain form: sqlite3_close() refuses with SQLITE_BUSY
     * while anything is outstanding and leaves the connection allocated.
     */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    g_clear_pointer(&self->path, g_free);

    G_OBJECT_CLASS(clawt_handoff_store_parent_class)->dispose(object);
}

static void
clawt_handoff_store_class_init(ClawtHandoffStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_handoff_store_dispose;
}

static void
clawt_handoff_store_init(ClawtHandoffStore *self)
{
    (void)self;
}

ClawtHandoffStore *
clawt_handoff_store_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtHandoffStore) self = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_HANDOFF_STORE, NULL);
    self->path = g_strdup(path);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open the handoff store: %s",
                    sqlite3_errmsg(self->db));

        /*
         * sqlite3_open leaves a usable handle even when it fails, and
         * dispose is what closes it -- returning NULL here without this
         * comment reads as a leak the next time somebody audits it.
         */
        return NULL;
    }

    if (!apply_schema(self->db, error))
        return NULL;

    return g_steal_pointer(&self);
}

/* ── Row <-> record ──────────────────────────────────────────────── */

static const gchar *
column_text(sqlite3_stmt *stmt, gint column)
{
    return (const gchar *)sqlite3_column_text(stmt, column);
}

static ClawtHandoff *
handoff_from_row(sqlite3_stmt *stmt)
{
    ClawtHandoff *handoff;

    handoff = clawt_handoff_new(column_text(stmt, 1),
                                column_text(stmt, 2),
                                column_text(stmt, 3),
                                column_text(stmt, 4));

    clawt_handoff_set_id(handoff, column_text(stmt, 0));
    clawt_handoff_set_room(handoff, column_text(stmt, 5));
    clawt_handoff_set_verdict(handoff, column_text(stmt, 6));

    /*
     * The state is written before the stamps, because
     * clawt_handoff_set_state() stamps a settle time of its own for a
     * terminal state -- and then the stored one overwrites it, which is
     * what makes a receipt read back with the time it really settled
     * rather than the time it was read.
     */
    clawt_handoff_set_state(handoff,
        (ClawtHandoffState)sqlite3_column_int(stmt, 7));
    clawt_handoff_set_attempts(handoff,
        (guint)sqlite3_column_int(stmt, 8));
    clawt_handoff_set_depth(handoff, sqlite3_column_int(stmt, 9));
    clawt_handoff_set_created_at(handoff, sqlite3_column_int64(stmt, 10));
    clawt_handoff_set_settled_at(handoff, sqlite3_column_int64(stmt, 11));

    return handoff;
}

static void
bind_text_or_null(sqlite3_stmt *stmt, gint index, const gchar *value)
{
    if (value != NULL)
        sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, index);
}

/* ── Writing ─────────────────────────────────────────────────────── */

gboolean
clawt_handoff_store_queue(ClawtHandoffStore *self,
                          ClawtHandoff      *handoff,
                          GError           **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_HANDOFF_STORE(self), FALSE);
    g_return_val_if_fail(handoff != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
                           "INSERT INTO handoffs ("
                           "  id, task_id, from_agent, to_agent, reason,"
                           "  room, verdict, state, attempts, depth,"
                           "  created_at, settled_at)"
                           " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "queuing a handoff");
        return FALSE;
    }

    bind_text_or_null(stmt, 1, clawt_handoff_get_id(handoff));
    bind_text_or_null(stmt, 2, clawt_handoff_get_task_id(handoff));
    bind_text_or_null(stmt, 3, clawt_handoff_get_from_agent(handoff));
    bind_text_or_null(stmt, 4, clawt_handoff_get_to_agent(handoff));
    bind_text_or_null(stmt, 5, clawt_handoff_get_reason(handoff));
    bind_text_or_null(stmt, 6, clawt_handoff_get_room(handoff));
    bind_text_or_null(stmt, 7, clawt_handoff_get_verdict(handoff));
    sqlite3_bind_int(stmt, 8, (gint)clawt_handoff_get_state(handoff));
    sqlite3_bind_int(stmt, 9, (gint)clawt_handoff_get_attempts(handoff));
    sqlite3_bind_int(stmt, 10, clawt_handoff_get_depth(handoff));
    sqlite3_bind_int64(stmt, 11, clawt_handoff_get_created_at(handoff));
    sqlite3_bind_int64(stmt, 12, clawt_handoff_get_settled_at(handoff));

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, "queuing a handoff");

    sqlite3_finalize(stmt);
    return ok;
}

gboolean
clawt_handoff_store_update(ClawtHandoffStore *self,
                           ClawtHandoff      *handoff,
                           GError           **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_HANDOFF_STORE(self), FALSE);
    g_return_val_if_fail(handoff != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
                           "UPDATE handoffs SET state = ?, attempts = ?,"
                           " verdict = ?, room = ?, settled_at = ?"
                           " WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "updating a handoff");
        return FALSE;
    }

    sqlite3_bind_int(stmt, 1, (gint)clawt_handoff_get_state(handoff));
    sqlite3_bind_int(stmt, 2, (gint)clawt_handoff_get_attempts(handoff));
    bind_text_or_null(stmt, 3, clawt_handoff_get_verdict(handoff));
    bind_text_or_null(stmt, 4, clawt_handoff_get_room(handoff));
    sqlite3_bind_int64(stmt, 5, clawt_handoff_get_settled_at(handoff));
    bind_text_or_null(stmt, 6, clawt_handoff_get_id(handoff));

    ok = sqlite3_step(stmt) == SQLITE_DONE &&
         sqlite3_changes(self->db) > 0;

    if (!ok)
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no handoff %s to update",
                    clawt_handoff_get_id(handoff) != NULL
                        ? clawt_handoff_get_id(handoff) : "");

    sqlite3_finalize(stmt);
    return ok;
}

/* ── Reading ─────────────────────────────────────────────────────── */

ClawtHandoff *
clawt_handoff_store_get(ClawtHandoffStore *self, const gchar *id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtHandoff *handoff = NULL;

    g_return_val_if_fail(CLAWT_IS_HANDOFF_STORE(self), NULL);

    if (id == NULL)
        return NULL;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT " SELECT_COLUMNS
                           " FROM handoffs WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        handoff = handoff_from_row(stmt);

    sqlite3_finalize(stmt);
    return handoff;
}

GPtrArray *
clawt_handoff_store_queued_from(ClawtHandoffStore *self,
                                const gchar       *from_agent)
{
    GPtrArray *out;
    sqlite3_stmt *stmt = NULL;
    const gchar *sql;

    g_return_val_if_fail(CLAWT_IS_HANDOFF_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_handoff_free);

    sql = (from_agent != NULL)
        ? "SELECT " SELECT_COLUMNS " FROM handoffs"
          " WHERE state = 0 AND from_agent = ?" ORDER_BY_WHEN
        : "SELECT " SELECT_COLUMNS " FROM handoffs"
          " WHERE state = 0" ORDER_BY_WHEN;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return out;

    if (from_agent != NULL)
        sqlite3_bind_text(stmt, 1, from_agent, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, handoff_from_row(stmt));

    sqlite3_finalize(stmt);
    return out;
}

guint
clawt_handoff_store_count_queued(ClawtHandoffStore *self,
                                 const gchar       *from_agent)
{
    sqlite3_stmt *stmt = NULL;
    const gchar *sql;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_HANDOFF_STORE(self), 0);

    sql = (from_agent != NULL)
        ? "SELECT COUNT(*) FROM handoffs WHERE state = 0 AND from_agent = ?"
        : "SELECT COUNT(*) FROM handoffs WHERE state = 0";

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    if (from_agent != NULL)
        sqlite3_bind_text(stmt, 1, from_agent, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    return count;
}

GPtrArray *
clawt_handoff_store_for_task(ClawtHandoffStore *self, const gchar *task_id)
{
    GPtrArray *out;
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_HANDOFF_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_handoff_free);

    if (task_id == NULL)
        return out;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT " SELECT_COLUMNS " FROM handoffs"
                           " WHERE task_id = ?" ORDER_BY_WHEN,
                           -1, &stmt, NULL) != SQLITE_OK)
        return out;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, handoff_from_row(stmt));

    sqlite3_finalize(stmt);
    return out;
}

/* ── Retention ───────────────────────────────────────────────────── */

guint
clawt_handoff_store_prune(ClawtHandoffStore *self, gint64 keep_seconds)
{
    sqlite3_stmt *stmt = NULL;
    guint removed = 0;

    g_return_val_if_fail(CLAWT_IS_HANDOFF_STORE(self), 0);

    /*
     * `state != 0` on both statements is the whole safety of this
     * function: a queued handoff is not old, it is undrained, and
     * deleting one would throw away work an agent has already been told
     * is on its way.
     */
    if (keep_seconds > 0 &&
        sqlite3_prepare_v2(self->db,
                           "DELETE FROM handoffs"
                           " WHERE state != 0 AND settled_at > 0"
                           "   AND settled_at < ?",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now_seconds() - keep_seconds);

        if (sqlite3_step(stmt) == SQLITE_DONE)
            removed += (guint)sqlite3_changes(self->db);

        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    if (sqlite3_prepare_v2(self->db,
                           "DELETE FROM handoffs WHERE id IN ("
                           "  SELECT id FROM handoffs WHERE state != 0"
                           "   ORDER BY settled_at DESC, created_at DESC"
                           "   LIMIT -1 OFFSET ?)",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, CLAWT_HANDOFF_STORE_MAX_RECEIPTS);

        if (sqlite3_step(stmt) == SQLITE_DONE)
            removed += (guint)sqlite3_changes(self->db);

        sqlite3_finalize(stmt);
    }

    return removed;
}
