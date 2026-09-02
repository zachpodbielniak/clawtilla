/*
 * clawt-transcript-index.c - Searching every conversation the fleet had
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "memory/clawt-transcript-index.h"

#include <sqlite3.h>
#include <string.h>

struct _ClawtTranscriptIndex {
    GObject parent_instance;

    sqlite3 *db;
    gchar   *path;

    /* Whether this sqlite has FTS5.  Not every build does. */
    gboolean full_text;
};

G_DEFINE_FINAL_TYPE(ClawtTranscriptIndex, clawt_transcript_index,
                    G_TYPE_OBJECT)

#define DEFAULT_LIMIT 20

/* ── The hit ─────────────────────────────────────────────────────── */

ClawtTranscriptHit *
clawt_transcript_hit_copy(ClawtTranscriptHit *self)
{
    ClawtTranscriptHit *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtTranscriptHit, 1);
    copy->id = g_strdup(self->id);
    copy->room_id = g_strdup(self->room_id);
    copy->sender_id = g_strdup(self->sender_id);
    copy->sender_name = g_strdup(self->sender_name);
    copy->body = g_strdup(self->body);
    copy->timestamp = self->timestamp;

    return copy;
}

void
clawt_transcript_hit_free(ClawtTranscriptHit *self)
{
    if (self == NULL)
        return;

    g_free(self->id);
    g_free(self->room_id);
    g_free(self->sender_id);
    g_free(self->sender_name);
    g_free(self->body);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtTranscriptHit, clawt_transcript_hit,
                    clawt_transcript_hit_copy, clawt_transcript_hit_free)

/* ── The schema ──────────────────────────────────────────────────── */

/*
 * WAL, for the reason the mailbox and the memory store both use it: the
 * daemon writes while a client is reading the same file to show somebody
 * what the fleet said.
 */
static const gchar SCHEMA[] =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS messages ("
    "  id          TEXT PRIMARY KEY,"
    "  room_id     TEXT NOT NULL,"
    "  sender_id   TEXT NOT NULL,"
    "  sender_name TEXT,"
    "  body        TEXT NOT NULL,"
    "  ts          INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS messages_room ON messages(room_id, ts DESC);"
    "CREATE INDEX IF NOT EXISTS messages_sender"
    "  ON messages(sender_id, ts DESC);";

/*
 * A standalone FTS5 table, not an external-content one, for the same
 * reason #ClawtMemoryStore uses one: external content ties the index to
 * the base table's implicit rowid, and this file has to survive being
 * copied, backed up and restored.
 */
static const gchar FTS_SCHEMA[] =
    "CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts"
    "  USING fts5(id UNINDEXED, body);";

/*
 * The columns, once bare for an INSERT and once qualified for a SELECT.
 *
 * The qualified form is not decoration.  Search joins messages_fts,
 * which also has an id, and an unqualified list makes the whole query
 * fail with "ambiguous column name" -- silently, because a search that
 * could not be prepared reports no matches rather than an error.  The
 * memory store carries the same pair for the same reason.
 */
#define INSERT_COLUMNS \
    "id, room_id, sender_id, sender_name, body, ts"

#define SELECT_COLUMNS \
    "m.id, m.room_id, m.sender_id, m.sender_name, m.body, m.ts"

/*
 * Every column the table must have, so a database written by an older
 * clawtilla can be brought up to date.
 *
 * `CREATE TABLE IF NOT EXISTS` does nothing at all to a file that
 * already has the table, so a column added later reaches new databases
 * only -- and every existing one then fails its first read.  The mailbox
 * learned this by quarantining a live fleet's queued work as corrupt on
 * upgrade.  PRAGMA table_info is asked rather than an ALTER being tried
 * and ignored, because a failed ALTER cannot tell an existing column
 * from an unwritable file.
 */
static const struct {
    const gchar *name;
    const gchar *definition;
} REQUIRED_COLUMNS[] = {
    { "id",          "TEXT" },
    { "room_id",     "TEXT NOT NULL DEFAULT ''" },
    { "sender_id",   "TEXT NOT NULL DEFAULT ''" },
    { "sender_name", "TEXT" },
    { "body",        "TEXT NOT NULL DEFAULT ''" },
    { "ts",          "INTEGER NOT NULL DEFAULT 0" }
};

static gboolean
fail(ClawtTranscriptIndex *self, GError **error, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s", what,
                self->db != NULL ? sqlite3_errmsg(self->db) : "no database");
    return FALSE;
}

/*
 * Creates the table if it is absent and adds anything a newer clawtilla
 * expects if it is not.
 *
 * One place, so the two ways in -- a fresh file and one that has been
 * here for months -- cannot end up with different schemas.
 */
static gboolean
apply_schema(ClawtTranscriptIndex *self, GError **error)
{
    gchar *message = NULL;
    g_autoptr(GHashTable) present = NULL;
    sqlite3_stmt *stmt = NULL;
    gsize i;

    if (sqlite3_exec(self->db, SCHEMA, NULL, NULL, &message) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create the transcript schema: %s",
                    message != NULL ? message : "unknown");
        sqlite3_free(message);
        return FALSE;
    }

    present = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (sqlite3_prepare_v2(self->db, "PRAGMA table_info(messages)", -1, &stmt,
                           NULL) != SQLITE_OK)
        return fail(self, error, "could not read the transcript schema");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *name = (const gchar *)sqlite3_column_text(stmt, 1);

        if (name != NULL)
            g_hash_table_add(present, g_strdup(name));
    }

    sqlite3_finalize(stmt);

    for (i = 0; i < G_N_ELEMENTS(REQUIRED_COLUMNS); i++) {
        g_autofree gchar *sql = NULL;

        if (g_hash_table_contains(present, REQUIRED_COLUMNS[i].name))
            continue;

        sql = g_strdup_printf("ALTER TABLE messages ADD COLUMN %s %s",
                              REQUIRED_COLUMNS[i].name,
                              REQUIRED_COLUMNS[i].definition);

        if (sqlite3_exec(self->db, sql, NULL, NULL, &message) != SQLITE_OK) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not add the '%s' column: %s",
                        REQUIRED_COLUMNS[i].name,
                        message != NULL ? message : "unknown");
            sqlite3_free(message);
            return FALSE;
        }
    }

    /*
     * FTS5 is optional at the sqlite build level, and its absence is a
     * reason to rank worse rather than to refuse to record anything.
     */
    self->full_text = sqlite3_exec(self->db, FTS_SCHEMA, NULL, NULL,
                                   &message) == SQLITE_OK;

    if (!self->full_text) {
        g_info("transcript: no FTS5 in this sqlite (%s); recall will use a "
               "substring match", message != NULL ? message : "unknown");
        sqlite3_free(message);
    }

    return TRUE;
}

ClawtTranscriptIndex *
clawt_transcript_index_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtTranscriptIndex) self = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_TRANSCRIPT_INDEX, NULL);
    self->path = g_strdup(path);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        /*
         * The connection is usable even when open fails -- that is how
         * sqlite3_errmsg() works on it -- so dispose closes it either
         * way rather than stranding it.
         */
        fail(self, error, "could not open the transcript index");
        return NULL;
    }

    if (!apply_schema(self, error))
        return NULL;

    return g_steal_pointer(&self);
}

gboolean
clawt_transcript_index_add(ClawtTranscriptIndex *self, const gchar *room_id,
                           ClawtMessage *message, GError **error)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *body = NULL;
    const gchar *id;
    const gchar *raw;

    g_return_val_if_fail(CLAWT_IS_TRANSCRIPT_INDEX(self), FALSE);
    g_return_val_if_fail(room_id != NULL, FALSE);
    g_return_val_if_fail(message != NULL, FALSE);

    id = clawt_message_get_id(message);
    raw = clawt_message_get_body(message);

    /*
     * A message with no id cannot be replaced on a re-index, and a
     * message with no body has nothing to find.  Neither is an error
     * worth failing a send over: this runs on the delivery path.
     */
    if (id == NULL || *id == '\0' || raw == NULL || *raw == '\0')
        return TRUE;

    /*
     * Redacted here rather than by the caller, because there are two
     * callers and one of them was already right: clawt_room_append()
     * scrubs the body before writing the JSONL transcript, and said so.
     * The row beside it kept the plaintext -- and this is the copy that
     * gets searched, so clawtilla_recall handed a model the very key the
     * file next to it had been careful not to keep.  The daemon's
     * start-time re-index goes through here too, so the rule lives in
     * the store and cannot be forgotten at a third call site.
     */
    body = clawt_redact_secrets(raw);

    if (sqlite3_prepare_v2(self->db,
                           "INSERT OR REPLACE INTO messages ("
                           INSERT_COLUMNS ") VALUES (?,?,?,?,?,?)",
                           -1, &stmt, NULL) != SQLITE_OK)
        return fail(self, error, "could not prepare the transcript insert");

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, room_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, clawt_message_get_sender_id(message), -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, clawt_message_get_sender_name(message), -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, body, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, clawt_message_get_timestamp(message));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fail(self, error, "could not index the message");
        sqlite3_finalize(stmt);
        return FALSE;
    }

    sqlite3_finalize(stmt);

    if (self->full_text) {
        /*
         * Deleted then inserted, so a message re-indexed at start is
         * indexed once rather than once per daemon lifetime.
         */
        if (sqlite3_prepare_v2(self->db,
                               "DELETE FROM messages_fts WHERE id = ?",
                               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        if (sqlite3_prepare_v2(self->db,
                               "INSERT INTO messages_fts (id, body) "
                               "VALUES (?, ?)", -1, &stmt,
                               NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, body, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return TRUE;
}

static ClawtTranscriptHit *
hit_from_row(sqlite3_stmt *stmt)
{
    ClawtTranscriptHit *hit = g_new0(ClawtTranscriptHit, 1);

    hit->id = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    hit->room_id = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    hit->sender_id = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
    hit->sender_name = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
    hit->body = g_strdup((const gchar *)sqlite3_column_text(stmt, 4));
    hit->timestamp = sqlite3_column_int64(stmt, 5);

    return hit;
}

GPtrArray *
clawt_transcript_index_search(ClawtTranscriptIndex *self, const gchar *query,
                              const gchar * const *rooms, const gchar *sender,
                              gint64 since, guint limit, GError **error)
{
    g_autoptr(GPtrArray) out = NULL;
    g_autoptr(GString) sql = NULL;
    g_autofree gchar *pattern = NULL;
    sqlite3_stmt *stmt = NULL;
    gsize n_rooms = 0;
    gsize i;
    gint bind = 1;

    g_return_val_if_fail(CLAWT_IS_TRANSCRIPT_INDEX(self), NULL);

    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_transcript_hit_free);

    if (limit == 0)
        limit = DEFAULT_LIMIT;

    /*
     * An empty allowed-room list is not "no filter": it is an agent that
     * is in no room at all, and it must see nothing.  %NULL is the
     * caller saying it speaks for the operator.
     */
    if (rooms != NULL) {
        while (rooms[n_rooms] != NULL)
            n_rooms++;

        if (n_rooms == 0)
            return g_steal_pointer(&out);
    }

    /*
     * Quoted before anything is built, so a query that cannot be
     * searched at all is refused with a reason rather than prepared,
     * run and reported as no matches -- which is what an empty store
     * looks like too.
     */
    if (self->full_text && query != NULL && query[0] != '\0') {
        pattern = clawt_fts5_phrase(query, error);

        if (pattern == NULL)
            return g_steal_pointer(&out);
    } else if (query != NULL && query[0] != '\0') {
        pattern = g_strdup_printf("%%%s%%", query);
    }

    sql = g_string_new(NULL);

    if (self->full_text && query != NULL && query[0] != '\0') {
        g_string_append(sql,
                        "SELECT " SELECT_COLUMNS " FROM messages m "
                        "JOIN messages_fts f ON f.id = m.id "
                        "WHERE messages_fts MATCH ?");
    } else if (query != NULL && query[0] != '\0') {
        g_string_append(sql,
                        "SELECT " SELECT_COLUMNS " FROM messages m "
                        "WHERE m.body LIKE ?");
    } else {
        g_string_append(sql,
                        "SELECT " SELECT_COLUMNS " FROM messages m "
                        "WHERE 1 = 1");
    }

    if (n_rooms > 0) {
        g_string_append(sql, " AND m.room_id IN (");

        for (i = 0; i < n_rooms; i++)
            g_string_append(sql, i == 0 ? "?" : ",?");

        g_string_append_c(sql, ')');
    }

    if (sender != NULL && sender[0] != '\0')
        g_string_append(sql, " AND m.sender_id = ?");

    if (since > 0)
        g_string_append(sql, " AND m.ts >= ?");

    /*
     * Newest first rather than by rank, even when FTS5 ranked it.
     *
     * Recall answers "when did we last talk about this", and the useful
     * answer is the most recent time rather than the densest match.
     * m.rowid is the tie-break because ts has one-second resolution, so
     * everything said in the same second has no defined order without
     * it.
     */
    g_string_append(sql, " ORDER BY m.ts DESC, m.rowid DESC LIMIT ?");

    if (sqlite3_prepare_v2(self->db, sql->str, -1, &stmt, NULL) != SQLITE_OK) {
        fail(self, error, "could not prepare the recall");
        return g_steal_pointer(&out);
    }

    if (pattern != NULL)
        sqlite3_bind_text(stmt, bind++, pattern, -1, SQLITE_STATIC);

    for (i = 0; i < n_rooms; i++)
        sqlite3_bind_text(stmt, bind++, rooms[i], -1, SQLITE_STATIC);

    if (sender != NULL && sender[0] != '\0')
        sqlite3_bind_text(stmt, bind++, sender, -1, SQLITE_STATIC);

    if (since > 0)
        sqlite3_bind_int64(stmt, bind++, since);

    sqlite3_bind_int(stmt, bind, (gint)limit);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, hit_from_row(stmt));

    sqlite3_finalize(stmt);

    return g_steal_pointer(&out);
}

guint
clawt_transcript_index_count(ClawtTranscriptIndex *self)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_TRANSCRIPT_INDEX(self), 0);

    if (sqlite3_prepare_v2(self->db, "SELECT COUNT(*) FROM messages", -1,
                           &stmt, NULL) != SQLITE_OK)
        return 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);

    return count;
}

gboolean
clawt_transcript_index_has_full_text(ClawtTranscriptIndex *self)
{
    g_return_val_if_fail(CLAWT_IS_TRANSCRIPT_INDEX(self), FALSE);

    return self->full_text;
}

static void
clawt_transcript_index_dispose(GObject *object)
{
    ClawtTranscriptIndex *self = CLAWT_TRANSCRIPT_INDEX(object);

    /*
     * _v2, because the plain form refuses with SQLITE_BUSY when anything
     * is still outstanding and leaves the connection -- page cache
     * included -- allocated.
     */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    g_clear_pointer(&self->path, g_free);

    G_OBJECT_CLASS(clawt_transcript_index_parent_class)->dispose(object);
}

static void
clawt_transcript_index_class_init(ClawtTranscriptIndexClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_transcript_index_dispose;
}

static void
clawt_transcript_index_init(ClawtTranscriptIndex *self)
{
    (void)self;
}
