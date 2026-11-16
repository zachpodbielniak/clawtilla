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
    "CREATE INDEX IF NOT EXISTS incidents_opened ON incidents(opened_at);";

struct _ClawtTaskStore {
    GObject  parent_instance;
    sqlite3 *db;
    gchar   *path;
};

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

    if (sqlite3_step(stmt) == SQLITE_DONE)
        id = sqlite3_last_insert_rowid(self->db);
    else
        set_sqlite_error(error, self->db, "adding a task note");

    sqlite3_finalize(stmt);
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
