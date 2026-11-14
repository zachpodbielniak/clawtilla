/*
 * clawt-grok-store.c - The Grok jobs that are owed an answer, on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-grok-store.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

/*
 * WAL and a busy timeout for the reason the trigger store has them: the
 * listener writes a report while a client is reading the listing.
 * Timestamps are microseconds, like every other in the tree, so a client
 * hands them to clawt_time_ago_label() unconverted.
 */
static const gchar SCHEMA_SQL[] =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS jobs ("
    "  id           TEXT PRIMARY KEY,"
    "  bot          TEXT NOT NULL,"
    "  agent_id     TEXT,"
    "  room_id      TEXT,"
    "  task_id      TEXT,"
    "  depth        INTEGER NOT NULL DEFAULT 0,"
    "  token_digest TEXT NOT NULL,"
    "  run_uuid     TEXT,"
    "  instructions TEXT,"
    "  state        INTEGER NOT NULL,"
    "  result       TEXT,"
    "  note         TEXT,"
    "  created_at   INTEGER NOT NULL,"
    "  updated_at   INTEGER NOT NULL,"
    "  expires_at   INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS jobs_by_time ON jobs(created_at);"
    "CREATE INDEX IF NOT EXISTS jobs_by_expiry ON jobs(state, expires_at);"
    /*
     * What agents said back to a bot in a conversation the bot started,
     * kept until the bot reads it -- so a reply survives a refused or
     * switched-off push, and a daemon restart, and the bot can always
     * pull what it was not pushed.
     */
    "CREATE TABLE IF NOT EXISTS replies ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  bot        TEXT NOT NULL,"
    "  agent_id   TEXT NOT NULL,"
    "  room_id    TEXT NOT NULL,"
    "  body       TEXT NOT NULL,"
    "  depth      INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  read       INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS replies_unread ON replies(bot, read, id);";

#define BUSY_TIMEOUT_MS (5000)

#define JOB_COLUMNS \
    "id, bot, agent_id, room_id, task_id, depth, run_uuid, instructions," \
    " state, result, note, created_at, updated_at, expires_at"

/*
 * The settled states, spelled once for SQL.  Built from
 * clawt_grok_job_state_is_settled() rather than written out, so a state
 * added to the enum is classified here by the same switch every client
 * uses.
 */
static gchar *
settled_list(void)
{
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    for (i = 0; i < clawt_grok_job_state_count(); i++) {
        ClawtGrokJobState state = clawt_grok_job_state_nth(i);

        if (!clawt_grok_job_state_is_settled(state))
            continue;

        if (out->len > 0)
            g_string_append_c(out, ',');

        g_string_append_printf(out, "%d", (gint)state);
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

struct _ClawtGrokStore {
    GObject  parent_instance;
    sqlite3 *db;
    gchar   *path;
    gchar   *settled;   /* "4,5,6,7,8": the IN () list */
};

G_DEFINE_FINAL_TYPE(ClawtGrokStore, clawt_grok_store, G_TYPE_OBJECT)

static void
set_sqlite_error(GError **error, sqlite3 *db, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s", what,
                db != NULL ? sqlite3_errmsg(db) : "no database");
}

static void
clawt_grok_store_dispose(GObject *object)
{
    ClawtGrokStore *self = CLAWT_GROK_STORE(object);

    /* _v2: the plain form refuses with SQLITE_BUSY and leaks the handle. */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    g_clear_pointer(&self->path, g_free);
    g_clear_pointer(&self->settled, g_free);

    G_OBJECT_CLASS(clawt_grok_store_parent_class)->dispose(object);
}

static void
clawt_grok_store_class_init(ClawtGrokStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_grok_store_dispose;
}

static void
clawt_grok_store_init(ClawtGrokStore *self)
{
    self->settled = settled_list();
}

ClawtGrokStore *
clawt_grok_store_new(const gchar *path, GError **error)
{
    ClawtGrokStore *self;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    self = g_object_new(CLAWT_TYPE_GROK_STORE, NULL);
    self->path = g_strdup(path);

    /*
     * sqlite3_open() leaves a usable handle even when it fails, so the
     * object is dropped and dispose closes it, rather than reopened into
     * the same pointer.
     */
    if (sqlite3_open(self->path, &self->db) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "opening the Grok job database");
        g_object_unref(self);
        return NULL;
    }

    sqlite3_busy_timeout(self->db, BUSY_TIMEOUT_MS);

    if (sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "creating the Grok job schema");
        g_object_unref(self);
        return NULL;
    }

    /* Jobs name rooms, agents and what they asked; nobody else reads them. */
    g_chmod(self->path, 0600);

    return self;
}

static void
bind_text(sqlite3_stmt *stmt, gint column, const gchar *value)
{
    if (value == NULL)
        sqlite3_bind_null(stmt, column);
    else
        sqlite3_bind_text(stmt, column, value, -1, SQLITE_TRANSIENT);
}

static const gchar *
column_text(sqlite3_stmt *stmt, gint column)
{
    return (const gchar *)sqlite3_column_text(stmt, column);
}

/* A row in JOB_COLUMNS order. */
static ClawtGrokJob *
job_from_row(sqlite3_stmt *stmt)
{
    ClawtGrokJob *job = clawt_grok_job_new(column_text(stmt, 0),
                                           column_text(stmt, 1));
    gint state = sqlite3_column_int(stmt, 8);

    clawt_grok_job_set_agent_id(job, column_text(stmt, 2));
    clawt_grok_job_set_room_id(job, column_text(stmt, 3));
    clawt_grok_job_set_task_id(job, column_text(stmt, 4));
    clawt_grok_job_set_depth(job, sqlite3_column_int(stmt, 5));
    clawt_grok_job_set_run_uuid(job, column_text(stmt, 6));
    clawt_grok_job_set_instructions(job, column_text(stmt, 7));

    /*
     * A number the enum does not have is read as failed, never as 0:
     * 0 is `sent`, which would make a row from a newer build look like a
     * job still in flight that nothing will ever settle.
     */
    if (state < 0 || (guint)state >= clawt_grok_job_state_count())
        state = CLAWT_GROK_JOB_FAILED;

    clawt_grok_job_set_state(job, (ClawtGrokJobState)state);
    clawt_grok_job_set_result(job, column_text(stmt, 9));
    clawt_grok_job_set_note(job, column_text(stmt, 10));
    clawt_grok_job_set_created_at(job, sqlite3_column_int64(stmt, 11));
    clawt_grok_job_set_updated_at(job, sqlite3_column_int64(stmt, 12));
    clawt_grok_job_set_expires_at(job, sqlite3_column_int64(stmt, 13));

    return job;
}

gboolean
clawt_grok_store_add(ClawtGrokStore  *self,
                     ClawtGrokJob    *job,
                     const gchar     *token_digest,
                     GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), FALSE);
    g_return_val_if_fail(job != NULL, FALSE);
    g_return_val_if_fail(token_digest != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO jobs(" JOB_COLUMNS ", token_digest)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "recording a Grok job");
        return FALSE;
    }

    bind_text(stmt, 1, clawt_grok_job_get_id(job));
    bind_text(stmt, 2, clawt_grok_job_get_bot(job));
    bind_text(stmt, 3, clawt_grok_job_get_agent_id(job));
    bind_text(stmt, 4, clawt_grok_job_get_room_id(job));
    bind_text(stmt, 5, clawt_grok_job_get_task_id(job));
    sqlite3_bind_int(stmt, 6, clawt_grok_job_get_depth(job));
    bind_text(stmt, 7, clawt_grok_job_get_run_uuid(job));
    bind_text(stmt, 8, clawt_grok_job_get_instructions(job));
    sqlite3_bind_int(stmt, 9, (gint)clawt_grok_job_get_state(job));
    bind_text(stmt, 10, clawt_grok_job_get_result(job));
    bind_text(stmt, 11, clawt_grok_job_get_note(job));
    sqlite3_bind_int64(stmt, 12, clawt_grok_job_get_created_at(job));
    sqlite3_bind_int64(stmt, 13, clawt_grok_job_get_updated_at(job));
    sqlite3_bind_int64(stmt, 14, clawt_grok_job_get_expires_at(job));
    bind_text(stmt, 15, token_digest);

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, "recording a Grok job");

    sqlite3_finalize(stmt);

    return ok;
}

ClawtGrokJob *
clawt_grok_store_get(ClawtGrokStore *self, const gchar *id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtGrokJob *job = NULL;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), NULL);

    if (id == NULL)
        return NULL;

    if (sqlite3_prepare_v2(self->db,
            "SELECT " JOB_COLUMNS " FROM jobs WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    bind_text(stmt, 1, id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        job = job_from_row(stmt);

    sqlite3_finalize(stmt);

    return job;
}

gboolean
clawt_grok_store_check_token(ClawtGrokStore *self,
                             const gchar    *id,
                             const gchar    *presented)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *digest = NULL;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), FALSE);

    if (id == NULL || presented == NULL)
        return FALSE;

    if (sqlite3_prepare_v2(self->db,
            "SELECT token_digest FROM jobs WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    bind_text(stmt, 1, id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        digest = g_strdup(column_text(stmt, 0));

    sqlite3_finalize(stmt);

    return clawt_grok_token_matches(digest, presented);
}

gboolean
clawt_grok_store_update(ClawtGrokStore  *self,
                        ClawtGrokJob    *job,
                        GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), FALSE);
    g_return_val_if_fail(job != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
            "UPDATE jobs SET state = ?, run_uuid = ?, result = ?, note = ?,"
            " updated_at = ? WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "updating a Grok job");
        return FALSE;
    }

    sqlite3_bind_int(stmt, 1, (gint)clawt_grok_job_get_state(job));
    bind_text(stmt, 2, clawt_grok_job_get_run_uuid(job));
    bind_text(stmt, 3, clawt_grok_job_get_result(job));
    bind_text(stmt, 4, clawt_grok_job_get_note(job));
    sqlite3_bind_int64(stmt, 5, clawt_grok_job_get_updated_at(job));
    bind_text(stmt, 6, clawt_grok_job_get_id(job));

    ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(self->db) == 1;

    if (!ok)
        set_sqlite_error(error, self->db, "updating a Grok job");

    sqlite3_finalize(stmt);

    return ok;
}

ClawtGrokJob *
clawt_grok_store_settle(ClawtGrokStore     *self,
                        const gchar        *id,
                        ClawtGrokJobState   state,
                        const gchar        *text,
                        gint64              now,
                        GError            **error)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *sql = NULL;
    gboolean changed;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), NULL);
    g_return_val_if_fail(clawt_grok_job_state_is_settled(state), NULL);

    /*
     * The not-already-settled condition is in the UPDATE itself.  A read
     * then a write would let a report and the expiry sweep both see an
     * open job and both deliver -- one "here is the result" and one "it
     * never answered" for the same job.
     */
    sql = g_strdup_printf("UPDATE jobs SET state = ?, result = ?,"
                          " updated_at = ? WHERE id = ?"
                          " AND state NOT IN (%s)", self->settled);

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "settling a Grok job");
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, (gint)state);
    bind_text(stmt, 2, text);
    sqlite3_bind_int64(stmt, 3, now);
    bind_text(stmt, 4, id);

    changed = sqlite3_step(stmt) == SQLITE_DONE &&
              sqlite3_changes(self->db) == 1;
    sqlite3_finalize(stmt);

    if (!changed) {
        g_autoptr(ClawtGrokJob) existing = clawt_grok_store_get(self, id);

        if (existing == NULL)
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "there is no Grok job %s", id);
        else
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                        "Grok job %s was already settled as %s", id,
                        clawt_enum_to_nick(CLAWT_TYPE_GROK_JOB_STATE,
                            (gint)clawt_grok_job_get_state(existing)));

        return NULL;
    }

    return clawt_grok_store_get(self, id);
}

static GPtrArray *
collect(sqlite3_stmt *stmt)
{
    GPtrArray *jobs = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_grok_job_free);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(jobs, job_from_row(stmt));

    sqlite3_finalize(stmt);

    return jobs;
}

GPtrArray *
clawt_grok_store_list(ClawtGrokStore *self,
                      const gchar    *bot,
                      const gchar    *agent_id,
                      gboolean        open_only,
                      guint           limit)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *sql = NULL;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), NULL);

    {
        g_autofree gchar *open_clause = open_only
            ? g_strdup_printf(" AND state NOT IN (%s)", self->settled)
            : g_strdup("");

        sql = g_strdup_printf(
            "SELECT " JOB_COLUMNS " FROM jobs"
            " WHERE (?1 IS NULL OR bot = ?1)"
            " AND (?2 IS NULL OR agent_id = ?2)%s"
            " ORDER BY created_at DESC LIMIT ?3", open_clause);
    }

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_grok_job_free);

    bind_text(stmt, 1, bot);
    bind_text(stmt, 2, agent_id);
    sqlite3_bind_int(stmt, 3, (gint)MAX(limit, 1));

    return collect(stmt);
}

GPtrArray *
clawt_grok_store_due(ClawtGrokStore *self, gint64 now)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *sql = NULL;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), NULL);

    sql = g_strdup_printf("SELECT " JOB_COLUMNS " FROM jobs"
                          " WHERE state NOT IN (%s) AND expires_at <= ?"
                          " ORDER BY expires_at", self->settled);

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_grok_job_free);

    sqlite3_bind_int64(stmt, 1, now);

    return collect(stmt);
}

guint
clawt_grok_store_prune(ClawtGrokStore *self, gint64 before)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *sql = NULL;
    guint removed = 0;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), 0);

    sql = g_strdup_printf("DELETE FROM jobs WHERE state IN (%s)"
                          " AND updated_at < ?", self->settled);

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, before);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        removed = (guint)sqlite3_changes(self->db);

    sqlite3_finalize(stmt);

    return removed;
}

/* ── Replies to a bot ────────────────────────────────────────────── */

gint64
clawt_grok_store_add_reply(ClawtGrokStore  *self,
                           const gchar     *bot,
                           const gchar     *agent_id,
                           const gchar     *room_id,
                           const gchar     *body,
                           gint             depth,
                           GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    gint64 id = 0;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), 0);

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO replies(bot, agent_id, room_id, body, depth,"
            " created_at) VALUES (?, ?, ?, ?, ?, ?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "recording a reply to a bot");
        return 0;
    }

    bind_text(stmt, 1, bot);
    bind_text(stmt, 2, agent_id);
    bind_text(stmt, 3, room_id);
    bind_text(stmt, 4, body);
    sqlite3_bind_int(stmt, 5, depth);
    sqlite3_bind_int64(stmt, 6, g_get_real_time());

    if (sqlite3_step(stmt) == SQLITE_DONE)
        id = sqlite3_last_insert_rowid(self->db);
    else
        set_sqlite_error(error, self->db, "recording a reply to a bot");

    sqlite3_finalize(stmt);

    return id;
}

void
clawt_grok_store_mark_reply_read(ClawtGrokStore *self, gint64 id)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_GROK_STORE(self));

    if (sqlite3_prepare_v2(self->db,
            "UPDATE replies SET read = 1 WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

JsonArray *
clawt_grok_store_take_replies(ClawtGrokStore *self,
                              const gchar    *bot,
                              const gchar    *agent_id,
                              guint           limit)
{
    sqlite3_stmt *stmt = NULL;
    JsonArray *out = json_array_new();
    g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    guint i;

    g_return_val_if_fail(CLAWT_IS_GROK_STORE(self), out);

    if (sqlite3_prepare_v2(self->db,
            "SELECT id, agent_id, room_id, body, depth, created_at"
            " FROM replies WHERE bot = ?1 AND read = 0"
            " AND (?2 IS NULL OR agent_id = ?2) ORDER BY id LIMIT ?3",
            -1, &stmt, NULL) != SQLITE_OK)
        return out;

    bind_text(stmt, 1, bot);
    bind_text(stmt, 2, agent_id);
    sqlite3_bind_int(stmt, 3, (gint)MAX(limit, 1));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *reply = json_object_new();
        gint64 id = sqlite3_column_int64(stmt, 0);

        json_object_set_int_member(reply, "id", id);
        json_object_set_string_member(reply, "agent", column_text(stmt, 1));
        json_object_set_string_member(reply, "conversation",
                                      column_text(stmt, 2));
        json_object_set_string_member(reply, "body", column_text(stmt, 3));
        json_object_set_int_member(reply, "depth",
                                   sqlite3_column_int(stmt, 4));
        json_object_set_int_member(reply, "created_at",
                                   sqlite3_column_int64(stmt, 5));
        json_array_add_object_element(out, reply);
        g_array_append_val(ids, id);
    }

    sqlite3_finalize(stmt);

    /* Taken is read: a reply handed over twice would be answered twice. */
    for (i = 0; i < ids->len; i++)
        clawt_grok_store_mark_reply_read(self, g_array_index(ids, gint64, i));

    return out;
}
