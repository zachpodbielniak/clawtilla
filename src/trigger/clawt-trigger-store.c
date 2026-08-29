/*
 * clawt-trigger-store.c - What a trigger has been sent, and what came of it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "trigger/clawt-trigger-store.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

/*
 * WAL and a busy timeout for the same reason the mailbox has them: the
 * ingress writes a receipt from a request while the IPC surface is
 * reading the listing, and a reader that gives up with SQLITE_BUSY
 * reports "no deliveries", which is the answer this store exists to stop
 * being wrong about.
 */
static const gchar SCHEMA_SQL[] =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS endpoints ("
    "  trigger_id TEXT PRIMARY KEY,"
    "  endpoint   TEXT NOT NULL UNIQUE,"
    "  verified   INTEGER NOT NULL DEFAULT 0,"
    "  capture    TEXT,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS deliveries ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  trigger_id  TEXT NOT NULL,"
    "  delivery_id TEXT,"
    "  event_name  TEXT,"
    "  repo        TEXT,"
    "  branch      TEXT,"
    "  actor       TEXT,"
    "  outcome     INTEGER NOT NULL,"
    "  detail      TEXT,"
    "  task_id     TEXT,"
    "  finished    INTEGER NOT NULL DEFAULT 0,"
    "  created_at  INTEGER NOT NULL"
    ");"
    /*
     * The uniqueness that makes a retry idempotent, and the reason it is
     * partial: a sender that gives no delivery id tells us nothing about
     * whether this is a retry, and a plain unique index would then treat
     * every such delivery after the first as a duplicate and drop it.
     */
    "CREATE UNIQUE INDEX IF NOT EXISTS deliveries_unique"
    "  ON deliveries(trigger_id, delivery_id)"
    "  WHERE delivery_id IS NOT NULL AND delivery_id <> '';"
    "CREATE INDEX IF NOT EXISTS deliveries_by_time"
    "  ON deliveries(trigger_id, created_at);";

#define BUSY_TIMEOUT_MS (5000)

struct _ClawtTriggerStore {
    GObject parent_instance;

    sqlite3 *db;
    gchar   *path;
};

G_DEFINE_FINAL_TYPE(ClawtTriggerStore, clawt_trigger_store, G_TYPE_OBJECT)

GType
clawt_delivery_outcome_get_type(void)
{
    static gsize type_id = 0;

    if (g_once_init_enter(&type_id)) {
        static const GEnumValue values[] = {
            { CLAWT_DELIVERY_RAN, "CLAWT_DELIVERY_RAN", "ran" },
            { CLAWT_DELIVERY_DUPLICATE, "CLAWT_DELIVERY_DUPLICATE",
              "duplicate" },
            { CLAWT_DELIVERY_IGNORED, "CLAWT_DELIVERY_IGNORED", "ignored" },
            { CLAWT_DELIVERY_CAPTURED, "CLAWT_DELIVERY_CAPTURED",
              "captured" },
            { CLAWT_DELIVERY_REFUSED, "CLAWT_DELIVERY_REFUSED", "refused" },
            { CLAWT_DELIVERY_FAILED, "CLAWT_DELIVERY_FAILED", "failed" },
            { 0, NULL, NULL }
        };
        GType id = g_enum_register_static("ClawtDeliveryOutcome", values);

        g_once_init_leave(&type_id, id);
    }

    return type_id;
}

static void
set_sqlite_error(GError **error, sqlite3 *db, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s", what,
                db != NULL ? sqlite3_errmsg(db) : "no database");
}

/*
 * Whether a column is already on a table.
 *
 * Asked rather than inferred from a failed ALTER.  "Add it and ignore
 * the error" cannot tell a column that is already there from a database
 * that is unwritable, and the second has to stay an error -- CREATE
 * TABLE IF NOT EXISTS does nothing at all to a file that already has the
 * table, so every column added after the first release reaches new
 * databases only.
 */
static gboolean
has_column(sqlite3 *db, const gchar *table, const gchar *column)
{
    g_autofree gchar *sql = NULL;
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    /*
     * PRAGMA takes no parameters, so the table name is spliced in.  It
     * is a literal at every call site here and never reaches this from a
     * config file or a delivery.
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
 * One function so that a column added later reaches a database that
 * already exists.  Skipping this is how a fleet's mailboxes were all
 * quarantined as corrupt on an upgrade; a trigger store failing the same
 * way would lose every receipt and every dedup key at once, so every
 * retry queued behind the upgrade would run a second time.
 */
static gboolean
apply_schema(sqlite3 *db, GError **error)
{
    if (db == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the trigger database is not open");
        return FALSE;
    }

    if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, db, "creating the trigger schema");
        return FALSE;
    }

    /*
     * Nothing has been added since 0.2.0 yet.  The check is here rather
     * than added with the first new column, because the migration that
     * is written when it is needed is the one that gets forgotten -- and
     * has_column() failing loudly on an unwritable file is the whole
     * value of the pattern.
     */
    if (!has_column(db, "deliveries", "finished")) {
        if (sqlite3_exec(db,
                         "ALTER TABLE deliveries ADD COLUMN finished"
                         " INTEGER NOT NULL DEFAULT 0",
                         NULL, NULL, NULL) != SQLITE_OK) {
            set_sqlite_error(error, db, "adding finished to the deliveries");
            return FALSE;
        }
    }

    return TRUE;
}

static void
clawt_trigger_store_dispose(GObject *object)
{
    ClawtTriggerStore *self = CLAWT_TRIGGER_STORE(object);

    /*
     * _v2, not the plain form: sqlite3_close() refuses with SQLITE_BUSY
     * while anything is outstanding and leaves the whole connection --
     * page cache included -- allocated.
     */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    g_clear_pointer(&self->path, g_free);

    G_OBJECT_CLASS(clawt_trigger_store_parent_class)->dispose(object);
}

static void
clawt_trigger_store_class_init(ClawtTriggerStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_trigger_store_dispose;
}

static void
clawt_trigger_store_init(ClawtTriggerStore *self)
{
    (void)self;
}

ClawtTriggerStore *
clawt_trigger_store_new(const gchar *path, GError **error)
{
    ClawtTriggerStore *self;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    self = g_object_new(CLAWT_TYPE_TRIGGER_STORE, NULL);
    self->path = g_strdup(path);

    if (sqlite3_open(self->path, &self->db) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "opening the trigger database");

        /*
         * sqlite3_open leaves a usable handle even when it fails -- that
         * is how sqlite3_errmsg() works on it -- so the object is
         * dropped rather than reused, and dispose closes it properly.
         */
        g_object_unref(self);
        return NULL;
    }

    sqlite3_busy_timeout(self->db, BUSY_TIMEOUT_MS);

    if (!apply_schema(self->db, error)) {
        g_object_unref(self);
        return NULL;
    }

    /* Deliveries name repositories and people; nobody else reads them. */
    g_chmod(self->path, 0600);

    return self;
}

/* ── Endpoints ───────────────────────────────────────────────────── */

gchar *
clawt_trigger_store_endpoint_for(ClawtTriggerStore  *self,
                                 const gchar        *trigger_id,
                                 gboolean            create,
                                 GError            **error)
{
    sqlite3_stmt *stmt = NULL;
    gchar *endpoint = NULL;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), NULL);
    g_return_val_if_fail(trigger_id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT endpoint FROM endpoints"
                           " WHERE trigger_id = ?",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW)
            endpoint = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));

        sqlite3_finalize(stmt);
    }

    if (endpoint != NULL || !create)
        return endpoint;

    endpoint = clawt_trigger_endpoint_new(error);

    if (endpoint == NULL)
        return NULL;

    stmt = NULL;

    if (sqlite3_prepare_v2(self->db,
                           "INSERT INTO endpoints"
                           " (trigger_id, endpoint, verified, created_at)"
                           " VALUES (?, ?, 0, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "recording the endpoint");
        g_free(endpoint);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, endpoint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, g_get_real_time() / G_USEC_PER_SEC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "recording the endpoint");
        sqlite3_finalize(stmt);
        g_free(endpoint);
        return NULL;
    }

    sqlite3_finalize(stmt);

    return endpoint;
}

gchar *
clawt_trigger_store_trigger_for_endpoint(ClawtTriggerStore *self,
                                         const gchar       *endpoint)
{
    sqlite3_stmt *stmt = NULL;
    gchar *trigger_id = NULL;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), NULL);

    if (endpoint == NULL || *endpoint == '\0')
        return NULL;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT trigger_id FROM endpoints"
                           " WHERE endpoint = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, endpoint, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        trigger_id = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));

    sqlite3_finalize(stmt);

    return trigger_id;
}

gchar *
clawt_trigger_store_rotate_endpoint(ClawtTriggerStore  *self,
                                    const gchar        *trigger_id,
                                    GError            **error)
{
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *endpoint = NULL;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), NULL);
    g_return_val_if_fail(trigger_id != NULL, NULL);

    endpoint = clawt_trigger_endpoint_new(error);

    if (endpoint == NULL)
        return NULL;

    /*
     * Replace rather than update, so a trigger with no row yet gets one.
     * Verification goes back to pending: the address changed, so the
     * next delivery is from a registration nobody has seen work.
     */
    if (sqlite3_prepare_v2(self->db,
                           "INSERT OR REPLACE INTO endpoints"
                           " (trigger_id, endpoint, verified, capture,"
                           "  created_at)"
                           " VALUES (?, ?, 0, NULL, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "rotating the endpoint");
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, endpoint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, g_get_real_time() / G_USEC_PER_SEC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "rotating the endpoint");
        sqlite3_finalize(stmt);
        return NULL;
    }

    sqlite3_finalize(stmt);

    return g_steal_pointer(&endpoint);
}

/* ── The handshake ───────────────────────────────────────────────── */

gboolean
clawt_trigger_store_is_pending_verification(ClawtTriggerStore *self,
                                            const gchar       *trigger_id)
{
    sqlite3_stmt *stmt = NULL;
    gboolean pending = TRUE;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), FALSE);
    g_return_val_if_fail(trigger_id != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT verified FROM endpoints"
                           " WHERE trigger_id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return TRUE;

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        pending = sqlite3_column_int(stmt, 0) == 0;

    sqlite3_finalize(stmt);

    return pending;
}

gboolean
clawt_trigger_store_capture(ClawtTriggerStore  *self,
                            const gchar        *trigger_id,
                            ClawtTriggerEvent  *event,
                            GError            **error)
{
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), FALSE);
    g_return_val_if_fail(trigger_id != NULL, FALSE);
    g_return_val_if_fail(event != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db,
                           "UPDATE endpoints SET verified = 1, capture = ?"
                           " WHERE trigger_id = ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "capturing the first delivery");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, clawt_trigger_event_get_payload(event), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, trigger_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "capturing the first delivery");
        sqlite3_finalize(stmt);
        return FALSE;
    }

    sqlite3_finalize(stmt);

    return TRUE;
}

gchar *
clawt_trigger_store_get_capture(ClawtTriggerStore *self,
                                const gchar       *trigger_id)
{
    sqlite3_stmt *stmt = NULL;
    gchar *capture = NULL;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), NULL);
    g_return_val_if_fail(trigger_id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT capture FROM endpoints"
                           " WHERE trigger_id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        capture = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));

    sqlite3_finalize(stmt);

    return capture;
}

void
clawt_trigger_store_reset_verification(ClawtTriggerStore *self,
                                       const gchar       *trigger_id)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_TRIGGER_STORE(self));
    g_return_if_fail(trigger_id != NULL);

    if (sqlite3_prepare_v2(self->db,
                           "UPDATE endpoints SET verified = 0,"
                           " capture = NULL WHERE trigger_id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* ── Deliveries ──────────────────────────────────────────────────── */

gboolean
clawt_trigger_store_seen_delivery(ClawtTriggerStore *self,
                                  const gchar       *trigger_id,
                                  const gchar       *delivery_id)
{
    sqlite3_stmt *stmt = NULL;
    gboolean seen = FALSE;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), FALSE);
    g_return_val_if_fail(trigger_id != NULL, FALSE);

    /*
     * No id is not a duplicate.  A sender that names no delivery has
     * told us nothing about whether this is a retry, and reading "we
     * cannot tell" as "already done" would silently drop real work --
     * which for a generic caller is every delivery after the first.
     */
    if (delivery_id == NULL || *delivery_id == '\0')
        return FALSE;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT 1 FROM deliveries"
                           " WHERE trigger_id = ? AND delivery_id = ?"
                           " LIMIT 1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, delivery_id, -1, SQLITE_TRANSIENT);

    seen = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);

    return seen;
}

void
clawt_trigger_store_record(ClawtTriggerStore    *self,
                           const gchar          *trigger_id,
                           ClawtTriggerEvent    *event,
                           ClawtDeliveryOutcome  outcome,
                           const gchar          *detail,
                           const gchar          *task_id)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_TRIGGER_STORE(self));
    g_return_if_fail(trigger_id != NULL);

    /*
     * OR IGNORE, so a receipt that collides with the partial unique
     * index is dropped rather than failing the request.  The collision
     * *is* the duplicate check having already answered; writing a second
     * row for the same delivery would make a retry look like two.
     */
    if (sqlite3_prepare_v2(self->db,
                           "INSERT OR IGNORE INTO deliveries"
                           " (trigger_id, delivery_id, event_name, repo,"
                           "  branch, actor, outcome, detail, task_id,"
                           "  finished, created_at)"
                           " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        g_warning("triggers: a receipt for '%s' could not be written: %s",
                  trigger_id, sqlite3_errmsg(self->db));
        return;
    }

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2,
                      event != NULL
                          ? clawt_trigger_event_get_delivery_id(event)
                          : NULL,
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3,
                      event != NULL ? clawt_trigger_event_get_name(event)
                                    : NULL,
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4,
                      event != NULL ? clawt_trigger_event_get_repo(event)
                                    : NULL,
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5,
                      event != NULL ? clawt_trigger_event_get_branch(event)
                                    : NULL,
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6,
                      event != NULL ? clawt_trigger_event_get_actor(event)
                                    : NULL,
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, (gint)outcome);
    sqlite3_bind_text(stmt, 8, detail, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, task_id, -1, SQLITE_TRANSIENT);
    /*
     * Only a delivery that started a run has anything to finish.  A
     * receipt that never ran counting as unfinished would fill the
     * pending cap with deliveries the trigger deliberately ignored.
     */
    sqlite3_bind_int(stmt, 10, outcome == CLAWT_DELIVERY_RAN ? 0 : 1);
    sqlite3_bind_int64(stmt, 11, g_get_real_time() / G_USEC_PER_SEC);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        g_warning("triggers: a receipt for '%s' could not be written: %s",
                  trigger_id, sqlite3_errmsg(self->db));

    sqlite3_finalize(stmt);
}

static void
put(GHashTable *row, const gchar *key, const gchar *value)
{
    if (value != NULL)
        g_hash_table_insert(row, g_strdup(key), g_strdup(value));
}

GPtrArray *
clawt_trigger_store_list_deliveries(ClawtTriggerStore *self,
                                    const gchar       *trigger_id,
                                    guint              limit)
{
    GPtrArray *out;
    sqlite3_stmt *stmt = NULL;
    const gchar *sql;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_unref);

    sql = (trigger_id != NULL)
        ? "SELECT trigger_id, delivery_id, event_name, repo, branch, actor,"
          " outcome, detail, task_id, created_at FROM deliveries"
          " WHERE trigger_id = ? ORDER BY created_at DESC, id DESC LIMIT ?"
        : "SELECT trigger_id, delivery_id, event_name, repo, branch, actor,"
          " outcome, detail, task_id, created_at FROM deliveries"
          " ORDER BY created_at DESC, id DESC LIMIT ?";

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return out;

    if (trigger_id != NULL) {
        sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, (gint)limit);
    } else {
        sqlite3_bind_int(stmt, 1, (gint)limit);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GHashTable *row = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);

        put(row, "trigger", (const gchar *)sqlite3_column_text(stmt, 0));
        put(row, "delivery", (const gchar *)sqlite3_column_text(stmt, 1));
        put(row, "event", (const gchar *)sqlite3_column_text(stmt, 2));
        put(row, "repo", (const gchar *)sqlite3_column_text(stmt, 3));
        put(row, "branch", (const gchar *)sqlite3_column_text(stmt, 4));
        put(row, "actor", (const gchar *)sqlite3_column_text(stmt, 5));
        put(row, "outcome",
            clawt_enum_to_nick(CLAWT_TYPE_DELIVERY_OUTCOME,
                               sqlite3_column_int(stmt, 6)));
        put(row, "detail", (const gchar *)sqlite3_column_text(stmt, 7));
        put(row, "task", (const gchar *)sqlite3_column_text(stmt, 8));

        {
            g_autofree gchar *at =
                g_strdup_printf("%" G_GINT64_FORMAT,
                                (gint64)sqlite3_column_int64(stmt, 9));

            put(row, "at", at);
        }

        g_ptr_array_add(out, row);
    }

    sqlite3_finalize(stmt);

    return out;
}

guint
clawt_trigger_store_count_unfinished(ClawtTriggerStore *self,
                                     const gchar       *trigger_id)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), 0);
    g_return_val_if_fail(trigger_id != NULL, 0);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT COUNT(*) FROM deliveries"
                           " WHERE trigger_id = ? AND finished = 0",
                           -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);

    return count;
}

void
clawt_trigger_store_finish(ClawtTriggerStore *self, const gchar *task_id)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_TRIGGER_STORE(self));

    if (task_id == NULL || *task_id == '\0')
        return;

    if (sqlite3_prepare_v2(self->db,
                           "UPDATE deliveries SET finished = 1"
                           " WHERE task_id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

guint
clawt_trigger_store_recent_count(ClawtTriggerStore *self,
                                 const gchar       *trigger_id,
                                 gint64             within_seconds)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_STORE(self), 0);
    g_return_val_if_fail(trigger_id != NULL, 0);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT COUNT(*) FROM deliveries"
                           " WHERE trigger_id = ? AND created_at >= ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, trigger_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2,
                       (g_get_real_time() / G_USEC_PER_SEC) - within_seconds);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);

    return count;
}

void
clawt_trigger_store_prune(ClawtTriggerStore *self, gint64 retain_seconds)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_TRIGGER_STORE(self));

    if (retain_seconds <= 0)
        return;

    if (sqlite3_prepare_v2(self->db,
                           "DELETE FROM deliveries WHERE created_at < ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_int64(stmt, 1,
                       (g_get_real_time() / G_USEC_PER_SEC) - retain_seconds);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}
