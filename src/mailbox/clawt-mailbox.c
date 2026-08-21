/*
 * clawt-mailbox.c - An agent's durable message queue
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mailbox/clawt-mailbox.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

enum {
    SIGNAL_ITEM_ADDED,
    SIGNAL_ITEM_LEASED,
    SIGNAL_ITEM_ACKED,
    SIGNAL_ITEM_DEAD_LETTERED,
    SIGNAL_DEPTH_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtMailbox {
    GObject parent_instance;

    gchar   *agent_id;
    gchar   *db_path;
    sqlite3 *db;

    guint               max_depth;
    ClawtOverflowPolicy overflow;
    guint               max_attempts;
    guint               lease_seconds;
    guint               backoff_seconds;
    guint               default_ttl_seconds;
};

G_DEFINE_FINAL_TYPE(ClawtMailbox, clawt_mailbox, G_TYPE_OBJECT)

/*
 * The schema.
 *
 * One index, on (state, priority, created_at), because that is exactly the
 * ordering every read uses: pending items, highest band first, oldest
 * first within a band.  The id sorts by time already, so created_at is
 * there for readability rather than necessity.
 */
static const gchar *SCHEMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS items ("
    "  id TEXT PRIMARY KEY,"
    "  sender TEXT,"
    "  recipient TEXT,"
    "  body TEXT NOT NULL,"
    "  room TEXT,"
    "  task_id TEXT,"
    "  reply_to TEXT,"
    "  subject TEXT,"
    "  idempotency_key TEXT,"
    "  last_error TEXT,"
    "  priority INTEGER NOT NULL DEFAULT 1,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  depth INTEGER NOT NULL DEFAULT 0,"
    "  attempts INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  not_before INTEGER NOT NULL DEFAULT 0,"
    "  expires_at INTEGER NOT NULL DEFAULT 0,"
    "  lease_expires_at INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_delivery"
    "  ON items(state, priority DESC, created_at ASC);"
    "CREATE INDEX IF NOT EXISTS idx_expiry ON items(expires_at);"
    /*
     * Unique only where a key was actually given.  A plain UNIQUE column
     * would make every keyless item collide with every other, since SQLite
     * treats NULLs as distinct but an empty string as equal.
     */
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_idempotency"
    "  ON items(idempotency_key) WHERE idempotency_key IS NOT NULL;";

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

/* ── Row <-> item ────────────────────────────────────────────────── */

static const gchar *
column_text(sqlite3_stmt *stmt, gint column)
{
    return (const gchar *)sqlite3_column_text(stmt, column);
}

static ClawtMailboxItem *
item_from_row(sqlite3_stmt *stmt)
{
    ClawtMailboxItem *item;

    item = clawt_mailbox_item_new(column_text(stmt, 1),
                                  column_text(stmt, 2),
                                  column_text(stmt, 3));

    clawt_mailbox_item_set_id(item, column_text(stmt, 0));
    clawt_mailbox_item_set_room(item, column_text(stmt, 4));
    clawt_mailbox_item_set_task_id(item, column_text(stmt, 5));
    clawt_mailbox_item_set_reply_to(item, column_text(stmt, 6));
    clawt_mailbox_item_set_subject(item, column_text(stmt, 7));
    clawt_mailbox_item_set_idempotency_key(item, column_text(stmt, 8));
    clawt_mailbox_item_set_last_error(item, column_text(stmt, 9));

    clawt_mailbox_item_set_priority(item,
        (ClawtPriority)sqlite3_column_int(stmt, 10));
    clawt_mailbox_item_set_state(item,
        (ClawtMailboxState)sqlite3_column_int(stmt, 11));
    clawt_mailbox_item_set_depth(item, sqlite3_column_int(stmt, 12));
    clawt_mailbox_item_set_attempts(item, sqlite3_column_int(stmt, 13));
    clawt_mailbox_item_set_created_at(item, sqlite3_column_int64(stmt, 14));
    clawt_mailbox_item_set_not_before(item, sqlite3_column_int64(stmt, 15));
    clawt_mailbox_item_set_expires_at(item, sqlite3_column_int64(stmt, 16));

    return item;
}

#define SELECT_COLUMNS \
    "id, sender, recipient, body, room, task_id, reply_to, subject, " \
    "idempotency_key, last_error, priority, state, depth, attempts, " \
    "created_at, not_before, expires_at"

/* ── Depth ───────────────────────────────────────────────────────── */

static guint
count_state(ClawtMailbox *self, ClawtMailboxState state)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT COUNT(*) FROM items WHERE state = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int(stmt, 1, (gint)state);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    return count;
}

guint
clawt_mailbox_depth(ClawtMailbox *self)
{
    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), 0);

    /*
     * Pending and leased both count: a leased item has not been dealt with,
     * and if its lease expires it comes straight back.  Counting only
     * pending would make a mailbox look empty while an agent sat on all of
     * it.
     */
    return count_state(self, CLAWT_MAILBOX_PENDING) +
           count_state(self, CLAWT_MAILBOX_LEASED);
}

static void
emit_depth_changed(ClawtMailbox *self)
{
    g_signal_emit(self, signals[SIGNAL_DEPTH_CHANGED], 0,
                  clawt_mailbox_depth(self));
}

/* ── Opening ─────────────────────────────────────────────────────── */

/*
 * A database that will not open is moved aside and recreated.
 *
 * A corrupt queue should cost the messages in it, not the agent: refusing
 * to start would leave an agent permanently dead because of one bad write
 * during a power cut.  The file is kept rather than deleted so it can be
 * looked at afterwards.
 */
static gboolean
quarantine_and_recreate(ClawtMailbox *self, GError **error)
{
    g_autofree gchar *quarantine = NULL;
    gint64 stamp = now_seconds();

    quarantine = g_strdup_printf("%s.corrupt-%" G_GINT64_FORMAT,
                                 self->db_path, stamp);

    g_warning("mailbox %s: the queue could not be opened; moving it to %s "
              "and starting a fresh one", self->agent_id, quarantine);

    if (g_rename(self->db_path, quarantine) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not move the unreadable mailbox aside: %s",
                    g_strerror(errno));
        return FALSE;
    }

    /*
     * The failed handle is closed before reopening.  sqlite3_open leaves
     * a usable handle even when it fails -- that is how sqlite3_errmsg()
     * works on it -- so overwriting the pointer would strand the whole
     * connection, page cache included, for the life of the process.
     */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    if (sqlite3_open(self->db_path, &self->db) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "reopening the mailbox");
        return FALSE;
    }

    if (sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "creating the mailbox schema");
        return FALSE;
    }

    return TRUE;
}

ClawtMailbox *
clawt_mailbox_new(const gchar *agent_id, const gchar *db_path, GError **error)
{
    ClawtMailbox *self;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(agent_id != NULL, NULL);
    g_return_val_if_fail(db_path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_MAILBOX, NULL);
    self->agent_id = g_strdup(agent_id);
    self->db_path = clawt_expand_path(db_path);

    dir = g_path_get_dirname(self->db_path);
    if (!clawt_ensure_dir(dir, 0700, error)) {
        g_object_unref(self);
        return NULL;
    }

    if (sqlite3_open(self->db_path, &self->db) != SQLITE_OK ||
        sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        if (!quarantine_and_recreate(self, error)) {
            g_object_unref(self);
            return NULL;
        }
    }

    /* Mailboxes hold conversations, so nobody else gets to read them. */
    g_chmod(self->db_path, 0600);

    return self;
}

void
clawt_mailbox_set_policy(ClawtMailbox        *self,
                         guint                max_depth,
                         ClawtOverflowPolicy  overflow,
                         guint                max_attempts,
                         guint                lease_seconds,
                         guint                backoff_seconds,
                         guint                default_ttl_seconds)
{
    g_return_if_fail(CLAWT_IS_MAILBOX(self));

    self->max_depth = max_depth;
    self->overflow = overflow;
    self->max_attempts = max_attempts;
    self->lease_seconds = lease_seconds;
    self->backoff_seconds = backoff_seconds;
    self->default_ttl_seconds = default_ttl_seconds;
}

/* ── Posting ─────────────────────────────────────────────────────── */

static gboolean
drop_oldest_low_priority(ClawtMailbox *self)
{
    sqlite3_stmt *stmt = NULL;
    gboolean dropped = FALSE;

    /*
     * Lowest band first, then oldest.  Dropping the newest would discard
     * exactly the message somebody just sent and is waiting on.
     */
    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM items WHERE id = ("
            "  SELECT id FROM items WHERE state = ?"
            "  ORDER BY priority ASC, created_at ASC LIMIT 1)",
            -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_PENDING);
    dropped = (sqlite3_step(stmt) == SQLITE_DONE) &&
              (sqlite3_changes(self->db) > 0);
    sqlite3_finalize(stmt);

    return dropped;
}

static gboolean
enforce_capacity(ClawtMailbox *self, GError **error)
{
    if (self->max_depth == 0)
        return TRUE;

    if (clawt_mailbox_depth(self) < self->max_depth)
        return TRUE;

    switch (self->overflow) {
    case CLAWT_OVERFLOW_DROP_OLDEST:
        if (drop_oldest_low_priority(self)) {
            /*
             * Logged, always.  A queue that silently discards is a queue
             * that loses work nobody notices until much later.
             */
            g_warning("mailbox %s: full at %u items; dropped the oldest "
                      "low-priority message to make room",
                      self->agent_id, self->max_depth);
            return TRUE;
        }
        break;

    case CLAWT_OVERFLOW_BLOCK_SENDER:
    case CLAWT_OVERFLOW_REJECT:
    default:
        break;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_FULL,
                "%s's mailbox is full (%u items); the message was not queued",
                self->agent_id, self->max_depth);
    return FALSE;
}

gchar *
clawt_mailbox_post(ClawtMailbox *self, ClawtMailboxItem *item, GError **error)
{
    sqlite3_stmt *stmt = NULL;
    const gchar *key;
    gint64 expires_at;
    gint rc;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);
    g_return_val_if_fail(item != NULL, NULL);

    key = clawt_mailbox_item_get_idempotency_key(item);

    /*
     * An idempotency key makes posting at-most-once.  A tool call that
     * timed out on the agent's side but succeeded here will be retried, and
     * without this that retry enqueues a second copy and the work happens
     * twice.
     */
    if (key != NULL) {
        sqlite3_stmt *check = NULL;
        gchar *existing = NULL;

        if (sqlite3_prepare_v2(self->db,
                "SELECT id FROM items WHERE idempotency_key = ?",
                -1, &check, NULL) == SQLITE_OK) {
            sqlite3_bind_text(check, 1, key, -1, SQLITE_TRANSIENT);

            if (sqlite3_step(check) == SQLITE_ROW)
                existing = g_strdup(column_text(check, 0));

            sqlite3_finalize(check);
        }

        if (existing != NULL)
            return existing;
    }

    if (!enforce_capacity(self, error))
        return NULL;

    expires_at = clawt_mailbox_item_get_expires_at(item);
    if (expires_at == 0 && self->default_ttl_seconds > 0)
        expires_at = now_seconds() + self->default_ttl_seconds;

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO items (id, sender, recipient, body, room, task_id,"
            " reply_to, subject, idempotency_key, priority, state, depth,"
            " attempts, created_at, not_before, expires_at)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "queueing a message");
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, clawt_mailbox_item_get_id(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, clawt_mailbox_item_get_from(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, clawt_mailbox_item_get_to(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, clawt_mailbox_item_get_body(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, clawt_mailbox_item_get_room(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, clawt_mailbox_item_get_task_id(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, clawt_mailbox_item_get_reply_to(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, clawt_mailbox_item_get_subject(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 10, (gint)clawt_mailbox_item_get_priority(item));
    sqlite3_bind_int(stmt, 11, CLAWT_MAILBOX_PENDING);
    sqlite3_bind_int(stmt, 12, clawt_mailbox_item_get_depth(item));
    sqlite3_bind_int(stmt, 13, 0);
    sqlite3_bind_int64(stmt, 14, clawt_mailbox_item_get_created_at(item));
    sqlite3_bind_int64(stmt, 15, clawt_mailbox_item_get_not_before(item));
    sqlite3_bind_int64(stmt, 16, expires_at);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "queueing a message");
        return NULL;
    }

    g_signal_emit(self, signals[SIGNAL_ITEM_ADDED], 0,
                  clawt_mailbox_item_get_id(item));
    emit_depth_changed(self);

    return g_strdup(clawt_mailbox_item_get_id(item));
}

/* ── Leasing ─────────────────────────────────────────────────────── */

ClawtMailboxItem *
clawt_mailbox_lease(ClawtMailbox *self, guint lease_seconds)
{
    sqlite3_stmt *stmt = NULL;
    ClawtMailboxItem *item = NULL;
    gint64 now;
    gint64 deadline;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);

    if (lease_seconds == 0)
        lease_seconds = self->lease_seconds > 0 ? self->lease_seconds : 300;

    now = now_seconds();
    deadline = now + lease_seconds;

    /*
     * Highest priority band first, oldest first within it, and nothing
     * whose not_before is still in the future.  not_before is how both
     * "deliver this later" and the retry backoff are expressed, so there is
     * one mechanism rather than two.
     */
    if (sqlite3_prepare_v2(self->db,
            "SELECT " SELECT_COLUMNS " FROM items"
            " WHERE state = ? AND (not_before = 0 OR not_before <= ?)"
            "   AND (expires_at = 0 OR expires_at > ?)"
            " ORDER BY priority DESC, created_at ASC LIMIT 1",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_PENDING);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, now);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        item = item_from_row(stmt);

    sqlite3_finalize(stmt);

    if (item == NULL)
        return NULL;

    if (sqlite3_prepare_v2(self->db,
            "UPDATE items SET state = ?, lease_expires_at = ?,"
            " attempts = attempts + 1 WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        clawt_mailbox_item_free(item);
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_LEASED);
    sqlite3_bind_int64(stmt, 2, deadline);
    sqlite3_bind_text(stmt, 3, clawt_mailbox_item_get_id(item), -1,
                      SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    clawt_mailbox_item_set_state(item, CLAWT_MAILBOX_LEASED);
    clawt_mailbox_item_set_attempts(item,
        clawt_mailbox_item_get_attempts(item) + 1);

    g_signal_emit(self, signals[SIGNAL_ITEM_LEASED], 0,
                  clawt_mailbox_item_get_id(item));

    return item;
}

static gboolean
item_is_in_state(ClawtMailbox      *self,
                 const gchar       *id,
                 ClawtMailboxState  state)
{
    sqlite3_stmt *stmt = NULL;
    gboolean matched = FALSE;

    if (sqlite3_prepare_v2(self->db, "SELECT state FROM items WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        matched = (sqlite3_column_int(stmt, 0) == (gint)state);

    sqlite3_finalize(stmt);
    return matched;
}

gboolean
clawt_mailbox_ack(ClawtMailbox *self, const gchar *id, GError **error)
{
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    /*
     * Only a leased item may be acknowledged.  An ack arriving after the
     * lease expired refers to work that has already been handed to somebody
     * else, and honouring it would mark that second delivery done before it
     * had happened.
     */
    if (!item_is_in_state(self, id, CLAWT_MAILBOX_LEASED)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_STATE,
                    "message %s is not leased; it cannot be acknowledged", id);
        return FALSE;
    }

    if (sqlite3_prepare_v2(self->db,
            "UPDATE items SET state = ?, lease_expires_at = 0 WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "acknowledging a message");
        return FALSE;
    }

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_ACKED);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    g_signal_emit(self, signals[SIGNAL_ITEM_ACKED], 0, id);
    emit_depth_changed(self);

    return TRUE;
}

gboolean
clawt_mailbox_nack(ClawtMailbox  *self,
                   const gchar   *id,
                   const gchar   *reason,
                   GError       **error)
{
    g_autoptr(ClawtMailboxItem) item = NULL;
    sqlite3_stmt *stmt = NULL;
    gboolean exhausted;
    gint64 retry_at;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    if (!item_is_in_state(self, id, CLAWT_MAILBOX_LEASED)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_STATE,
                    "message %s is not leased; it cannot be returned", id);
        return FALSE;
    }

    item = clawt_mailbox_get(self, id);
    if (item == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "message %s does not exist", id);
        return FALSE;
    }

    exhausted = (self->max_attempts > 0) &&
                ((guint)clawt_mailbox_item_get_attempts(item) >=
                 self->max_attempts);

    if (exhausted) {
        if (sqlite3_prepare_v2(self->db,
                "UPDATE items SET state = ?, last_error = ?,"
                " lease_expires_at = 0 WHERE id = ?",
                -1, &stmt, NULL) != SQLITE_OK) {
            set_sqlite_error(error, self->db, "dead-lettering a message");
            return FALSE;
        }

        sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_DEAD);
        sqlite3_bind_text(stmt, 2, reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, id, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        /*
         * Dead-lettered, never dropped.  A message that failed five times
         * is usually a bug worth looking at, and deleting it destroys the
         * evidence.
         */
        g_warning("mailbox %s: message %s failed %d times and was "
                  "dead-lettered: %s",
                  self->agent_id, id, clawt_mailbox_item_get_attempts(item),
                  reason != NULL ? reason : "no reason given");

        g_signal_emit(self, signals[SIGNAL_ITEM_DEAD_LETTERED], 0, id);
        emit_depth_changed(self);

        return TRUE;
    }

    /*
     * Exponential backoff with jitter.  Without the jitter, a dependency
     * that failed for every queued message brings them all back at the same
     * instant, and the retry storm looks exactly like the original problem.
     */
    if (self->backoff_seconds == 0) {
        /*
         * Zero means retry immediately, the same way zero means "no limit"
         * for max_depth and the TTL.  Substituting a default here instead
         * would make a config asking for immediate retries quietly wait
         * half a minute, which is a hard thing to notice and a harder one
         * to explain.
         */
        retry_at = 0;
    } else {
        guint attempts = (guint)clawt_mailbox_item_get_attempts(item);
        guint delay = self->backoff_seconds << MIN(attempts, 6);
        guint jitter = g_random_int_range(0, (gint)MAX(delay / 4, 1));

        retry_at = now_seconds() + delay + jitter;
    }

    if (sqlite3_prepare_v2(self->db,
            "UPDATE items SET state = ?, not_before = ?, last_error = ?,"
            " lease_expires_at = 0 WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "returning a message to the queue");
        return FALSE;
    }

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_PENDING);
    sqlite3_bind_int64(stmt, 2, retry_at);
    sqlite3_bind_text(stmt, 3, reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return TRUE;
}

gboolean
clawt_mailbox_requeue(ClawtMailbox *self, const gchar *id, GError **error)
{
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    if (!item_is_in_state(self, id, CLAWT_MAILBOX_DEAD)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_STATE,
                    "message %s is not a dead letter", id);
        return FALSE;
    }

    /*
     * Attempts reset to zero.  Requeueing is a deliberate act after
     * somebody fixed whatever was broken, so it should get a full set of
     * tries rather than dying again on the next failure.
     */
    if (sqlite3_prepare_v2(self->db,
            "UPDATE items SET state = ?, attempts = 0, not_before = 0,"
            " last_error = NULL WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "requeueing a message");
        return FALSE;
    }

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_PENDING);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    emit_depth_changed(self);

    return TRUE;
}

/* ── Reading ─────────────────────────────────────────────────────── */

GPtrArray *
clawt_mailbox_list(ClawtMailbox *self, ClawtMailboxFilter *filter)
{
    ClawtMailboxFilter defaults = { CLAWT_MAILBOX_PENDING, 0, TRUE };
    g_autofree gchar *sql = NULL;
    sqlite3_stmt *stmt = NULL;
    GPtrArray *items;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);

    if (filter == NULL)
        filter = &defaults;

    items = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_mailbox_item_free);

    sql = g_strdup_printf(
        "SELECT " SELECT_COLUMNS " FROM items%s%s"
        " ORDER BY priority DESC, created_at ASC%s",
        filter->state >= 0 ? " WHERE state = ?" : "",
        (filter->state >= 0 && !filter->include_future)
            ? " AND (not_before = 0 OR not_before <= strftime('%s','now'))" : "",
        filter->limit > 0 ? " LIMIT ?" : "");

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return items;

    {
        gint index = 1;

        if (filter->state >= 0)
            sqlite3_bind_int(stmt, index++, filter->state);
        if (filter->limit > 0)
            sqlite3_bind_int(stmt, index++, (gint)filter->limit);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(items, item_from_row(stmt));

    sqlite3_finalize(stmt);

    return items;
}

ClawtMailboxItem *
clawt_mailbox_get(ClawtMailbox *self, const gchar *id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtMailboxItem *item = NULL;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT " SELECT_COLUMNS " FROM items WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        item = item_from_row(stmt);

    sqlite3_finalize(stmt);
    return item;
}

GPtrArray *
clawt_mailbox_dead_letters(ClawtMailbox *self)
{
    ClawtMailboxFilter filter = { CLAWT_MAILBOX_DEAD, 0, TRUE };

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);

    return clawt_mailbox_list(self, &filter);
}

/* ── Sweeps ──────────────────────────────────────────────────────── */

guint
clawt_mailbox_purge_expired(ClawtMailbox *self)
{
    sqlite3_stmt *stmt = NULL;
    guint removed = 0;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), 0);

    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM items WHERE expires_at > 0 AND expires_at <= ?"
            " AND state IN (?, ?)",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, now_seconds());
    sqlite3_bind_int(stmt, 2, CLAWT_MAILBOX_PENDING);
    sqlite3_bind_int(stmt, 3, CLAWT_MAILBOX_FAILED);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        removed = (guint)sqlite3_changes(self->db);

    sqlite3_finalize(stmt);

    if (removed > 0) {
        g_info("mailbox %s: %u message(s) expired unread", self->agent_id,
               removed);
        emit_depth_changed(self);
    }

    return removed;
}

guint
clawt_mailbox_reclaim_expired_leases(ClawtMailbox *self)
{
    sqlite3_stmt *stmt = NULL;
    guint reclaimed = 0;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), 0);

    /*
     * This is what recovers work from an agent that died holding it: the
     * item goes back to pending with its attempt already counted, so a
     * repeatedly-crashing agent eventually dead-letters the message that
     * kills it rather than looping on it for ever.
     */
    if (sqlite3_prepare_v2(self->db,
            "UPDATE items SET state = ?, lease_expires_at = 0"
            " WHERE state = ? AND lease_expires_at > 0"
            "   AND lease_expires_at <= ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_PENDING);
    sqlite3_bind_int(stmt, 2, CLAWT_MAILBOX_LEASED);
    sqlite3_bind_int64(stmt, 3, now_seconds());

    if (sqlite3_step(stmt) == SQLITE_DONE)
        reclaimed = (guint)sqlite3_changes(self->db);

    sqlite3_finalize(stmt);

    if (reclaimed > 0)
        g_info("mailbox %s: reclaimed %u message(s) whose lease expired",
               self->agent_id, reclaimed);

    return reclaimed;
}

const gchar *
clawt_mailbox_get_agent_id(ClawtMailbox *self)
{
    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);
    return self->agent_id;
}

/* ── Object lifecycle ────────────────────────────────────────────── */

static void
clawt_mailbox_finalize(GObject *object)
{
    ClawtMailbox *self = CLAWT_MAILBOX(object);

    if (self->db != NULL) {
        /*
         * close_v2, not close.  Plain sqlite3_close() refuses with
         * SQLITE_BUSY when anything is still outstanding and leaves the
         * whole connection -- page cache included -- allocated.  close_v2
         * marks it a zombie and frees it once the last statement goes,
         * which is what we want at teardown.
         */
        gint status = sqlite3_close_v2(self->db);

        if (status != SQLITE_OK)
            g_warning("mailbox %s: its database did not close cleanly: %s",
                      self->agent_id, sqlite3_errstr(status));

        self->db = NULL;
    }

    g_clear_pointer(&self->agent_id, g_free);
    g_clear_pointer(&self->db_path, g_free);

    G_OBJECT_CLASS(clawt_mailbox_parent_class)->finalize(object);
}

static void
clawt_mailbox_class_init(ClawtMailboxClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = clawt_mailbox_finalize;

    signals[SIGNAL_ITEM_ADDED] =
        g_signal_new("item-added", CLAWT_TYPE_MAILBOX, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_ITEM_LEASED] =
        g_signal_new("item-leased", CLAWT_TYPE_MAILBOX, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_ITEM_ACKED] =
        g_signal_new("item-acked", CLAWT_TYPE_MAILBOX, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_ITEM_DEAD_LETTERED] =
        g_signal_new("item-dead-lettered", CLAWT_TYPE_MAILBOX,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_DEPTH_CHANGED] =
        g_signal_new("depth-changed", CLAWT_TYPE_MAILBOX, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
clawt_mailbox_init(ClawtMailbox *self)
{
    self->max_depth = 1000;
    self->overflow = CLAWT_OVERFLOW_REJECT;
    self->max_attempts = 5;
    self->lease_seconds = 300;
    self->backoff_seconds = 30;
    self->default_ttl_seconds = 604800;
}
