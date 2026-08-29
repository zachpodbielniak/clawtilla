/*
 * clawt-memory-store.c - Where an agent's memories live
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "memory/clawt-memory-store.h"

#include <sqlite3.h>
#include <string.h>

struct _ClawtMemoryStore {
    GObject parent_instance;

    sqlite3 *db;
    gchar   *path;

    /* Whether this sqlite has FTS5. Not every build does. */
    gboolean full_text;
};

G_DEFINE_FINAL_TYPE(ClawtMemoryStore, clawt_memory_store, G_TYPE_OBJECT)

#define DEFAULT_LIMIT 20

/*
 * WAL for the same reason the mailbox uses it: the daemon writes while a
 * client may be reading the same file to show a person what an agent
 * knows.
 */
static const gchar SCHEMA[] =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS memories ("
    "  id           TEXT PRIMARY KEY,"
    "  content      TEXT NOT NULL,"
    "  summary      TEXT,"
    "  category     TEXT NOT NULL DEFAULT 'general',"
    "  importance   TEXT NOT NULL DEFAULT 'normal',"
    "  tags         TEXT,"
    "  source       TEXT,"
    "  pinned       INTEGER NOT NULL DEFAULT 0,"
    "  archived     INTEGER NOT NULL DEFAULT 0,"
    "  created_at   INTEGER NOT NULL,"
    "  updated_at   INTEGER NOT NULL,"
    "  accessed_at  INTEGER NOT NULL DEFAULT 0,"
    "  access_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS memories_recent"
    "  ON memories(archived, pinned DESC, created_at DESC);"
    "CREATE INDEX IF NOT EXISTS memories_category"
    "  ON memories(category, archived);";

/*
 * A separate, non-external FTS5 table rather than an external-content
 * one.
 *
 * External content saves the duplicate text but ties the index to
 * memories' implicit rowid, and a store that has to survive being
 * copied, restored from a backup or repaired is better off with an
 * index that stands on its own.
 */
static const gchar FTS_SCHEMA[] =
    "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts"
    "  USING fts5(id UNINDEXED, body);";

static gboolean
fail(ClawtMemoryStore *self, GError **error, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s", what,
                self->db != NULL ? sqlite3_errmsg(self->db) : "no database");
    return FALSE;
}

/* The text FTS5 indexes: everything a person might search by. */
static gchar *
searchable(ClawtMemory *memory)
{
    return g_strjoin(" ",
                     memory->content != NULL ? memory->content : "",
                     memory->summary != NULL ? memory->summary : "",
                     memory->tags != NULL ? memory->tags : "",
                     memory->category != NULL ? memory->category : "",
                     NULL);
}

static const gchar *
column_text(sqlite3_stmt *stmt, gint column)
{
    return (const gchar *)sqlite3_column_text(stmt, column);
}

/*
 * The same columns in the same order, once bare for an INSERT and once
 * qualified for a SELECT.
 *
 * The qualified form is not optional: search joins memories_fts, which
 * also has an id, and an unqualified list makes every such query fail
 * with "ambiguous column name" -- silently, because a search reports no
 * matches rather than an error.
 */
#define INSERT_COLUMNS \
    "id, content, summary, category, importance, tags, source, " \
    "pinned, archived, created_at, updated_at, accessed_at, access_count"

#define SELECT_COLUMNS \
    "m.id, m.content, m.summary, m.category, m.importance, m.tags, " \
    "m.source, m.pinned, m.archived, m.created_at, m.updated_at, " \
    "m.accessed_at, m.access_count"

static ClawtMemory *
memory_from_row(sqlite3_stmt *stmt)
{
    ClawtMemory *memory = g_new0(ClawtMemory, 1);

    memory->id = g_strdup(column_text(stmt, 0));
    memory->content = g_strdup(column_text(stmt, 1));
    memory->summary = g_strdup(column_text(stmt, 2));
    memory->category = g_strdup(column_text(stmt, 3));
    memory->importance = g_strdup(column_text(stmt, 4));
    memory->tags = g_strdup(column_text(stmt, 5));
    memory->source = g_strdup(column_text(stmt, 6));
    memory->pinned = sqlite3_column_int(stmt, 7) != 0;
    memory->archived = sqlite3_column_int(stmt, 8) != 0;
    memory->created_at = sqlite3_column_int64(stmt, 9);
    memory->updated_at = sqlite3_column_int64(stmt, 10);
    memory->accessed_at = sqlite3_column_int64(stmt, 11);
    memory->access_count = sqlite3_column_int(stmt, 12);

    return memory;
}

/*
 * Records that a set of memories was read back.
 *
 * Done in one statement after the rows are collected rather than per row
 * inside the read: sqlite will not let a SELECT's statement be stepped
 * while the same connection writes the table underneath it.
 */
static void
mark_accessed(ClawtMemoryStore *self, GPtrArray *memories)
{
    sqlite3_stmt *stmt = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    if (memories->len == 0)
        return;

    if (sqlite3_prepare_v2(self->db,
                           "UPDATE memories SET accessed_at = ?, "
                           "access_count = access_count + 1 WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return;

    for (i = 0; i < memories->len; i++) {
        ClawtMemory *memory = g_ptr_array_index(memories, i);

        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, memory->id, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
}

ClawtMemoryStore *
clawt_memory_store_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtMemoryStore) self = NULL;
    g_autofree gchar *dir = NULL;
    gchar *message = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_MEMORY_STORE, NULL);
    self->path = g_strdup(path);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        fail(self, error, "could not open the memory store");
        return NULL;
    }

    /*
     * The connection is usable even when open fails -- that is how
     * sqlite3_errmsg() works on it -- so it is closed by dispose either
     * way rather than stranded.
     */
    if (sqlite3_exec(self->db, SCHEMA, NULL, NULL, &message) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create the memory schema: %s",
                    message != NULL ? message : "unknown");
        sqlite3_free(message);
        return NULL;
    }

    /*
     * FTS5 is optional at the sqlite build level, and its absence is a
     * reason to rank worse, not to refuse to remember anything.
     */
    self->full_text = sqlite3_exec(self->db, FTS_SCHEMA, NULL, NULL,
                                    &message) == SQLITE_OK;

    if (!self->full_text) {
        g_info("memory: no FTS5 in this sqlite (%s); "
               "search will use a substring match",
               message != NULL ? message : "unknown");
        sqlite3_free(message);
    }

    return g_steal_pointer(&self);
}

gchar *
clawt_memory_store_add(ClawtMemoryStore *self, ClawtMemory *memory,
                       GError **error)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *generated = NULL;
    const gchar *id;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), NULL);
    g_return_val_if_fail(memory != NULL, NULL);

    if (memory->content == NULL || memory->content[0] == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a memory with no content is not a memory");
        return NULL;
    }

    if (memory->id == NULL)
        memory->id = clawt_generate_id("mem");

    id = memory->id;

    if (memory->created_at == 0)
        memory->created_at = g_get_real_time() / G_USEC_PER_SEC;

    memory->updated_at = g_get_real_time() / G_USEC_PER_SEC;

    if (sqlite3_prepare_v2(self->db,
                           "INSERT OR REPLACE INTO memories ("
                           INSERT_COLUMNS ") VALUES "
                           "(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        fail(self, error, "could not prepare the insert");
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, memory->content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, memory->summary, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4,
                      memory->category != NULL ? memory->category : "general",
                      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5,
                      memory->importance != NULL ? memory->importance
                                                 : "normal",
                      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, memory->tags, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, memory->source, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 8, memory->pinned ? 1 : 0);
    sqlite3_bind_int(stmt, 9, memory->archived ? 1 : 0);
    sqlite3_bind_int64(stmt, 10, memory->created_at);
    sqlite3_bind_int64(stmt, 11, memory->updated_at);
    sqlite3_bind_int64(stmt, 12, memory->accessed_at);
    sqlite3_bind_int(stmt, 13, memory->access_count);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fail(self, error, "could not write the memory");
        sqlite3_finalize(stmt);
        return NULL;
    }

    sqlite3_finalize(stmt);

    if (self->full_text) {
        g_autofree gchar *body = searchable(memory);

        /* Replaced rather than appended, so an updated memory is not
         * indexed twice under the same id. */
        if (sqlite3_prepare_v2(self->db,
                               "DELETE FROM memories_fts WHERE id = ?",
                               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        if (sqlite3_prepare_v2(self->db,
                               "INSERT INTO memories_fts (id, body) "
                               "VALUES (?, ?)", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, body, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return g_strdup(id);
}

GPtrArray *
clawt_memory_store_search(ClawtMemoryStore *self, const gchar *query,
                          const gchar *category, guint limit,
                          GError **error)
{
    g_autoptr(GPtrArray) out = NULL;
    g_autofree gchar *pattern = NULL;
    g_autoptr(GString) sql = NULL;
    sqlite3_stmt *stmt = NULL;
    gint bind = 1;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_memory_free);

    if (limit == 0)
        limit = DEFAULT_LIMIT;

    if (query == NULL || query[0] == '\0')
        return clawt_memory_store_list(self, category, FALSE, limit, error);

    sql = g_string_new(NULL);

    if (self->full_text) {
        g_string_append(sql,
                        "SELECT " SELECT_COLUMNS " FROM memories m "
                        "JOIN memories_fts f ON f.id = m.id "
                        "WHERE memories_fts MATCH ? AND m.archived = 0");
    } else {
        g_string_append(sql,
                        "SELECT " SELECT_COLUMNS " FROM memories m "
                        "WHERE m.archived = 0 AND ("
                        "m.content LIKE ? OR m.summary LIKE ? "
                        "OR m.tags LIKE ?)");
    }

    if (category != NULL && category[0] != '\0')
        g_string_append(sql, " AND m.category = ?");

    /*
     * rowid is the tie-break, and it is not decoration.
     *
     * created_at has one-second resolution, so anything written in the
     * same second has no defined order; the id does not save it either,
     * because two memories written in the same *millisecond* differ only
     * by the random tail. rowid is sqlite's own insertion counter, which
     * is exactly the question being asked.
     */
    g_string_append(sql, self->full_text
                         ? " ORDER BY m.pinned DESC, bm25(memories_fts), "
                           "m.rowid DESC LIMIT ?"
                         : " ORDER BY m.pinned DESC, m.created_at DESC, "
                           "m.rowid DESC LIMIT ?");

    if (sqlite3_prepare_v2(self->db, sql->str, -1, &stmt, NULL) != SQLITE_OK) {
        fail(self, error, "could not prepare the search");
        return g_steal_pointer(&out);
    }

    if (self->full_text) {
        /*
         * Quoted as an FTS5 string literal, through the one helper every
         * FTS5 table here uses.  A query is whatever a person or a model
         * typed, and unquoted it is FTS5 syntax -- a stray '"' or a bare
         * 'NOT' is a parse error rather than a search for those words,
         * and a parse error comes back as no rows.
         *
         * A query with nothing tokenizable in it is refused by the
         * helper with a reason, for the same reason: an empty phrase
         * matches nothing and reads as an empty store.
         */
        pattern = clawt_fts5_phrase(query, error);

        if (pattern == NULL) {
            sqlite3_finalize(stmt);
            return g_steal_pointer(&out);
        }

        sqlite3_bind_text(stmt, bind++, pattern, -1, SQLITE_TRANSIENT);
    } else {
        pattern = g_strdup_printf("%%%s%%", query);
        sqlite3_bind_text(stmt, bind++, pattern, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, bind++, pattern, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, bind++, pattern, -1, SQLITE_STATIC);
    }

    if (category != NULL && category[0] != '\0')
        sqlite3_bind_text(stmt, bind++, category, -1, SQLITE_STATIC);

    sqlite3_bind_int(stmt, bind, (gint)limit);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, memory_from_row(stmt));

    sqlite3_finalize(stmt);
    mark_accessed(self, out);

    return g_steal_pointer(&out);
}

GPtrArray *
clawt_memory_store_list(ClawtMemoryStore *self, const gchar *category,
                        gboolean pinned_only, guint limit, GError **error)
{
    g_autoptr(GPtrArray) out = NULL;
    g_autoptr(GString) sql = NULL;
    sqlite3_stmt *stmt = NULL;
    gint bind = 1;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_memory_free);

    if (limit == 0)
        limit = DEFAULT_LIMIT;

    sql = g_string_new("SELECT " SELECT_COLUMNS " FROM memories m "
                       "WHERE m.archived = 0");

    if (pinned_only)
        g_string_append(sql, " AND m.pinned = 1");

    if (category != NULL && category[0] != '\0')
        g_string_append(sql, " AND m.category = ?");

    g_string_append(sql, " ORDER BY m.pinned DESC, m.created_at DESC, "
                         "m.rowid DESC LIMIT ?");

    if (sqlite3_prepare_v2(self->db, sql->str, -1, &stmt, NULL) != SQLITE_OK) {
        fail(self, error, "could not prepare the listing");
        return g_steal_pointer(&out);
    }

    if (category != NULL && category[0] != '\0')
        sqlite3_bind_text(stmt, bind++, category, -1, SQLITE_STATIC);

    sqlite3_bind_int(stmt, bind, (gint)limit);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, memory_from_row(stmt));

    sqlite3_finalize(stmt);

    return g_steal_pointer(&out);
}

ClawtMemory *
clawt_memory_store_get(ClawtMemoryStore *self, const gchar *id, GError **error)
{
    g_autoptr(GPtrArray) one = NULL;
    ClawtMemory *memory = NULL;
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT " SELECT_COLUMNS
                           " FROM memories m WHERE m.id = ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        fail(self, error, "could not prepare the read");
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        memory = memory_from_row(stmt);

    sqlite3_finalize(stmt);

    if (memory == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no memory with id '%s'", id);
        return NULL;
    }

    one = g_ptr_array_new();
    g_ptr_array_add(one, memory);
    mark_accessed(self, one);

    return memory;
}

static gboolean
set_flag(ClawtMemoryStore *self, const gchar *sql, const gchar *id,
         gint value, GError **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return fail(self, error, "could not prepare the update");

    sqlite3_bind_int(stmt, 1, value);
    sqlite3_bind_int64(stmt, 2, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_bind_text(stmt, 3, id, -1, SQLITE_STATIC);

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (ok && sqlite3_changes(self->db) == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no memory with id '%s'", id);
        ok = FALSE;
    } else if (!ok) {
        fail(self, error, "could not update the memory");
    }

    sqlite3_finalize(stmt);

    return ok;
}

gboolean
clawt_memory_store_forget(ClawtMemoryStore *self, const gchar *id,
                          GError **error)
{
    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    return set_flag(self,
                    "UPDATE memories SET archived = ?, updated_at = ? "
                    "WHERE id = ?", id, 1, error);
}

gboolean
clawt_memory_store_pin(ClawtMemoryStore *self, const gchar *id,
                       gboolean pinned, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    return set_flag(self,
                    "UPDATE memories SET pinned = ?, updated_at = ? "
                    "WHERE id = ?", id, pinned ? 1 : 0, error);
}

guint
clawt_memory_store_count(ClawtMemoryStore *self, gboolean include_archived)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), 0);

    if (sqlite3_prepare_v2(self->db,
                           include_archived
                               ? "SELECT COUNT(*) FROM memories"
                               : "SELECT COUNT(*) FROM memories "
                                 "WHERE archived = 0",
                           -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);

    return count;
}

gboolean
clawt_memory_store_has_full_text(ClawtMemoryStore *self)
{
    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), FALSE);

    return self->full_text;
}

static void
clawt_memory_store_dispose(GObject *object)
{
    ClawtMemoryStore *self = CLAWT_MEMORY_STORE(object);

    /*
     * _v2, because the plain form refuses with SQLITE_BUSY when anything
     * is still outstanding and leaves the whole connection -- page cache
     * included -- allocated.
     */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    g_clear_pointer(&self->path, g_free);

    G_OBJECT_CLASS(clawt_memory_store_parent_class)->dispose(object);
}

static void
clawt_memory_store_class_init(ClawtMemoryStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_memory_store_dispose;
}

static void
clawt_memory_store_init(ClawtMemoryStore *self)
{
    (void)self;
}
