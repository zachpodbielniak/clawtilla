/*
 * clawt-task-store.c - Every task, and every problem report, on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <sqlite3.h>

#include "clawtilla.h"
#include "task/clawt-task-store.h"

/*
 * The indexed columns are for finding rows; `data` is what is restored.
 * `finished_at` is 0 for a task that has not finished, which is what
 * "live" means to clawt_task_store_load_live() and why pruning can never
 * reach one.
 */
static const gchar *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS tasks ("
    "  id TEXT PRIMARY KEY,"
    "  state INTEGER NOT NULL,"
    "  assignee TEXT,"
    "  origin TEXT,"
    "  parent_id TEXT,"
    "  title TEXT,"
    "  created_at INTEGER NOT NULL,"
    "  finished_at INTEGER NOT NULL DEFAULT 0,"
    "  data TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS tasks_created ON tasks(created_at);"
    "CREATE INDEX IF NOT EXISTS tasks_finished ON tasks(finished_at);"
    "CREATE INDEX IF NOT EXISTS tasks_assignee ON tasks(assignee, created_at);"
    "CREATE INDEX IF NOT EXISTS tasks_parent ON tasks(parent_id);"
    "CREATE TABLE IF NOT EXISTS task_journal ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  task_id TEXT NOT NULL,"
    "  at INTEGER NOT NULL,"
    "  state INTEGER NOT NULL,"
    "  what TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS task_journal_task ON task_journal(task_id, id);"
    "CREATE TABLE IF NOT EXISTS task_notes ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  task_id TEXT NOT NULL,"
    "  at INTEGER NOT NULL,"
    "  author TEXT,"
    "  kind TEXT NOT NULL,"
    "  body TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS task_notes_task ON task_notes(task_id, id);"
    "CREATE TABLE IF NOT EXISTS task_links ("
    "  blocker TEXT NOT NULL,"
    "  blocked TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  PRIMARY KEY (blocker, blocked)"
    ");"
    "CREATE INDEX IF NOT EXISTS task_links_blocked ON task_links(blocked);"
    "CREATE TABLE IF NOT EXISTS held_deliveries ("
    "  task_id TEXT PRIMARY KEY,"
    "  data TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS incidents ("
    "  id TEXT PRIMARY KEY,"
    "  opened_at INTEGER NOT NULL,"
    "  data TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS incidents_opened ON incidents(opened_at);"
    /*
     * The VENTURE mirror's side (0.44.0).  The outbox is written in the
     * same transaction as the change it reports, so a crash between
     * "the task moved" and "VENTURE must hear of it" cannot happen; the
     * mirror only ever reads it.  `state` 0 is queued, 2 is dead.  A
     * task's `sync` row is coalesced -- one queued row per task, its
     * `gen` bumped by every later change -- while each note is its own
     * row, in order.
     */
    "CREATE TABLE IF NOT EXISTS venture_outbox ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  task_id TEXT NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  note_id INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  next_at INTEGER NOT NULL,"
    "  attempts INTEGER NOT NULL DEFAULT 0,"
    "  gen INTEGER NOT NULL DEFAULT 1,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  error TEXT"
    ");"
    "CREATE UNIQUE INDEX IF NOT EXISTS venture_outbox_queued "
    "  ON venture_outbox(task_id, kind, note_id) WHERE state = 0;"
    "CREATE INDEX IF NOT EXISTS venture_outbox_task "
    "  ON venture_outbox(task_id, id);"
    "CREATE TABLE IF NOT EXISTS venture_refs ("
    "  task_id TEXT PRIMARY KEY,"
    "  instance TEXT,"
    "  ticket_id INTEGER NOT NULL DEFAULT 0,"
    "  organization_id INTEGER NOT NULL DEFAULT 0,"
    "  version INTEGER NOT NULL DEFAULT 0,"
    "  snapshot TEXT,"
    "  state TEXT NOT NULL,"
    "  reason TEXT,"
    "  url TEXT,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS venture_note_refs ("
    "  note_id INTEGER PRIMARY KEY,"
    "  task_id TEXT NOT NULL,"
    "  comment_id INTEGER NOT NULL"
    ");"
    /* Where the reverse poll got to, per account (0.45.0). */
    "CREATE TABLE IF NOT EXISTS venture_cursors ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ");"
    /*
     * Each team's SELinux MCS level (0.78.0): picked once, the first time
     * one of the team's containers is created, and kept for good so the
     * team's containers can always read what each of them writes into a
     * shared folder.  UNIQUE on the level, so two teams can never be
     * handed one pair however the allocation is raced.
     */
    "CREATE TABLE IF NOT EXISTS team_selinux_levels ("
    "  team TEXT PRIMARY KEY,"
    "  level TEXT NOT NULL UNIQUE,"
    "  created_at INTEGER NOT NULL"
    ");";

struct _ClawtTaskStore {
    GObject  parent_instance;
    sqlite3 *db;
    gchar   *path;
    gboolean mirror;     /* write outbox rows (the VENTURE mirror is on) */
};

/*
 * One outbox row for @task_id: a coalesced `sync` (@note_id 0) or one
 * note.  Inside the caller's transaction.  A queued row already there
 * has its generation bumped and is made due now, so a change made while
 * the mirror is sending the previous one is not lost.
 */
static gboolean
outbox_add(ClawtTaskStore *self, const gchar *task_id, const gchar *kind,
           gint64 note_id, GError **error)
{
    sqlite3_stmt *stmt = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gboolean ok;

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO venture_outbox "
            "  (task_id, kind, note_id, created_at, next_at) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(task_id, kind, note_id) WHERE state = 0 "
            "DO UPDATE SET gen = gen + 1, next_at = MIN(next_at, excluded.next_at)",
            -1, &stmt, NULL) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "preparing an outbox row: %s", sqlite3_errmsg(self->db));
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, note_id);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int64(stmt, 5, now);

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "adding an outbox row: %s", sqlite3_errmsg(self->db));

    sqlite3_finalize(stmt);
    return ok;
}

G_DEFINE_FINAL_TYPE(ClawtTaskStore, clawt_task_store, G_TYPE_OBJECT)

static void
set_sqlite_error(GError **error, sqlite3 *db, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                "%s: %s", what, sqlite3_errmsg(db));
}

static gint64
now_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

/* Whether @table has @column, from its own description. */
static gboolean
has_column(ClawtTaskStore *self, const gchar *table, const gchar *column)
{
    g_autofree gchar *sql = g_strdup_printf("PRAGMA table_info(%s)", table);
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (g_strcmp0((const gchar *)sqlite3_column_text(stmt, 1),
                      column) == 0)
            found = TRUE;
    }

    sqlite3_finalize(stmt);

    return found;
}

ClawtTaskStore *
clawt_task_store_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtTaskStore) self = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    self = g_object_new(CLAWT_TYPE_TASK_STORE, NULL);
    self->path = g_strdup(path);

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "opening the task store");
        return NULL;
    }

    /*
     * WAL, because the daemon writes on every task transition while a
     * client's history page reads: a reader must not wait on a writer,
     * and a writer must not wait on a long list.
     */
    sqlite3_busy_timeout(self->db, 2000);
    sqlite3_exec(self->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(self->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    if (sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "creating the task store's tables");
        return NULL;
    }

    /*
     * Columns added after the table first shipped: a store written by an
     * older daemon gets them now, empty, and is filled in as its tasks
     * are next saved.  `project` (0.40.0) is what a project's month of
     * spend is summed over.
     */
    if (!has_column(self, "tasks", "project") &&
        sqlite3_exec(self->db, "ALTER TABLE tasks ADD COLUMN project TEXT;",
                     NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "adding tasks.project");
        return NULL;
    }

    /*
     * venture_refs.organization_id: which VENTURE organization a ticket
     * is in, so its link can switch the browser to it.  Old rows get 0
     * (not known).
     */
    if (!has_column(self, "venture_refs", "organization_id") &&
        sqlite3_exec(self->db,
                     "ALTER TABLE venture_refs ADD COLUMN organization_id "
                     "INTEGER NOT NULL DEFAULT 0;",
                     NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "adding venture_refs.organization_id");
        return NULL;
    }

    /*
     * Links stored before VENTURE's record pages were known point at the
     * board, <base>/tickets/<id>; the record is <base>/e/ticket/<id>.
     * Rewritten in place, keeping whatever base the row held.  Idempotent:
     * a rewritten url no longer ends in /tickets/<id>.  The organization
     * of an old row is not known, so it gets the bare page.
     */
    sqlite3_exec(self->db,
                 "UPDATE venture_refs SET url = "
                 "  substr(url, 1, length(url) - length('/tickets/' || ticket_id)) "
                 "  || '/e/ticket/' || ticket_id "
                 "WHERE ticket_id > 0 AND url IS NOT NULL "
                 "  AND length(url) > length('/tickets/' || ticket_id) "
                 "  AND substr(url, -length('/tickets/' || ticket_id)) "
                 "      = '/tickets/' || ticket_id;",
                 NULL, NULL, NULL);

    sqlite3_exec(self->db,
                 "CREATE INDEX IF NOT EXISTS tasks_project "
                 "ON tasks(project, created_at);", NULL, NULL, NULL);

    return (ClawtTaskStore *)g_steal_pointer(&self);
}

const gchar *
clawt_task_store_get_path(ClawtTaskStore *self)
{
    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    return self->path;
}

/*
 * The state the row says the task is in, or -1 when there is no row.
 * Read inside the save's transaction, which is what decides whether this
 * save is an event in the task's history.
 */
static gint
stored_state(ClawtTaskStore *self, const gchar *task_id)
{
    sqlite3_stmt *stmt = NULL;
    gint state = -1;

    if (sqlite3_prepare_v2(self->db, "SELECT state FROM tasks WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        state = sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    return state;
}

static gboolean
exec(ClawtTaskStore *self, const gchar *sql, GError **error)
{
    if (sqlite3_exec(self->db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, sql);
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_task_store_save(ClawtTaskStore  *self,
                      ClawtTask       *task,
                      const gchar     *what,
                      GError         **error)
{
    g_autoptr(JsonNode) node = NULL;
    g_autofree gchar *data = NULL;
    sqlite3_stmt *stmt = NULL;
    const gchar *task_id;
    gint before;
    gint state;
    gboolean ok = FALSE;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);
    g_return_val_if_fail(task != NULL, FALSE);

    task_id = clawt_task_get_id(task);
    state = (gint)clawt_task_get_state(task);
    node = clawt_task_to_json(task);
    data = json_to_string(node, FALSE);

    /*
     * One transaction for the row and its journal entry: a history that
     * says a task moved and a row that says it did not, or the other way
     * round, is worse than either alone.
     */
    if (!exec(self, "BEGIN IMMEDIATE", error))
        return FALSE;

    before = stored_state(self, task_id);

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO tasks (id, state, assignee, origin, parent_id, "
            "  title, created_at, finished_at, data, project) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET state = excluded.state, "
            "  assignee = excluded.assignee, origin = excluded.origin, "
            "  parent_id = excluded.parent_id, title = excluded.title, "
            "  finished_at = excluded.finished_at, data = excluded.data, "
            "  project = excluded.project",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing a task save");
        goto out;
    }

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, state);
    sqlite3_bind_text(stmt, 3, clawt_task_get_assignee(task), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, clawt_task_get_origin(task), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, clawt_task_get_parent_id(task), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, clawt_task_get_title(task), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, clawt_task_get_created_at(task));
    sqlite3_bind_int64(stmt, 8, clawt_task_get_finished_at(task));
    sqlite3_bind_text(stmt, 9, data, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, clawt_task_get_project(task), -1,
                      SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "saving a task");
        goto out;
    }

    g_clear_pointer(&stmt, sqlite3_finalize);

    if (before != state || what != NULL) {
        if (sqlite3_prepare_v2(self->db,
                "INSERT INTO task_journal (task_id, at, state, what) "
                "VALUES (?, ?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK) {
            set_sqlite_error(error, self->db, "preparing a journal row");
            goto out;
        }

        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now_seconds());
        sqlite3_bind_int(stmt, 3, state);
        sqlite3_bind_text(stmt, 4, what, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            set_sqlite_error(error, self->db, "adding a journal row");
            goto out;
        }
    }

    g_clear_pointer(&stmt, sqlite3_finalize);

    if (self->mirror && !outbox_add(self, task_id, "sync", 0, error))
        goto out;

    ok = TRUE;

out:
    g_clear_pointer(&stmt, sqlite3_finalize);

    if (ok)
        ok = exec(self, "COMMIT", error);
    else
        sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);

    return ok;
}

/*
 * A row's `data`, back into a task.  NULL with a warning naming the row
 * when it cannot be read, so one damaged row costs that task and not the
 * list it was in.
 */
static ClawtTask *
task_from_text(const gchar *task_id, const gchar *data)
{
    g_autoptr(JsonNode) node = NULL;
    g_autoptr(GError) error = NULL;
    ClawtTask *task;

    node = json_from_string(data != NULL ? data : "", &error);

    if (node == NULL) {
        g_warning("task store: task %s is not readable JSON (%s); "
                  "skipping it", task_id != NULL ? task_id : "(unnamed)",
                  error != NULL ? error->message : "empty");
        return NULL;
    }

    task = clawt_task_new_from_json(node, &error);

    if (task == NULL)
        g_warning("task store: task %s cannot be restored (%s); skipping it",
                  task_id != NULL ? task_id : "(unnamed)", error->message);

    return task;
}

/* Every row a prepared statement yields, as tasks, in its order. */
static GPtrArray *
collect_tasks(sqlite3_stmt *stmt)
{
    GPtrArray *tasks = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_task_free);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ClawtTask *task = task_from_text(
            (const gchar *)sqlite3_column_text(stmt, 0),
            (const gchar *)sqlite3_column_text(stmt, 1));

        if (task != NULL)
            g_ptr_array_add(tasks, task);
    }

    return tasks;
}

ClawtTask *
clawt_task_store_get(ClawtTaskStore *self, const gchar *task_id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtTask *task = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    if (task_id == NULL)
        return NULL;

    if (sqlite3_prepare_v2(self->db, "SELECT id, data FROM tasks WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        task = task_from_text((const gchar *)sqlite3_column_text(stmt, 0),
                              (const gchar *)sqlite3_column_text(stmt, 1));

    sqlite3_finalize(stmt);
    return task;
}

GPtrArray *
clawt_task_store_load_live(ClawtTaskStore  *self,
                           gint64           finished_since,
                           GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    GPtrArray *tasks;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    /*
     * rowid breaks ties, because timestamps are whole seconds and a
     * fan-out creates several tasks in one: the order they were made is
     * what "oldest first" means.
     */
    if (sqlite3_prepare_v2(self->db,
            "SELECT id, data FROM tasks "
            "WHERE finished_at = 0 OR finished_at >= ? "
            "ORDER BY created_at ASC, rowid ASC", -1, &stmt,
            NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "reading the live tasks");
        return NULL;
    }

    sqlite3_bind_int64(stmt, 1, finished_since);
    tasks = collect_tasks(stmt);
    sqlite3_finalize(stmt);

    return tasks;
}

GPtrArray *
clawt_task_store_list(ClawtTaskStore        *self,
                      const ClawtTaskQuery  *query,
                      GError               **error)
{
    g_autoptr(GString) sql = NULL;
    sqlite3_stmt *stmt = NULL;
    GPtrArray *tasks;
    gint bind = 1;
    guint limit;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    sql = g_string_new("SELECT id, data FROM tasks WHERE 1");

    if (query != NULL && query->assignee != NULL)
        g_string_append(sql, " AND assignee = ?");
    if (query != NULL && query->state >= 0)
        g_string_append(sql, " AND state = ?");
    if (query != NULL && query->before > 0)
        g_string_append(sql, " AND created_at < ?");

    g_string_append(sql, " ORDER BY created_at DESC, rowid DESC LIMIT ?");

    if (sqlite3_prepare_v2(self->db, sql->str, -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "listing tasks");
        return NULL;
    }

    if (query != NULL && query->assignee != NULL)
        sqlite3_bind_text(stmt, bind++, query->assignee, -1, SQLITE_TRANSIENT);
    if (query != NULL && query->state >= 0)
        sqlite3_bind_int(stmt, bind++, query->state);
    if (query != NULL && query->before > 0)
        sqlite3_bind_int64(stmt, bind++, query->before);

    limit = (query != NULL && query->limit > 0) ? query->limit
                                                : CLAWT_TASK_STORE_PAGE;
    sqlite3_bind_int(stmt, bind, (gint)limit);

    tasks = collect_tasks(stmt);
    sqlite3_finalize(stmt);

    return tasks;
}

GPtrArray *
clawt_task_store_project_tasks(ClawtTaskStore  *self,
                               const gchar     *project,
                               gint64           since,
                               GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    GPtrArray *tasks;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);
    g_return_val_if_fail(project != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT id, data FROM tasks WHERE project = ? AND created_at >= ? "
            "ORDER BY created_at",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "listing a project's tasks");
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, since);

    tasks = collect_tasks(stmt);
    sqlite3_finalize(stmt);

    return tasks;
}

JsonNode *
clawt_task_store_journal(ClawtTaskStore  *self,
                         const gchar     *task_id,
                         GError         **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);
    g_return_val_if_fail(task_id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT at, state, what FROM task_journal WHERE task_id = ? "
            "ORDER BY id ASC", -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "reading a task's journal");
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    builder = json_builder_new();
    json_builder_begin_array(builder);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *what = (const gchar *)sqlite3_column_text(stmt, 2);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "at");
        json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 0));
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(builder,
            clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                               sqlite3_column_int(stmt, 1)));

        if (what != NULL) {
            json_builder_set_member_name(builder, "what");
            json_builder_add_string_value(builder, what);
        }

        json_builder_end_object(builder);
    }

    sqlite3_finalize(stmt);
    json_builder_end_array(builder);

    return json_builder_get_root(builder);
}

guint
clawt_task_store_prune(ClawtTaskStore  *self,
                       gint64           finished_before,
                       GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    guint removed = 0;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), 0);

    if (!exec(self, "BEGIN IMMEDIATE", error))
        return 0;

    /* The journals and notes first, while the rows that name them exist. */
    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM task_journal WHERE task_id IN ("
            "  SELECT id FROM tasks WHERE finished_at > 0 "
            "  AND finished_at < ?)", -1, &stmt, NULL) != SQLITE_OK ||
        (sqlite3_bind_int64(stmt, 1, finished_before),
         sqlite3_step(stmt) != SQLITE_DONE)) {
        set_sqlite_error(error, self->db, "pruning task journals");
        goto fail;
    }

    g_clear_pointer(&stmt, sqlite3_finalize);

    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM task_notes WHERE task_id IN ("
            "  SELECT id FROM tasks WHERE finished_at > 0 "
            "  AND finished_at < ?)", -1, &stmt, NULL) != SQLITE_OK ||
        (sqlite3_bind_int64(stmt, 1, finished_before),
         sqlite3_step(stmt) != SQLITE_DONE)) {
        set_sqlite_error(error, self->db, "pruning task notes");
        goto fail;
    }

    g_clear_pointer(&stmt, sqlite3_finalize);

    /* Links and held deliveries that name a task about to go. */
    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM task_links WHERE blocker IN ("
            "  SELECT id FROM tasks WHERE finished_at > 0 AND finished_at < ?)"
            " OR blocked IN ("
            "  SELECT id FROM tasks WHERE finished_at > 0 AND finished_at < ?)",
            -1, &stmt, NULL) != SQLITE_OK ||
        (sqlite3_bind_int64(stmt, 1, finished_before),
         sqlite3_bind_int64(stmt, 2, finished_before),
         sqlite3_step(stmt) != SQLITE_DONE)) {
        set_sqlite_error(error, self->db, "pruning task links");
        goto fail;
    }

    g_clear_pointer(&stmt, sqlite3_finalize);

    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM tasks WHERE finished_at > 0 AND finished_at < ?",
            -1, &stmt, NULL) != SQLITE_OK ||
        (sqlite3_bind_int64(stmt, 1, finished_before),
         sqlite3_step(stmt) != SQLITE_DONE)) {
        set_sqlite_error(error, self->db, "pruning tasks");
        goto fail;
    }

    removed = (guint)sqlite3_changes(self->db);
    g_clear_pointer(&stmt, sqlite3_finalize);

    if (!exec(self, "COMMIT", error))
        return 0;

    return removed;

fail:
    g_clear_pointer(&stmt, sqlite3_finalize);
    sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
    return 0;
}

gint64
clawt_task_store_add_note(ClawtTaskStore  *self,
                          const gchar     *task_id,
                          const gchar     *author,
                          const gchar     *kind,
                          const gchar     *body,
                          GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    gint64 id = 0;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), 0);
    g_return_val_if_fail(task_id != NULL, 0);
    g_return_val_if_fail(kind != NULL, 0);
    g_return_val_if_fail(body != NULL, 0);

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO task_notes (task_id, at, author, kind, body) "
            "VALUES (?, ?, ?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing a task note");
        return 0;
    }

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, now_seconds());
    sqlite3_bind_text(stmt, 3, author, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, body, -1, SQLITE_TRANSIENT);

    /* The note and the mirror's row for it, together or not at all. */
    if (self->mirror && !exec(self, "BEGIN IMMEDIATE", error)) {
        sqlite3_finalize(stmt);
        return 0;
    }

    if (sqlite3_step(stmt) == SQLITE_DONE)
        id = sqlite3_last_insert_rowid(self->db);
    else
        set_sqlite_error(error, self->db, "adding a task note");

    sqlite3_finalize(stmt);

    if (self->mirror) {
        if (id > 0 && outbox_add(self, task_id, "note", id, error)) {
            if (!exec(self, "COMMIT", error))
                id = 0;
        } else {
            sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
            id = 0;
        }
    }

    return id;
}

JsonNode *
clawt_task_store_notes(ClawtTaskStore  *self,
                       const gchar     *task_id,
                       GError         **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);
    g_return_val_if_fail(task_id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT id, at, author, kind, body FROM task_notes "
            "WHERE task_id = ? ORDER BY id ASC", -1, &stmt,
            NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "reading a task's notes");
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    builder = json_builder_new();
    json_builder_begin_array(builder);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *author = (const gchar *)sqlite3_column_text(stmt, 2);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 0));
        json_builder_set_member_name(builder, "at");
        json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 1));

        if (author != NULL) {
            json_builder_set_member_name(builder, "author");
            json_builder_add_string_value(builder, author);
        }

        json_builder_set_member_name(builder, "kind");
        json_builder_add_string_value(builder,
            (const gchar *)sqlite3_column_text(stmt, 3));
        json_builder_set_member_name(builder, "body");
        json_builder_add_string_value(builder,
            (const gchar *)sqlite3_column_text(stmt, 4));
        json_builder_end_object(builder);
    }

    sqlite3_finalize(stmt);
    json_builder_end_array(builder);

    return json_builder_get_root(builder);
}

/* One statement with up to two text parameters and no rows wanted. */
static gboolean
run_text2(ClawtTaskStore *self, const gchar *sql, const gchar *a,
          const gchar *b, GError **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, sql);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, a, -1, SQLITE_TRANSIENT);

    if (b != NULL)
        sqlite3_bind_text(stmt, 2, b, -1, SQLITE_TRANSIENT);

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, sql);

    sqlite3_finalize(stmt);
    return ok;
}

/* Every value of column 0 a one-parameter query yields. */
static GPtrArray *
ids_for(ClawtTaskStore *self, const gchar *sql, const gchar *value)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return out;

    sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, g_strdup(
            (const gchar *)sqlite3_column_text(stmt, 0)));

    sqlite3_finalize(stmt);
    return out;
}

gboolean
clawt_task_store_add_link(ClawtTaskStore  *self,
                          const gchar     *blocker,
                          const gchar     *blocked,
                          GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);
    g_return_val_if_fail(blocker != NULL && blocked != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
            "INSERT OR IGNORE INTO task_links (blocker, blocked, created_at) "
            "VALUES (?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing a task link");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, blocker, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, blocked, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, now_seconds());
    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, "adding a task link");

    sqlite3_finalize(stmt);
    return ok;
}

gboolean
clawt_task_store_remove_link(ClawtTaskStore  *self,
                             const gchar     *blocker,
                             const gchar     *blocked,
                             GError         **error)
{
    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);

    return run_text2(self,
                     "DELETE FROM task_links WHERE blocker = ? AND blocked = ?",
                     blocker, blocked, error);
}

GPtrArray *
clawt_task_store_blockers_of(ClawtTaskStore *self, const gchar *task_id)
{
    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    return ids_for(self,
                   "SELECT blocker FROM task_links WHERE blocked = ? "
                   "ORDER BY created_at, rowid", task_id);
}

GPtrArray *
clawt_task_store_blocked_by(ClawtTaskStore *self, const gchar *task_id)
{
    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    return ids_for(self,
                   "SELECT blocked FROM task_links WHERE blocker = ? "
                   "ORDER BY created_at, rowid", task_id);
}

gboolean
clawt_task_store_hold_delivery(ClawtTaskStore  *self,
                               const gchar     *task_id,
                               JsonNode        *delivery,
                               GError         **error)
{
    g_autofree gchar *text = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);
    g_return_val_if_fail(task_id != NULL && delivery != NULL, FALSE);

    text = json_to_string(delivery, FALSE);

    return run_text2(self,
                     "INSERT OR REPLACE INTO held_deliveries (task_id, data) "
                     "VALUES (?, ?)", task_id, text, error);
}

JsonNode *
clawt_task_store_take_delivery(ClawtTaskStore *self, const gchar *task_id)
{
    sqlite3_stmt *stmt = NULL;
    JsonNode *node = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    if (task_id == NULL ||
        sqlite3_prepare_v2(self->db,
            "SELECT data FROM held_deliveries WHERE task_id = ?", -1, &stmt,
            NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        node = json_from_string((const gchar *)sqlite3_column_text(stmt, 0),
                                NULL);

    sqlite3_finalize(stmt);

    /* Taken, not read: a delivery released twice is work done twice. */
    if (node != NULL)
        run_text2(self, "DELETE FROM held_deliveries WHERE task_id = ?",
                  task_id, NULL, NULL);

    return node;
}

gboolean
clawt_task_store_has_delivery(ClawtTaskStore *self, const gchar *task_id)
{
    g_autoptr(GPtrArray) found = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);

    found = ids_for(self,
                    "SELECT task_id FROM held_deliveries WHERE task_id = ?",
                    task_id);
    return found->len > 0;
}

gboolean
clawt_task_store_save_incident(ClawtTaskStore  *self,
                               const gchar     *incident_id,
                               gint64           opened_at,
                               JsonNode        *data,
                               GError         **error)
{
    g_autofree gchar *text = NULL;
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);
    g_return_val_if_fail(incident_id != NULL, FALSE);
    g_return_val_if_fail(data != NULL, FALSE);

    text = json_to_string(data, FALSE);

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO incidents (id, opened_at, data) VALUES (?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET opened_at = excluded.opened_at, "
            "  data = excluded.data", -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing an incident save");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, incident_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, opened_at);
    sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, "saving an incident");

    sqlite3_finalize(stmt);
    return ok;
}

gboolean
clawt_task_store_delete_incident(ClawtTaskStore  *self,
                                 const gchar     *incident_id,
                                 GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);
    g_return_val_if_fail(incident_id != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db, "DELETE FROM incidents WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing an incident delete");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, incident_id, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, "deleting an incident");

    sqlite3_finalize(stmt);
    return ok;
}

GPtrArray *
clawt_task_store_load_incidents(ClawtTaskStore  *self,
                                GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    GPtrArray *out;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT id, data FROM incidents ORDER BY opened_at ASC, "
            "rowid ASC", -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "reading incidents");
        return NULL;
    }

    out = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_unref);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        g_autoptr(GError) parse_error = NULL;
        JsonNode *node = json_from_string(
            (const gchar *)sqlite3_column_text(stmt, 1), &parse_error);

        if (node == NULL) {
            g_warning("task store: incident %s is not readable JSON (%s); "
                      "skipping it",
                      (const gchar *)sqlite3_column_text(stmt, 0),
                      parse_error != NULL ? parse_error->message : "empty");
            continue;
        }

        g_ptr_array_add(out, node);
    }

    sqlite3_finalize(stmt);
    return out;
}

/* ── The VENTURE mirror's outbox and refs (0.44.0) ──────────────── */

void
clawt_task_store_set_mirror(ClawtTaskStore *self, gboolean mirror)
{
    g_return_if_fail(CLAWT_IS_TASK_STORE(self));

    self->mirror = mirror;
}

gboolean
clawt_task_store_get_mirror(ClawtTaskStore *self)
{
    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);

    return self->mirror;
}

void
clawt_venture_outbox_row_free(ClawtVentureOutboxRow *row)
{
    if (row == NULL)
        return;

    g_free(row->task_id);
    g_free(row->kind);
    g_free(row);
}

void
clawt_venture_ref_free(ClawtVentureRef *ref)
{
    if (ref == NULL)
        return;

    g_free(ref->task_id);
    g_free(ref->instance);
    g_free(ref->snapshot);
    g_free(ref->state);
    g_free(ref->reason);
    g_free(ref->url);
    g_free(ref);
}

GPtrArray *
clawt_task_store_outbox_due(ClawtTaskStore  *self,
                            gint64           now,
                            guint            limit,
                            GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    GPtrArray *rows;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    /*
     * The oldest queued row of each task, and only when it is due: FIFO
     * per task, so a comment never reaches VENTURE before the ticket it
     * belongs to, and a task waiting out a backoff holds its own later
     * rows without holding anybody else's.
     */
    if (sqlite3_prepare_v2(self->db,
            "SELECT o.id, o.task_id, o.kind, o.note_id, o.attempts, o.gen "
            "FROM venture_outbox o "
            "WHERE o.state = 0 AND o.next_at <= ? AND o.id = "
            "  (SELECT MIN(i.id) FROM venture_outbox i "
            "   WHERE i.task_id = o.task_id AND i.state = 0) "
            "ORDER BY o.id LIMIT ?", -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "reading the outbox");
        return NULL;
    }

    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int(stmt, 2, (gint)limit);

    rows = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_venture_outbox_row_free);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ClawtVentureOutboxRow *row = g_new0(ClawtVentureOutboxRow, 1);

        row->id = sqlite3_column_int64(stmt, 0);
        row->task_id = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        row->kind = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        row->note_id = sqlite3_column_int64(stmt, 3);
        row->attempts = sqlite3_column_int(stmt, 4);
        row->gen = sqlite3_column_int64(stmt, 5);
        g_ptr_array_add(rows, row);
    }

    sqlite3_finalize(stmt);
    return rows;
}

/*
 * One UPDATE over one outbox row.  @sql binds, in order, @a and @b when
 * @with_numbers, then @text, then the row's @id.
 */
static gboolean
outbox_update(ClawtTaskStore *self, const gchar *sql, gboolean with_numbers,
              gint64 a, gint64 b, const gchar *text, gint64 id)
{
    sqlite3_stmt *stmt = NULL;
    gint n = 1;
    gboolean ok;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    if (with_numbers) {
        sqlite3_bind_int64(stmt, n++, a);
        sqlite3_bind_int64(stmt, n++, b);
    }

    sqlite3_bind_text(stmt, n++, text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, n, id);

    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
clawt_task_store_outbox_finish(ClawtTaskStore *self, gint64 id, gint64 gen)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);

    /*
     * Done only if nothing changed since it was read: a `sync` whose
     * generation moved on is the newer change, still owed, so it goes
     * back to due-now rather than being deleted.
     */
    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM venture_outbox WHERE id = ? AND gen = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, gen);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    if (ok && sqlite3_changes(self->db) == 0)
        ok = outbox_update(self,
            "UPDATE venture_outbox SET attempts = ?, next_at = ?, "
            "  error = ? WHERE id = ?", TRUE,
            0, g_get_real_time() / G_USEC_PER_SEC, NULL, id);

    return ok;
}

gboolean
clawt_task_store_outbox_retry(ClawtTaskStore *self, gint64 id,
                              gint attempts, gint64 next_at,
                              const gchar *why)
{
    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);

    return outbox_update(self,
        "UPDATE venture_outbox SET attempts = ?, next_at = ?, error = ? "
        "WHERE id = ?", TRUE, attempts, next_at, why, id);
}

gboolean
clawt_task_store_outbox_kill(ClawtTaskStore *self, gint64 id,
                             const gchar *why)
{
    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);

    return outbox_update(self,
        "UPDATE venture_outbox SET state = 2, error = ? WHERE id = ?",
        FALSE, 0, 0, why, id);
}

guint
clawt_task_store_outbox_revive(ClawtTaskStore *self, const gchar *task_id)
{
    sqlite3_stmt *stmt = NULL;
    guint revived = 0;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), 0);
    g_return_val_if_fail(task_id != NULL, 0);

    /*
     * Dead rows back to queued, oldest first.  OR IGNORE: a dead `sync`
     * whose task has since queued a newer one is already covered by it,
     * and is dropped instead.
     */
    if (sqlite3_prepare_v2(self->db,
            "UPDATE OR IGNORE venture_outbox SET state = 0, attempts = 0, "
            "  next_at = ?, error = NULL WHERE task_id = ? AND state = 2",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        revived = (guint)sqlite3_changes(self->db);

    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM venture_outbox WHERE task_id = ? AND state = 2",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return revived;
}

void
clawt_task_store_outbox_state(ClawtTaskStore  *self,
                              const gchar     *task_id,
                              guint           *queued,
                              guint           *dead,
                              gchar          **last_error)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_TASK_STORE(self));

    if (queued != NULL)
        *queued = 0;
    if (dead != NULL)
        *dead = 0;
    if (last_error != NULL)
        *last_error = NULL;

    if (sqlite3_prepare_v2(self->db,
            "SELECT state, error FROM venture_outbox WHERE task_id = ? "
            "ORDER BY id", -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint state = sqlite3_column_int(stmt, 0);
        const gchar *why = (const gchar *)sqlite3_column_text(stmt, 1);

        if (state == 2 && dead != NULL)
            (*dead)++;
        else if (state == 0 && queued != NULL)
            (*queued)++;

        if (why != NULL && last_error != NULL) {
            g_free(*last_error);
            *last_error = g_strdup(why);
        }
    }

    sqlite3_finalize(stmt);
}

ClawtVentureRef *
clawt_task_store_venture_ref(ClawtTaskStore *self, const gchar *task_id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtVentureRef *ref = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);
    g_return_val_if_fail(task_id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT instance, ticket_id, version, snapshot, state, reason, "
            "  url, updated_at, organization_id FROM venture_refs "
            "WHERE task_id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ref = g_new0(ClawtVentureRef, 1);
        ref->task_id = g_strdup(task_id);
        ref->instance = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        ref->ticket_id = sqlite3_column_int64(stmt, 1);
        ref->version = sqlite3_column_int64(stmt, 2);
        ref->snapshot = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
        ref->state = g_strdup((const gchar *)sqlite3_column_text(stmt, 4));
        ref->reason = g_strdup((const gchar *)sqlite3_column_text(stmt, 5));
        ref->url = g_strdup((const gchar *)sqlite3_column_text(stmt, 6));
        ref->updated_at = sqlite3_column_int64(stmt, 7);
        ref->organization_id = sqlite3_column_int64(stmt, 8);
    }

    sqlite3_finalize(stmt);
    return ref;
}

gboolean
clawt_task_store_put_venture_ref(ClawtTaskStore        *self,
                                 const ClawtVentureRef *ref,
                                 GError               **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);
    g_return_val_if_fail(ref != NULL && ref->task_id != NULL, FALSE);
    g_return_val_if_fail(ref->state != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
            "INSERT OR REPLACE INTO venture_refs (task_id, instance, "
            "  ticket_id, version, snapshot, state, reason, url, updated_at, "
            "  organization_id) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt, NULL)
        != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing a VENTURE ref");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, ref->task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ref->instance, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, ref->ticket_id);
    sqlite3_bind_int64(stmt, 4, ref->version);
    sqlite3_bind_text(stmt, 5, ref->snapshot, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, ref->state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, ref->reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, ref->url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 9, now_seconds());
    sqlite3_bind_int64(stmt, 10, ref->organization_id);

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, "saving a VENTURE ref");

    sqlite3_finalize(stmt);
    return ok;
}

gchar *
clawt_task_store_venture_cursor(ClawtTaskStore *self, const gchar *key)
{
    sqlite3_stmt *stmt = NULL;
    gchar *value = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT value FROM venture_cursors WHERE key = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        value = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));

    sqlite3_finalize(stmt);
    return value;
}

void
clawt_task_store_put_venture_cursor(ClawtTaskStore *self, const gchar *key,
                                    const gchar *value)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_TASK_STORE(self));

    if (sqlite3_prepare_v2(self->db,
            "INSERT OR REPLACE INTO venture_cursors (key, value) "
            "VALUES (?, ?)", -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

GHashTable *
clawt_task_store_team_selinux_levels(ClawtTaskStore *self)
{
    sqlite3_stmt *stmt = NULL;
    GHashTable *levels;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    levels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    if (sqlite3_prepare_v2(self->db,
            "SELECT team, level FROM team_selinux_levels",
            -1, &stmt, NULL) != SQLITE_OK)
        return levels;

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_hash_table_insert(
            levels, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)),
            g_strdup((const gchar *)sqlite3_column_text(stmt, 1)));

    sqlite3_finalize(stmt);
    return levels;
}

gchar *
clawt_task_store_team_selinux_level(ClawtTaskStore *self, const gchar *team)
{
    sqlite3_stmt *stmt = NULL;
    gchar *level = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);
    g_return_val_if_fail(team != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT level FROM team_selinux_levels WHERE team = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, team, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        level = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));

    sqlite3_finalize(stmt);
    return level;
}

gboolean
clawt_task_store_add_team_selinux_level(ClawtTaskStore  *self,
                                        const gchar     *team,
                                        const gchar     *level,
                                        GError         **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);
    g_return_val_if_fail(team != NULL, FALSE);
    g_return_val_if_fail(level != NULL, FALSE);

    /*
     * A plain INSERT, never OR REPLACE: a team's level is not changed by
     * allocating again, and a level some other team already holds is a
     * conflict to report rather than a row to overwrite.
     */
    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO team_selinux_levels (team, level, created_at) "
            "VALUES (?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "storing a team's SELinux level");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, team, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, level, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, g_get_real_time() / G_USEC_PER_SEC);

    ok = sqlite3_step(stmt) == SQLITE_DONE;

    if (!ok)
        set_sqlite_error(error, self->db, "storing a team's SELinux level");

    sqlite3_finalize(stmt);
    return ok;
}

GHashTable *
clawt_task_store_venture_live_tickets(ClawtTaskStore *self)
{
    sqlite3_stmt *stmt = NULL;
    GHashTable *live = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), live);

    /* Mirrored tasks still going: the tickets whose comments matter. */
    if (sqlite3_prepare_v2(self->db,
            "SELECT r.ticket_id, r.task_id FROM venture_refs r "
            "JOIN tasks t ON t.id = r.task_id "
            "WHERE r.ticket_id > 0 AND r.state = 'synced' "
            "  AND t.finished_at = 0", -1, &stmt, NULL) != SQLITE_OK)
        return live;

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_hash_table_replace(live,
            g_strdup_printf("%" G_GINT64_FORMAT, (gint64)sqlite3_column_int64(stmt, 0)),
            g_strdup((const gchar *)sqlite3_column_text(stmt, 1)));

    sqlite3_finalize(stmt);
    return live;
}

gchar *
clawt_task_store_venture_task_for_ticket(ClawtTaskStore *self,
                                         gint64          ticket_id)
{
    sqlite3_stmt *stmt = NULL;
    gchar *task_id = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT task_id FROM venture_refs WHERE ticket_id = ? LIMIT 1",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_int64(stmt, 1, ticket_id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        task_id = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));

    sqlite3_finalize(stmt);
    return task_id;
}

JsonNode *
clawt_task_store_note(ClawtTaskStore *self, gint64 note_id)
{
    sqlite3_stmt *stmt = NULL;
    JsonNode *node = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT task_id, at, author, kind, body FROM task_notes "
            "WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_int64(stmt, 1, note_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        const gchar *author = (const gchar *)sqlite3_column_text(stmt, 2);

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id");
        json_builder_add_int_value(b, note_id);
        json_builder_set_member_name(b, "task_id");
        json_builder_add_string_value(b,
            (const gchar *)sqlite3_column_text(stmt, 0));
        json_builder_set_member_name(b, "at");
        json_builder_add_int_value(b, sqlite3_column_int64(stmt, 1));
        if (author != NULL) {
            json_builder_set_member_name(b, "author");
            json_builder_add_string_value(b, author);
        }
        json_builder_set_member_name(b, "kind");
        json_builder_add_string_value(b,
            (const gchar *)sqlite3_column_text(stmt, 3));
        json_builder_set_member_name(b, "body");
        json_builder_add_string_value(b,
            (const gchar *)sqlite3_column_text(stmt, 4));
        json_builder_end_object(b);
        node = json_builder_get_root(b);
    }

    sqlite3_finalize(stmt);
    return node;
}

gint64
clawt_task_store_venture_comment(ClawtTaskStore *self, gint64 note_id)
{
    sqlite3_stmt *stmt = NULL;
    gint64 comment = 0;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), 0);

    if (sqlite3_prepare_v2(self->db,
            "SELECT comment_id FROM venture_note_refs WHERE note_id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, note_id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        comment = sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    return comment;
}

gboolean
clawt_task_store_put_venture_comment(ClawtTaskStore *self, gint64 note_id,
                                     const gchar *task_id, gint64 comment_id)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(self), FALSE);

    if (sqlite3_prepare_v2(self->db,
            "INSERT OR REPLACE INTO venture_note_refs "
            "  (note_id, task_id, comment_id) VALUES (?, ?, ?)",
            -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_int64(stmt, 1, note_id);
    sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, comment_id);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static void
clawt_task_store_finalize(GObject *object)
{
    ClawtTaskStore *self = CLAWT_TASK_STORE(object);

    if (self->db != NULL)
        sqlite3_close_v2(self->db);

    g_free(self->path);

    G_OBJECT_CLASS(clawt_task_store_parent_class)->finalize(object);
}

static void
clawt_task_store_class_init(ClawtTaskStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_task_store_finalize;
}

static void
clawt_task_store_init(ClawtTaskStore *self)
{
    (void)self;
}
