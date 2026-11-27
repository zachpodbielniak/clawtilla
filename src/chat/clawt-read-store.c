/*
 * clawt-read-store.c - How far the operator has read, per conversation
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <sqlite3.h>

#include "clawtilla.h"
#include "chat/clawt-read-store.h"

/*
 * One row per room.  `updated_at` is not read by anything yet; it is
 * there so a question like "when did I last open this" can be answered
 * later without a migration, and it costs one integer per conversation.
 */
static const gchar *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS read_marks ("
    "  room TEXT PRIMARY KEY,"
    "  last_id TEXT,"
    "  last_ts INTEGER NOT NULL DEFAULT 0,"
    "  updated_at INTEGER NOT NULL DEFAULT 0"
    ");";

struct _ClawtReadStore {
    GObject   parent_instance;
    sqlite3  *db;
    gchar    *path;
    gboolean  created;
};

G_DEFINE_FINAL_TYPE(ClawtReadStore, clawt_read_store, G_TYPE_OBJECT)

static void
set_sqlite_error(GError **error, sqlite3 *db, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                "%s: %s", what, sqlite3_errmsg(db));
}

static void
clawt_read_store_dispose(GObject *object)
{
    ClawtReadStore *self = CLAWT_READ_STORE(object);

    /*
     * _v2, as the handoff store does: the plain close refuses with
     * SQLITE_BUSY while a statement is outstanding and leaves the
     * connection allocated.
     */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    g_clear_pointer(&self->path, g_free);

    G_OBJECT_CLASS(clawt_read_store_parent_class)->dispose(object);
}

static void
clawt_read_store_class_init(ClawtReadStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_read_store_dispose;
}

static void
clawt_read_store_init(ClawtReadStore *self)
{
    (void)self;
}

ClawtReadStore *
clawt_read_store_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtReadStore) self = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_READ_STORE, NULL);
    self->path = g_strdup(path);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    /*
     * Asked before sqlite3_open(), which creates the file -- afterwards
     * every open looks like one that found it.
     */
    self->created = !g_file_test(path, G_FILE_TEST_EXISTS);

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open the read store: %s",
                    sqlite3_errmsg(self->db));

        /* sqlite3_open leaves a handle even on failure; dispose closes it. */
        return NULL;
    }

    /*
     * Written on the daemon's main loop, so a write must not wait on the
     * disk: WAL with synchronous=NORMAL appends a commit to the log and
     * syncs only at a checkpoint, as the task store and the mailbox
     * already do.  At SQLite's defaults every write fsync()ed a rollback
     * journal and the database, and on a saturated disk one fsync held
     * the loop -- and the control socket's accept -- for tens of seconds
     * (tests/test-store-sync.c).  A power cut can lose the last commits;
     * it cannot corrupt the file.
     */
    sqlite3_exec(self->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(self->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    if (sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "creating the read schema");
        return NULL;
    }

    return g_steal_pointer(&self);
}

gboolean
clawt_read_store_was_created(ClawtReadStore *self)
{
    g_return_val_if_fail(CLAWT_IS_READ_STORE(self), FALSE);

    return self->created;
}

gboolean
clawt_read_store_get(ClawtReadStore *self,
                     const gchar    *room_id,
                     gchar         **out_last_id,
                     gint64         *out_last_ts)
{
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    if (out_last_id != NULL)
        *out_last_id = NULL;

    if (out_last_ts != NULL)
        *out_last_ts = 0;

    g_return_val_if_fail(CLAWT_IS_READ_STORE(self), FALSE);

    if (room_id == NULL || self->db == NULL)
        return FALSE;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT last_id, last_ts FROM read_marks"
                           " WHERE room = ?1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_text(stmt, 1, room_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = TRUE;

        if (out_last_id != NULL &&
            sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            *out_last_id = g_strdup(
                (const gchar *)sqlite3_column_text(stmt, 0));

        if (out_last_ts != NULL)
            *out_last_ts = (gint64)sqlite3_column_int64(stmt, 1);
    }

    sqlite3_finalize(stmt);

    return found;
}

gboolean
clawt_read_store_set(ClawtReadStore *self,
                     const gchar    *room_id,
                     const gchar    *last_id,
                     gint64          last_ts,
                     GError        **error)
{
    sqlite3_stmt *stmt = NULL;
    gint rc;

    g_return_val_if_fail(CLAWT_IS_READ_STORE(self), FALSE);

    if (room_id == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "which room has been read?");
        return FALSE;
    }

    if (sqlite3_prepare_v2(self->db,
                           "INSERT INTO read_marks"
                           " (room, last_id, last_ts, updated_at)"
                           " VALUES (?1, ?2, ?3, ?4)"
                           " ON CONFLICT(room) DO UPDATE SET"
                           "  last_id = excluded.last_id,"
                           "  last_ts = excluded.last_ts,"
                           "  updated_at = excluded.updated_at",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing a read mark");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, room_id, -1, SQLITE_TRANSIENT);

    if (last_id != NULL)
        sqlite3_bind_text(stmt, 2, last_id, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 2);

    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)last_ts);
    sqlite3_bind_int64(stmt, 4,
                       (sqlite3_int64)(g_get_real_time() / G_USEC_PER_SEC));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "writing a read mark");
        return FALSE;
    }

    return TRUE;
}

guint
clawt_read_store_count(ClawtReadStore *self)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_READ_STORE(self), 0);

    if (self->db == NULL)
        return 0;

    if (sqlite3_prepare_v2(self->db, "SELECT COUNT(*) FROM read_marks", -1,
                           &stmt, NULL) != SQLITE_OK)
        return 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);

    return count;
}
