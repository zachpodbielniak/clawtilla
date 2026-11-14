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

    /* memories.journal_keep: journal rows kept, 0 for all of them. */
    guint journal_keep;
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
    "  ON memories(category, archived);"
    /*
     * The journal: one row per change, written by the function that
     * makes it.  A table of its own, so a database from before it
     * existed gains it on open -- CREATE TABLE IF NOT EXISTS reaches an
     * old file for a new table, unlike a new column.
     *
     * was_pinned / was_archived are the memory's state *before* the
     * change, which is what an undo restores; existed says an add
     * replaced a row with the same id, whose old text is not kept.
     */
    "CREATE TABLE IF NOT EXISTS memory_journal ("
    "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  at           INTEGER NOT NULL,"
    "  action       TEXT NOT NULL,"
    "  memory_id    TEXT NOT NULL,"
    "  actor        TEXT,"
    "  excerpt      TEXT,"
    "  was_pinned   INTEGER NOT NULL DEFAULT 0,"
    "  was_archived INTEGER NOT NULL DEFAULT 0,"
    "  existed      INTEGER NOT NULL DEFAULT 0,"
    "  undoes       INTEGER NOT NULL DEFAULT 0,"
    "  undone_at    INTEGER NOT NULL DEFAULT 0,"
    "  undone_by    TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS memory_journal_memory"
    "  ON memory_journal(memory_id, id);";

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

/* ── The journal ─────────────────────────────────────────────────── */

/* How much of a memory's text a journal row keeps, in characters. */
#define JOURNAL_EXCERPT_CHARS (80)
#define JOURNAL_DEFAULT_LIMIT (50)

/*
 * A memory's pinned and archived flags as they are now, so the journal
 * can record what a change is about to overwrite.  FALSE when there is
 * no such memory.
 */
static gboolean
read_flags(ClawtMemoryStore *self, const gchar *id, gboolean *pinned,
           gboolean *archived, gchar **excerpt)
{
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT pinned, archived, "
                           "COALESCE(summary, content) FROM memories "
                           "WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = TRUE;
        *pinned = sqlite3_column_int(stmt, 0) != 0;
        *archived = sqlite3_column_int(stmt, 1) != 0;

        if (excerpt != NULL)
            *excerpt = clawt_clip_line(column_text(stmt, 2),
                                       JOURNAL_EXCERPT_CHARS);
    }

    sqlite3_finalize(stmt);

    return found;
}

/* The oldest rows past memories.journal_keep, gone. */
static void
journal_trim(ClawtMemoryStore *self)
{
    sqlite3_stmt *stmt = NULL;

    if (self->journal_keep == 0)
        return;

    if (sqlite3_prepare_v2(self->db,
                           "DELETE FROM memory_journal WHERE id <= "
                           "(SELECT id FROM memory_journal ORDER BY id DESC "
                           "LIMIT 1 OFFSET ?)", -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_int(stmt, 1, (gint)self->journal_keep);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/*
 * One journal row.  A failure to journal is logged, not returned: the
 * change itself has happened by now, and reporting it as failed would
 * tell an agent its memory was not written when it was.
 */
static gint64
journal_write(ClawtMemoryStore *self, const gchar *action,
              const gchar *memory_id, const gchar *actor,
              const gchar *excerpt, gboolean was_pinned,
              gboolean was_archived, gboolean existed, gint64 undoes)
{
    sqlite3_stmt *stmt = NULL;
    gint64 row = 0;

    if (sqlite3_prepare_v2(self->db,
                           "INSERT INTO memory_journal (at, action, "
                           "memory_id, actor, excerpt, was_pinned, "
                           "was_archived, existed, undoes) VALUES "
                           "(?,?,?,?,?,?,?,?,?)", -1, &stmt,
                           NULL) != SQLITE_OK) {
        g_warning("memory %s: could not journal a %s: %s", self->path,
                  action, sqlite3_errmsg(self->db));
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_bind_text(stmt, 2, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, memory_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, actor, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, excerpt, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 6, was_pinned ? 1 : 0);
    sqlite3_bind_int(stmt, 7, was_archived ? 1 : 0);
    sqlite3_bind_int(stmt, 8, existed ? 1 : 0);
    sqlite3_bind_int64(stmt, 9, undoes);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        row = sqlite3_last_insert_rowid(self->db);
    else
        g_warning("memory %s: could not journal a %s: %s", self->path,
                  action, sqlite3_errmsg(self->db));

    sqlite3_finalize(stmt);

    journal_trim(self);

    return row;
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
    g_autofree gchar *excerpt = NULL;
    gboolean existed = FALSE;
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

    /*
     * Whether this replaces a row with the same id -- the one add an
     * undo cannot reverse, since the earlier text is not kept.
     */
    {
        gboolean was_pinned = FALSE;
        gboolean was_archived = FALSE;

        existed = read_flags(self, id, &was_pinned, &was_archived, NULL);
    }

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

    /*
     * Journaled here rather than by the tool that called, so a pod's
     * memory_add, the summariser and anything added later are on the
     * record too.  The source is the actor: it is who formed it.
     */
    excerpt = clawt_clip_line(
        (memory->summary != NULL && *memory->summary != '\0')
            ? memory->summary : memory->content,
        JOURNAL_EXCERPT_CHARS);
    journal_write(self, "add", id, memory->source, excerpt, FALSE, FALSE,
                  existed, 0);

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
    return clawt_memory_store_forget_as(self, id, NULL, error);
}

gboolean
clawt_memory_store_forget_as(ClawtMemoryStore *self, const gchar *id,
                             const gchar *actor, GError **error)
{
    gboolean was_pinned = FALSE;
    gboolean was_archived = FALSE;
    g_autofree gchar *excerpt = NULL;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    read_flags(self, id, &was_pinned, &was_archived, &excerpt);

    if (!set_flag(self,
                  "UPDATE memories SET archived = ?, updated_at = ? "
                  "WHERE id = ?", id, 1, error))
        return FALSE;

    journal_write(self, "forget", id, actor, excerpt, was_pinned,
                  was_archived, FALSE, 0);

    return TRUE;
}

gboolean
clawt_memory_store_pin(ClawtMemoryStore *self, const gchar *id,
                       gboolean pinned, GError **error)
{
    return clawt_memory_store_pin_as(self, id, pinned, NULL, error);
}

gboolean
clawt_memory_store_pin_as(ClawtMemoryStore *self, const gchar *id,
                          gboolean pinned, const gchar *actor,
                          GError **error)
{
    gboolean was_pinned = FALSE;
    gboolean was_archived = FALSE;
    g_autofree gchar *excerpt = NULL;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    read_flags(self, id, &was_pinned, &was_archived, &excerpt);

    if (!set_flag(self,
                  "UPDATE memories SET pinned = ?, updated_at = ? "
                  "WHERE id = ?", id, pinned ? 1 : 0, error))
        return FALSE;

    journal_write(self, pinned ? "pin" : "unpin", id, actor, excerpt,
                  was_pinned, was_archived, FALSE, 0);

    return TRUE;
}

void
clawt_memory_store_set_journal_keep(ClawtMemoryStore *self, guint keep)
{
    g_return_if_fail(CLAWT_IS_MEMORY_STORE(self));

    self->journal_keep = keep;
}

/*
 * Why a journal row cannot be undone now, or NULL when it can.
 *
 * One function, read by the listing (to grey the button and say why)
 * and by the undo itself (to refuse), so the two cannot disagree about
 * which rows are offered.
 */
static const gchar *
undo_refusal(ClawtMemoryStore *self, gint64 row, const gchar *action,
             const gchar *memory_id, gboolean existed, gint64 undone_at)
{
    sqlite3_stmt *stmt = NULL;
    gboolean pinned = FALSE;
    gboolean archived = FALSE;
    gboolean later = FALSE;

    if (g_strcmp0(action, "undo") == 0)
        return "this row is an undo; the change it took back has its own row";

    if (undone_at > 0)
        return "already undone";

    if (g_strcmp0(action, "add") == 0 && existed)
        return "it replaced an earlier version of the same memory, whose "
               "text is not kept, so undoing it would not bring that back";

    if (!read_flags(self, memory_id, &pinned, &archived, NULL))
        return "the memory is no longer on disk";

    /*
     * A later change still standing.  Undoing this one would overwrite
     * it -- bringing back a forgotten memory the agent had since pinned
     * and unpinned is not the state anybody asked for.
     */
    if (sqlite3_prepare_v2(self->db,
                           "SELECT 1 FROM memory_journal WHERE memory_id = ? "
                           "AND id > ? AND action <> 'undo' AND undone_at = 0 "
                           "LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, memory_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, row);
        later = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }

    if (later)
        return "a later change to the same memory has to be undone first";

    return NULL;
}

#define JOURNAL_COLUMNS \
    "id, at, action, memory_id, actor, excerpt, undoes, undone_at, " \
    "undone_by, existed"

static ClawtMemoryJournalEntry *
journal_from_row(ClawtMemoryStore *self, sqlite3_stmt *stmt)
{
    ClawtMemoryJournalEntry *entry = g_new0(ClawtMemoryJournalEntry, 1);
    const gchar *why;

    entry->id = sqlite3_column_int64(stmt, 0);
    entry->at = sqlite3_column_int64(stmt, 1);
    entry->action = g_strdup(column_text(stmt, 2));
    entry->memory_id = g_strdup(column_text(stmt, 3));
    entry->actor = g_strdup(column_text(stmt, 4));
    entry->excerpt = g_strdup(column_text(stmt, 5));
    entry->undoes = sqlite3_column_int64(stmt, 6);
    entry->undone_at = sqlite3_column_int64(stmt, 7);
    entry->undone_by = g_strdup(column_text(stmt, 8));

    why = undo_refusal(self, entry->id, entry->action, entry->memory_id,
                       sqlite3_column_int(stmt, 9) != 0, entry->undone_at);
    entry->undoable = (why == NULL);
    entry->why_not = g_strdup(why);

    return entry;
}

GPtrArray *
clawt_memory_store_journal(ClawtMemoryStore *self, const gchar *actor,
                           guint limit, GError **error)
{
    g_autoptr(GPtrArray) out = NULL;
    sqlite3_stmt *stmt = NULL;
    gboolean filtered = (actor != NULL && *actor != '\0');

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_memory_journal_entry_free);

    if (limit == 0)
        limit = JOURNAL_DEFAULT_LIMIT;

    if (sqlite3_prepare_v2(self->db,
                           filtered
                               ? "SELECT " JOURNAL_COLUMNS " FROM "
                                 "memory_journal WHERE actor = ? "
                                 "ORDER BY id DESC LIMIT ?"
                               : "SELECT " JOURNAL_COLUMNS " FROM "
                                 "memory_journal ORDER BY id DESC LIMIT ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        fail(self, error, "could not read the journal");
        return NULL;
    }

    if (filtered) {
        sqlite3_bind_text(stmt, 1, actor, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, (gint)limit);
    } else {
        sqlite3_bind_int(stmt, 1, (gint)limit);
    }

    /*
     * Collected before the refusals are worked out: each one runs its
     * own SELECT, and stepping a second statement inside the first is
     * fine for reads but the rows are cheap to hold either way.
     */
    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, journal_from_row(self, stmt));

    sqlite3_finalize(stmt);

    return g_steal_pointer(&out);
}

gboolean
clawt_memory_store_undo(ClawtMemoryStore *self, gint64 entry_id,
                        const gchar *actor, GError **error)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *action = NULL;
    g_autofree gchar *memory_id = NULL;
    g_autofree gchar *excerpt = NULL;
    gboolean was_pinned = FALSE;
    gboolean was_archived = FALSE;
    gboolean existed = FALSE;
    gint64 undone_at = 0;
    const gchar *why;
    const gchar *sql;
    gint value;
    gboolean found = FALSE;

    g_return_val_if_fail(CLAWT_IS_MEMORY_STORE(self), FALSE);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT action, memory_id, excerpt, was_pinned, "
                           "was_archived, existed, undone_at FROM "
                           "memory_journal WHERE id = ?", -1, &stmt,
                           NULL) != SQLITE_OK)
        return fail(self, error, "could not read the journal");

    sqlite3_bind_int64(stmt, 1, entry_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = TRUE;
        action = g_strdup(column_text(stmt, 0));
        memory_id = g_strdup(column_text(stmt, 1));
        excerpt = g_strdup(column_text(stmt, 2));
        was_pinned = sqlite3_column_int(stmt, 3) != 0;
        was_archived = sqlite3_column_int(stmt, 4) != 0;
        existed = sqlite3_column_int(stmt, 5) != 0;
        undone_at = sqlite3_column_int64(stmt, 6);
    }

    sqlite3_finalize(stmt);

    if (!found) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no change #%" G_GINT64_FORMAT " in this "
                    "journal; it may be older than memories.journal_keep",
                    entry_id);
        return FALSE;
    }

    why = undo_refusal(self, entry_id, action, memory_id, existed,
                       undone_at);

    if (why != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "change #%" G_GINT64_FORMAT " cannot be undone: %s",
                    entry_id, why);
        return FALSE;
    }

    /* What each action's reversal writes back. */
    if (g_strcmp0(action, "add") == 0) {
        sql = "UPDATE memories SET archived = ?, updated_at = ? WHERE id = ?";
        value = 1;
    } else if (g_strcmp0(action, "forget") == 0) {
        sql = "UPDATE memories SET archived = ?, updated_at = ? WHERE id = ?";
        value = was_archived ? 1 : 0;
    } else if (g_strcmp0(action, "pin") == 0 ||
               g_strcmp0(action, "unpin") == 0) {
        sql = "UPDATE memories SET pinned = ?, updated_at = ? WHERE id = ?";
        value = was_pinned ? 1 : 0;
    } else {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "change #%" G_GINT64_FORMAT " is a '%s', which this "
                    "version does not know how to undo", entry_id, action);
        return FALSE;
    }

    /*
     * The change, the mark and the undo's own row are one decision: a
     * crash between them would leave a memory restored with a journal
     * still offering to restore it.
     */
    if (sqlite3_exec(self->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) !=
        SQLITE_OK)
        return fail(self, error, "could not start the undo");

    if (!set_flag(self, sql, memory_id, value, error)) {
        sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
        return FALSE;
    }

    if (sqlite3_prepare_v2(self->db,
                           "UPDATE memory_journal SET undone_at = ?, "
                           "undone_by = ? WHERE id = ?", -1, &stmt,
                           NULL) != SQLITE_OK) {
        sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
        return fail(self, error, "could not mark the change undone");
    }

    sqlite3_bind_int64(stmt, 1, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_bind_text(stmt, 2, actor, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, entry_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
        return fail(self, error, "could not mark the change undone");
    }

    sqlite3_finalize(stmt);

    if (journal_write(self, "undo", memory_id, actor, excerpt, FALSE, FALSE,
                      FALSE, entry_id) == 0) {
        sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
        return fail(self, error, "could not journal the undo");
    }

    if (sqlite3_exec(self->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
        return fail(self, error, "could not finish the undo");
    }

    return TRUE;
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
    self->journal_keep = CLAWT_MEMORY_JOURNAL_KEEP_DEFAULT;
}
