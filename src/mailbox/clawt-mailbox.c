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
	"  request_room TEXT,"
	"  request_origin TEXT,"
	"  request_task TEXT,"
    "  model TEXT,"
    "  subject TEXT,"
    "  idempotency_key TEXT,"
    "  last_error TEXT,"
    "  priority INTEGER NOT NULL DEFAULT 1,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  depth INTEGER NOT NULL DEFAULT 0,"
    "  attempts INTEGER NOT NULL DEFAULT 0,"
    "  invites_reply INTEGER NOT NULL DEFAULT 1,"
	"  composed INTEGER NOT NULL DEFAULT 0,"
	"  one_way INTEGER NOT NULL DEFAULT 0,"
	"  consult INTEGER NOT NULL DEFAULT 0,"
	"  room_round INTEGER NOT NULL DEFAULT 0,"
	"  chain TEXT,"
	"  message_id TEXT,"
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
    "  ON items(idempotency_key) WHERE idempotency_key IS NOT NULL;"
	/*
	 * The turn records: one row per delivery the agent was handed and
	 * has not finished, and the items it carried.  In this file rather
	 * than in memory because the question they answer -- "was this turn
	 * cut, and had it already acted outward?" -- is asked by the *next*
	 * daemon as often as by this one.  See clawt_mailbox_turn_open().
	 */
	"CREATE TABLE IF NOT EXISTS turns ("
	"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"  room TEXT,"
	"  replays INTEGER NOT NULL DEFAULT 0,"
	"  outward TEXT,"
	"  opened_at INTEGER NOT NULL"
	");"
	"CREATE TABLE IF NOT EXISTS turn_items ("
	"  turn INTEGER NOT NULL,"
	"  item TEXT NOT NULL,"
	"  position INTEGER NOT NULL"
	");"
	"CREATE INDEX IF NOT EXISTS idx_turn_items ON turn_items(turn);"
	"CREATE INDEX IF NOT EXISTS idx_turn_item_ids ON turn_items(item);";

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
 * Whether a column is already on the items table.
 *
 * Asked rather than inferred from a failed ALTER.  "Add it and ignore
 * the error" cannot tell a column that is already there from a database
 * that is unwritable, and the second one has to keep being an error --
 * a mailbox that silently declines to migrate would answer every read
 * with the column's default for ever.
 */
static gboolean
has_column(sqlite3 *db, const gchar *table, const gchar *column)
{
    g_autofree gchar *sql = NULL;
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    /*
     * PRAGMA takes no parameters, so the table name is spliced in.  It
     * is a literal at every call site below and never reaches here from
     * a config file or a message.
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
 * One function because there are two places a mailbox is opened -- the
 * ordinary path and the one that recreates a quarantined file -- and a
 * migration applied at only one of them is a migration that runs
 * whenever the file happens to be corrupt.
 *
 * CREATE TABLE IF NOT EXISTS covers a new file and does nothing to an
 * existing one, which is exactly why every column added after the first
 * release needs a line here as well.
 */
static gboolean
apply_schema(sqlite3 *db, GError **error)
{
	static const gchar *context_columns[] = {
		"request_room", "request_origin", "request_task"
	};
	guint i;

    if (db == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the mailbox database is not open");
        return FALSE;
    }

    if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, db, "creating the mailbox schema");
        return FALSE;
    }

    /*
     * Since 0.7.7: the model a delivery's turn runs on.  A fleet's
     * mailboxes predate it, and a column they lack would fail their
     * first read and quarantine the queue as corrupt.
     */
    if (!has_column(db, "items", "model") &&
        sqlite3_exec(db, "ALTER TABLE items ADD COLUMN model TEXT",
                     NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, db, "adding model to the mailbox");
        return FALSE;
    }

    /* Since 0.1.0: which items leave their recipient free to reply. */
    if (!has_column(db, "items", "invites_reply") &&
        sqlite3_exec(db,
                     "ALTER TABLE items ADD COLUMN invites_reply"
                     " INTEGER NOT NULL DEFAULT 1",
                     NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, db, "adding invites_reply to the mailbox");
        return FALSE;
    }

	/*
	 * Which bodies the daemon wrote itself.  Absent means every queued
	 * item reads as somebody else's text and is escaped whole at
	 * delivery -- the safe direction for a mailbox that predates it.
	 */
	if (!has_column(db, "items", "composed") &&
		sqlite3_exec(db,
			"ALTER TABLE items ADD COLUMN composed"
			" INTEGER NOT NULL DEFAULT 0",
			NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "adding composed to the mailbox");
		return FALSE;
	}

	/*
	 * Since 0.16.0: `reply: none`.  Absent means every queued item
	 * wanted an answer, which is what each one meant when it was queued.
	 */
	if (!has_column(db, "items", "one_way") &&
		sqlite3_exec(db,
			"ALTER TABLE items ADD COLUMN one_way"
			" INTEGER NOT NULL DEFAULT 0",
			NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "adding one_way to the mailbox");
		return FALSE;
	}

	/*
	 * Since 0.22.0: a consultation.  Absent means nothing queued was
	 * one, which is what each item meant when it was queued.
	 */
	if (!has_column(db, "items", "consult") &&
		sqlite3_exec(db,
			"ALTER TABLE items ADD COLUMN consult"
			" INTEGER NOT NULL DEFAULT 0",
			NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "adding consult to the mailbox");
		return FALSE;
	}

	/*
	 * Since 0.18.0: a group room's round and thread of replies.  Absent
	 * means every queued item opens a fresh thread, which lets a mention
	 * through that a fresh mailbox would hold back -- once, with
	 * max_hops still behind it.
	 */
	if (!has_column(db, "items", "room_round") &&
		sqlite3_exec(db,
			"ALTER TABLE items ADD COLUMN room_round"
			" INTEGER NOT NULL DEFAULT 0",
			NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "adding room_round to the mailbox");
		return FALSE;
	}

	if (!has_column(db, "items", "chain") &&
		sqlite3_exec(db, "ALTER TABLE items ADD COLUMN chain TEXT",
			NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "adding chain to the mailbox");
		return FALSE;
	}

	/*
	 * Since the restart replay: how many times an item was handed back
	 * after a restart cut its turn.  Absent means never, which is what
	 * every item queued before it was.
	 */
	if (!has_column(db, "items", "replays") &&
		sqlite3_exec(db,
			"ALTER TABLE items ADD COLUMN replays"
			" INTEGER NOT NULL DEFAULT 0",
			NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "adding replays to the mailbox");
		return FALSE;
	}

	/*
	 * Since 0.28.0: the room message an item delivers, so a steer's
	 * outcome can be drawn on it.  Absent means none, which only costs
	 * an older queued item its "steered" mark.
	 */
	if (!has_column(db, "items", "message_id") &&
		sqlite3_exec(db, "ALTER TABLE items ADD COLUMN message_id TEXT",
			NULL, NULL, NULL) != SQLITE_OK) {
		set_sqlite_error(error, db, "adding message_id to the mailbox");
		return FALSE;
	}

	/* Existing mailboxes must retain their queued work when upgraded. */
	for (i = 0; i < G_N_ELEMENTS(context_columns); i++) {
		g_autofree gchar *sql = NULL;

		if (has_column(db, "items", context_columns[i]))
			continue;
		sql = g_strdup_printf("ALTER TABLE items ADD COLUMN %s TEXT",
			context_columns[i]);
		if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
			set_sqlite_error(error, db, "adding request context to the mailbox");
			return FALSE;
		}
	}

    return TRUE;
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
    clawt_mailbox_item_set_invites_reply(item,
        sqlite3_column_int(stmt, 17) != 0);
	clawt_mailbox_item_set_request_context(item, column_text(stmt, 18),
		column_text(stmt, 19), column_text(stmt, 20));
    clawt_mailbox_item_set_model(item, column_text(stmt, 21));
	clawt_mailbox_item_set_composed(item, sqlite3_column_int(stmt, 22) != 0);
	clawt_mailbox_item_set_one_way(item, sqlite3_column_int(stmt, 23) != 0);
	clawt_mailbox_item_set_consult(item, sqlite3_column_int(stmt, 24) != 0);
	clawt_mailbox_item_set_round_chain(item,
		(guint)MAX(sqlite3_column_int(stmt, 25), 0), column_text(stmt, 26));
	clawt_mailbox_item_set_replays(item,
		(guint)MAX(sqlite3_column_int(stmt, 27), 0));
	clawt_mailbox_item_set_message_id(item, column_text(stmt, 28));

    return item;
}

#define SELECT_COLUMNS \
    "id, sender, recipient, body, room, task_id, reply_to, subject, " \
    "idempotency_key, last_error, priority, state, depth, attempts, " \
    "created_at, not_before, expires_at, invites_reply, " \
	"request_room, request_origin, request_task, model, composed, one_way, " \
	"consult, room_round, chain, replays, message_id"

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
 * How long to wait for another writer before giving up.
 *
 * Without this, a lock held by anything else -- a backup, a checkpoint,
 * somebody running sqlite3 on the file -- makes every write fail
 * instantly.  Waiting briefly turns almost all of that into a short pause
 * rather than an error.
 */
#define BUSY_TIMEOUT_MS 5000

/*
 * How long an acknowledged message stays in the queue before the sweep
 * removes it.
 *
 * Long enough that anything inspecting a mailbox shortly after delivery
 * still sees what happened, short enough that the file does not grow
 * without bound.  The durable record is the transcript and the event log.
 */
#define ACKED_RETENTION_SECONDS (24 * 60 * 60)

static void
set_busy_timeout(sqlite3 *db)
{
    if (db != NULL)
        sqlite3_busy_timeout(db, BUSY_TIMEOUT_MS);
}

/*
 * Runs a mutating statement and confirms it actually happened.
 *
 * sqlite3_step() returning SQLITE_BUSY is not a rare event: a second
 * connection to the same file -- a backup, a checkpoint, somebody running
 * sqlite3 on the mailbox -- takes a lock and this write is refused.
 * Ignoring the return value meant the queue reported success while the
 * row was untouched, which showed up as the same message being leased
 * twice and as acknowledgements that did not stick.
 *
 * Returns: %TRUE if the statement ran to completion
 */
static gboolean
step_write(ClawtMailbox  *self,
           sqlite3_stmt  *stmt,
           const gchar   *what,
           GError       **error)
{
    gint status = sqlite3_step(stmt);

    sqlite3_finalize(stmt);

    if (status == SQLITE_DONE)
        return TRUE;

    set_sqlite_error(error, self->db, what);

    return FALSE;
}

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

    set_busy_timeout(self->db);

    return apply_schema(self->db, error);
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

    if (sqlite3_open(self->db_path, &self->db) == SQLITE_OK)
        set_busy_timeout(self->db);

    if (!apply_schema(self->db, NULL)) {
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
        /*
         * A different error from reject, deliberately.  Nothing here
         * blocks a thread -- a daemon that stalled on a full queue would
         * stall every other agent with it -- so "block the sender" means
         * telling the sender to come back, and it has to be
         * distinguishable from "this was refused" or a caller cannot know
         * whether retrying is pointless.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_BUSY,
                    "%s's mailbox is full (%u items); try again shortly",
                    self->agent_id, self->max_depth);
        return FALSE;

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
            " attempts, created_at, not_before, expires_at, invites_reply,"
			" request_room, request_origin, request_task, model, composed,"
			" one_way, consult, room_round, chain, message_id)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
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
    sqlite3_bind_int(stmt, 17,
                     clawt_mailbox_item_get_invites_reply(item) ? 1 : 0);
	sqlite3_bind_text(stmt, 18, clawt_mailbox_item_get_request_room(item),
		-1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 19, clawt_mailbox_item_get_request_origin(item),
		-1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 20, clawt_mailbox_item_get_request_task(item),
		-1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 21, clawt_mailbox_item_get_model(item), -1,
                      SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 22, clawt_mailbox_item_get_composed(item) ? 1 : 0);
	sqlite3_bind_int(stmt, 23, clawt_mailbox_item_get_one_way(item) ? 1 : 0);
	sqlite3_bind_int(stmt, 24, clawt_mailbox_item_get_consult(item) ? 1 : 0);
	sqlite3_bind_int(stmt, 25, (gint)clawt_mailbox_item_get_round(item));
	sqlite3_bind_text(stmt, 26, clawt_mailbox_item_get_chain(item), -1,
		SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 27, clawt_mailbox_item_get_message_id(item), -1,
		SQLITE_TRANSIENT);

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

/*
 * Marks @item leased and hands it out, or frees it and returns NULL.
 *
 * Shared by the two ways of choosing an item, so "leased" means one
 * thing however the item was picked.
 */
static ClawtMailboxItem *
mark_leased(ClawtMailbox *self, ClawtMailboxItem *item, guint lease_seconds)
{
    sqlite3_stmt *stmt = NULL;
    gint64 deadline;

    if (lease_seconds == 0)
        lease_seconds = self->lease_seconds > 0 ? self->lease_seconds : 300;

    deadline = now_seconds() + lease_seconds;

    if (sqlite3_prepare_v2(self->db,
            "UPDATE items SET state = ?, lease_expires_at = ?,"
            " attempts = attempts + 1 WHERE id = ? AND state = ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        clawt_mailbox_item_free(item);
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_LEASED);
    sqlite3_bind_int64(stmt, 2, deadline);
    sqlite3_bind_text(stmt, 3, clawt_mailbox_item_get_id(item), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, CLAWT_MAILBOX_PENDING);

    /*
     * Nothing is handed out unless the lease is durably recorded.  The
     * alternative -- returning the item anyway -- means the next caller
     * leases the same message, and two agents work on it believing they
     * are the only one.
     */
    if (!step_write(self, stmt, "leasing a message", NULL)) {
        g_warning("mailbox %s: a lease could not be recorded; the message "
                  "stays queued", self->agent_id);
        clawt_mailbox_item_free(item);
        return NULL;
    }

    clawt_mailbox_item_set_state(item, CLAWT_MAILBOX_LEASED);
    clawt_mailbox_item_set_attempts(item,
        clawt_mailbox_item_get_attempts(item) + 1);

    g_signal_emit(self, signals[SIGNAL_ITEM_LEASED], 0,
                  clawt_mailbox_item_get_id(item));

    return item;
}

/*
 * The next deliverable item, or the deliverable item called @id.
 *
 * Highest priority band first, oldest first within it, and nothing
 * whose not_before is still in the future.  not_before is how both
 * "deliver this later" and the retry backoff are expressed, so there is
 * one mechanism rather than two -- and a lease by id asks the same
 * question, so an item in backoff cannot be taken early by naming it.
 */
static ClawtMailboxItem *
select_deliverable(ClawtMailbox *self, const gchar *id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtMailboxItem *item = NULL;
    gint64 now = now_seconds();

    if (sqlite3_prepare_v2(self->db,
            id == NULL
            ? "SELECT " SELECT_COLUMNS " FROM items"
              " WHERE state = ? AND (not_before = 0 OR not_before <= ?)"
              "   AND (expires_at = 0 OR expires_at > ?)"
              " ORDER BY priority DESC, created_at ASC LIMIT 1"
            : "SELECT " SELECT_COLUMNS " FROM items"
              " WHERE state = ? AND (not_before = 0 OR not_before <= ?)"
              "   AND (expires_at = 0 OR expires_at > ?) AND id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_PENDING);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, now);

    if (id != NULL)
        sqlite3_bind_text(stmt, 4, id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        item = item_from_row(stmt);

    sqlite3_finalize(stmt);

    return item;
}

ClawtMailboxItem *
clawt_mailbox_lease(ClawtMailbox *self, guint lease_seconds)
{
    ClawtMailboxItem *item;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);

    item = select_deliverable(self, NULL);

    if (item == NULL)
        return NULL;

    return mark_leased(self, item, lease_seconds);
}

ClawtMailboxItem *
clawt_mailbox_lease_id(ClawtMailbox *self, const gchar *id,
                       guint lease_seconds)
{
    ClawtMailboxItem *item;

    g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    item = select_deliverable(self, id);

    if (item == NULL)
        return NULL;

    return mark_leased(self, item, lease_seconds);
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

    if (!step_write(self, stmt, "acknowledging a message", error))
        return FALSE;

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

        if (!step_write(self, stmt, "dead-lettering a message", error))
            return FALSE;

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

    if (!step_write(self, stmt, "returning a message to the queue", error))
        return FALSE;

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

    if (!step_write(self, stmt, "requeueing a message", error))
        return FALSE;

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

    /*
     * The deferral clause is built independently of the state clause.
     * It used to be appended only when a state filter was present, so
     * listing every state silently included items that are not due yet --
     * the one combination no caller happened to use.
     */
    sql = g_strdup_printf(
        "SELECT " SELECT_COLUMNS " FROM items%s%s%s"
        " ORDER BY priority DESC, created_at ASC%s",
        (filter->state >= 0 || !filter->include_future) ? " WHERE " : "",
        filter->state >= 0 ? "state = ?" : "",
        !filter->include_future
            ? (filter->state >= 0 ? " AND (not_before = 0 OR not_before <="
                                    " strftime('%s','now'))"
                                  : "(not_before = 0 OR not_before <="
                                    " strftime('%s','now'))")
            : "",
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

    /*
     * Acknowledged messages are reaped too, once they are older than the
     * retention window.
     *
     * A queue only ever added rows before this: an acknowledged item was
     * left in place for ever, so a busy agent's mailbox.db grew by one
     * permanent row per message handled and never shrank.  The record of
     * what was said lives in the room transcript and the event log, which
     * is where anybody looks for it -- keeping a second copy in the queue
     * bought nothing.
     *
     * Dead letters are deliberately not swept: they are there to be
     * looked at.
     */
    {
        sqlite3_stmt *acked = NULL;
        gint64 cutoff = now_seconds() - (gint64)ACKED_RETENTION_SECONDS;

        if (sqlite3_prepare_v2(self->db,
                "DELETE FROM items WHERE state = ? AND created_at <= ?"
                /*
                 * Never an item an open turn record names: recovery
                 * hands it back by flipping this row, and a row reaped
                 * underneath it would turn a cut turn the agent could
                 * have rerun into an "interrupted" notice.
                 */
                " AND id NOT IN (SELECT item FROM turn_items)",
                -1, &acked, NULL) == SQLITE_OK) {
            sqlite3_bind_int(acked, 1, CLAWT_MAILBOX_ACKED);
            sqlite3_bind_int64(acked, 2, cutoff);

            if (sqlite3_step(acked) == SQLITE_DONE)
                removed += (guint)sqlite3_changes(self->db);

            sqlite3_finalize(acked);
        }
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

/* ── Turn records ────────────────────────────────────────────────── */

/*
 * A delivery is acknowledged at the socket, so after the ack the queue
 * no longer knows about it -- and libreclaw's queue is in memory, so a
 * process that dies mid-turn takes the turn with it.  These rows are
 * what survives: one per delivery the agent was handed and has not
 * finished, naming the items it carried and whether the turn has acted
 * outward yet.  The next start reads them back and decides, per turn,
 * between one rerun and an "interrupted" notice.
 */

void
clawt_turn_record_free(
	ClawtTurnRecord *record
){
	if (record == NULL)
		return;

	g_free(record->room);
	g_strfreev(record->items);
	g_free(record->outward);
	g_free(record->reason);
	g_free(record->from);
	g_free(record->body);
	g_free(record);
}

/* Runs one statement with no result, for BEGIN / COMMIT / ROLLBACK. */
static gboolean
exec_simple(
	ClawtMailbox  *self,
	const gchar   *sql,
	GError       **error
){
	if (sqlite3_exec(self->db, sql, NULL, NULL, NULL) == SQLITE_OK)
		return TRUE;

	set_sqlite_error(error, self->db, sql);
	return FALSE;
}

gint64
clawt_mailbox_turn_open(
	ClawtMailbox        *self,
	const gchar         *room_id,
	const gchar * const *item_ids,
	guint                replays,
	GError             **error
){
	sqlite3_stmt *stmt = NULL;
	gint64 turn;
	guint i;

	g_return_val_if_fail(CLAWT_IS_MAILBOX(self), 0);
	g_return_val_if_fail(item_ids != NULL && item_ids[0] != NULL, 0);

	/*
	 * One transaction, so a record is never on disk without the items
	 * it names: recovery would read a turn with nothing to hand back as
	 * a turn whose message was purged, and report a loss that is not
	 * one.
	 */
	if (!exec_simple(self, "BEGIN IMMEDIATE", error))
		return 0;

	if (sqlite3_prepare_v2(self->db,
			"INSERT INTO turns (room, replays, opened_at) VALUES (?, ?, ?)",
			-1, &stmt, NULL) != SQLITE_OK) {
		set_sqlite_error(error, self->db, "recording a turn");
		exec_simple(self, "ROLLBACK", NULL);
		return 0;
	}

	sqlite3_bind_text(stmt, 1, room_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, (gint)replays);
	sqlite3_bind_int64(stmt, 3, g_get_real_time());

	if (!step_write(self, stmt, "recording a turn", error)) {
		exec_simple(self, "ROLLBACK", NULL);
		return 0;
	}

	turn = (gint64)sqlite3_last_insert_rowid(self->db);

	for (i = 0; item_ids[i] != NULL; i++) {
		if (sqlite3_prepare_v2(self->db,
				"INSERT INTO turn_items (turn, item, position)"
				" VALUES (?, ?, ?)",
				-1, &stmt, NULL) != SQLITE_OK) {
			set_sqlite_error(error, self->db, "recording a turn's items");
			exec_simple(self, "ROLLBACK", NULL);
			return 0;
		}

		sqlite3_bind_int64(stmt, 1, turn);
		sqlite3_bind_text(stmt, 2, item_ids[i], -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 3, (gint)i);

		if (!step_write(self, stmt, "recording a turn's items", error)) {
			exec_simple(self, "ROLLBACK", NULL);
			return 0;
		}
	}

	if (!exec_simple(self, "COMMIT", error)) {
		exec_simple(self, "ROLLBACK", NULL);
		return 0;
	}

	return turn;
}

gboolean
clawt_mailbox_turn_mark_outward(
	ClawtMailbox  *self,
	gint64         turn,
	const gchar   *what,
	GError       **error
){
	sqlite3_stmt *stmt = NULL;
	g_autofree gchar *current = NULL;
	g_autofree gchar *updated = NULL;
	gboolean found = FALSE;

	g_return_val_if_fail(CLAWT_IS_MAILBOX(self), FALSE);
	g_return_val_if_fail(what != NULL && *what != '\0', FALSE);

	if (sqlite3_prepare_v2(self->db,
			"SELECT outward FROM turns WHERE id = ?",
			-1, &stmt, NULL) != SQLITE_OK) {
		set_sqlite_error(error, self->db, "reading a turn record");
		return FALSE;
	}

	sqlite3_bind_int64(stmt, 1, turn);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		found = TRUE;
		current = g_strdup(column_text(stmt, 0));
	}

	sqlite3_finalize(stmt);

	if (!found) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			"no open turn record %" G_GINT64_FORMAT, turn);
		return FALSE;
	}

	/*
	 * What it did, each kind once: the notice lists it, and "sent a
	 * message to the operator" forty times over says nothing the first
	 * one did not.
	 */
	if (current != NULL && *current != '\0') {
		g_auto(GStrv) done = g_strsplit(current, ", ", -1);

		if (g_strv_contains((const gchar * const *)done, what))
			return TRUE;
		updated = g_strconcat(current, ", ", what, NULL);
	} else {
		updated = g_strdup(what);
	}

	if (sqlite3_prepare_v2(self->db,
			"UPDATE turns SET outward = ? WHERE id = ?",
			-1, &stmt, NULL) != SQLITE_OK) {
		set_sqlite_error(error, self->db, "marking a turn outward");
		return FALSE;
	}

	sqlite3_bind_text(stmt, 1, updated, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, turn);

	return step_write(self, stmt, "marking a turn outward", error);
}

/* Deletes one record and its item list. */
static gboolean
delete_turn(
	ClawtMailbox  *self,
	gint64         turn,
	GError       **error
){
	static const gchar *statements[] = {
		"DELETE FROM turn_items WHERE turn = ?",
		"DELETE FROM turns WHERE id = ?"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(statements); i++) {
		sqlite3_stmt *stmt = NULL;

		if (sqlite3_prepare_v2(self->db, statements[i], -1, &stmt,
				NULL) != SQLITE_OK) {
			set_sqlite_error(error, self->db, "closing a turn record");
			return FALSE;
		}

		sqlite3_bind_int64(stmt, 1, turn);

		if (!step_write(self, stmt, "closing a turn record", error))
			return FALSE;
	}

	return TRUE;
}

gboolean
clawt_mailbox_turn_close(
	ClawtMailbox  *self,
	gint64         turn,
	GError       **error
){
	g_return_val_if_fail(CLAWT_IS_MAILBOX(self), FALSE);

	if (!exec_simple(self, "BEGIN IMMEDIATE", error))
		return FALSE;

	if (!delete_turn(self, turn, error)) {
		exec_simple(self, "ROLLBACK", NULL);
		return FALSE;
	}

	if (!exec_simple(self, "COMMIT", error)) {
		exec_simple(self, "ROLLBACK", NULL);
		return FALSE;
	}

	return TRUE;
}

gboolean
clawt_mailbox_turn_join(
	ClawtMailbox  *self,
	gint64         into,
	gint64         from,
	GError       **error
){
	static const gchar *statements[] = {
		/* the items, after the ones already there, in their own order */
		"UPDATE turn_items SET turn = ?1, position = position +"
		" (SELECT COALESCE(MAX(position), -1) + 1 FROM turn_items"
		"  WHERE turn = ?1) WHERE turn = ?2",
		/* the higher replay count, so the join buys no extra rerun */
		"UPDATE turns SET replays = MAX(replays,"
		" COALESCE((SELECT replays FROM turns WHERE id = ?2), 0))"
		" WHERE id = ?1",
		/* and anything the steered part is marked as having done */
		"UPDATE turns SET outward = CASE"
		" WHEN (SELECT outward FROM turns WHERE id = ?2) IS NULL"
		"   OR (SELECT outward FROM turns WHERE id = ?2) = '' THEN outward"
		" WHEN outward IS NULL OR outward = ''"
		"   THEN (SELECT outward FROM turns WHERE id = ?2)"
		" ELSE outward || ', ' || (SELECT outward FROM turns WHERE id = ?2)"
		" END WHERE id = ?1",
		"DELETE FROM turns WHERE id = ?2"
	};
	guint i;

	g_return_val_if_fail(CLAWT_IS_MAILBOX(self), FALSE);

	if (into == from)
		return TRUE;

	if (!exec_simple(self, "BEGIN IMMEDIATE", error))
		return FALSE;

	for (i = 0; i < G_N_ELEMENTS(statements); i++) {
		sqlite3_stmt *stmt = NULL;

		if (sqlite3_prepare_v2(self->db, statements[i], -1, &stmt,
				NULL) != SQLITE_OK) {
			set_sqlite_error(error, self->db, "joining a turn record");
			exec_simple(self, "ROLLBACK", NULL);
			return FALSE;
		}

		sqlite3_bind_int64(stmt, 1, into);
		sqlite3_bind_int64(stmt, 2, from);

		if (!step_write(self, stmt, "joining a turn record", error)) {
			exec_simple(self, "ROLLBACK", NULL);
			return FALSE;
		}
	}

	if (!exec_simple(self, "COMMIT", error)) {
		exec_simple(self, "ROLLBACK", NULL);
		return FALSE;
	}

	return TRUE;
}

/* The items one record names, in the order they were delivered. */
static gchar **
turn_items(
	ClawtMailbox *self,
	gint64        turn
){
	sqlite3_stmt *stmt = NULL;
	GPtrArray *ids = g_ptr_array_new();

	if (sqlite3_prepare_v2(self->db,
			"SELECT item FROM turn_items WHERE turn = ? ORDER BY position",
			-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, turn);

		while (sqlite3_step(stmt) == SQLITE_ROW)
			g_ptr_array_add(ids, g_strdup(column_text(stmt, 0)));

		sqlite3_finalize(stmt);
	}

	g_ptr_array_add(ids, NULL);
	return (gchar **)g_ptr_array_free(ids, FALSE);
}

GPtrArray *
clawt_mailbox_turn_list_open(
	ClawtMailbox *self
){
	sqlite3_stmt *stmt = NULL;
	GPtrArray *out;

	g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_turn_record_free);

	if (sqlite3_prepare_v2(self->db,
			"SELECT id, room, replays, outward, opened_at FROM turns"
			" ORDER BY id",
			-1, &stmt, NULL) != SQLITE_OK)
		return out;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ClawtTurnRecord *record = g_new0(ClawtTurnRecord, 1);

		record->id = sqlite3_column_int64(stmt, 0);
		record->room = g_strdup(column_text(stmt, 1));
		record->replays = (guint)MAX(sqlite3_column_int(stmt, 2), 0);
		record->outward = g_strdup(column_text(stmt, 3));
		record->opened_at = sqlite3_column_int64(stmt, 4);
		g_ptr_array_add(out, record);
	}

	sqlite3_finalize(stmt);

	{
		guint i;

		for (i = 0; i < out->len; i++) {
			ClawtTurnRecord *record = g_ptr_array_index(out, i);
			g_autoptr(ClawtMailboxItem) first = NULL;

			record->items = turn_items(self, record->id);
			first = record->items[0] != NULL
				? clawt_mailbox_get(self, record->items[0]) : NULL;

			if (first != NULL) {
				record->from = g_strdup(clawt_mailbox_item_get_from(first));
				record->body = g_strdup(clawt_mailbox_item_get_body(first));
			}
		}
	}

	return out;
}

/*
 * Whether every item a cut turn carried can be handed back: still in
 * the queue, acknowledged (which is what a delivery leaves it), and not
 * past its own expiry.  Asked of the whole set before any is touched,
 * so a merged turn is handed back whole or not at all.
 */
static gboolean
items_can_return(
	ClawtMailbox  *self,
	gchar        **items,
	const gchar  **why
){
	gint64 now = now_seconds();
	guint i;

	if (items == NULL || items[0] == NULL) {
		*why = "the message it was answering is no longer in the queue";
		return FALSE;
	}

	for (i = 0; items[i] != NULL; i++) {
		g_autoptr(ClawtMailboxItem) item = clawt_mailbox_get(self, items[i]);

		if (item == NULL) {
			*why = "the message it was answering is no longer in the queue";
			return FALSE;
		}

		if (clawt_mailbox_item_get_state(item) != CLAWT_MAILBOX_ACKED) {
			*why = "the message it was answering is already back in the queue";
			return FALSE;
		}

		if (clawt_mailbox_item_get_expires_at(item) > 0 &&
			clawt_mailbox_item_get_expires_at(item) <= now) {
			*why = "the message it was answering has expired";
			return FALSE;
		}
	}

	return TRUE;
}

/* Puts one acknowledged item back in the queue, counted as a replay. */
static gboolean
return_item(
	ClawtMailbox  *self,
	const gchar   *id,
	GError       **error
){
	sqlite3_stmt *stmt = NULL;

	if (sqlite3_prepare_v2(self->db,
			"UPDATE items SET state = ?, replays = replays + 1,"
			" not_before = 0, lease_expires_at = 0 WHERE id = ? AND state = ?",
			-1, &stmt, NULL) != SQLITE_OK) {
		set_sqlite_error(error, self->db, "handing a cut turn back");
		return FALSE;
	}

	sqlite3_bind_int(stmt, 1, CLAWT_MAILBOX_PENDING);
	sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, CLAWT_MAILBOX_ACKED);

	return step_write(self, stmt, "handing a cut turn back", error);
}

GPtrArray *
clawt_mailbox_turn_recover(
	ClawtMailbox       *self,
	gboolean            replay,
	const gchar        *reason,
	ClawtTurnVetoFunc   veto,
	gpointer            veto_data,
	GError            **error
){
	g_autoptr(GPtrArray) open = NULL;
	guint i;
	gboolean any_returned = FALSE;

	g_return_val_if_fail(CLAWT_IS_MAILBOX(self), NULL);

	open = clawt_mailbox_turn_list_open(self);

	if (open->len == 0)
		return g_steal_pointer(&open);

	/*
	 * One transaction for the lot.  Handing an item back and deleting
	 * the record that allowed it are one decision: a daemon killed
	 * between the two would find the record again on its next start,
	 * still saying "not yet rerun", and hand the same message over a
	 * second time.
	 */
	if (!exec_simple(self, "BEGIN IMMEDIATE", error))
		return NULL;

	for (i = 0; i < open->len; i++) {
		ClawtTurnRecord *record = g_ptr_array_index(open, i);
		const gchar *why = NULL;
		guint j;

		/*
		 * The rule, in the order it is checked:
		 *
		 *   - a stop somebody asked for is not a crash, and the turn
		 *     it cut is not resumed behind their back;
		 *   - a turn that already acted outward is never rerun, since
		 *     a rerun would send, post or run it a second time;
		 *   - a turn that was itself a rerun is not rerun again, so a
		 *     message that kills the agent cannot loop;
		 *   - and the items have to still be there to hand back.
		 */
		if (!replay)
			why = reason != NULL ? reason : "it was stopped";
		else if (record->outward != NULL && *record->outward != '\0')
			why = "it had already acted outside the fleet";
		else if (record->replays > 0)
			why = "it was already handed back once after a restart";
		else
			items_can_return(self, record->items, &why);

		/* And the caller's own reasons, which the queue cannot see. */
		if (why == NULL && veto != NULL)
			why = veto(self, record, veto_data);

		if (why == NULL) {
			for (j = 0; record->items[j] != NULL; j++)
				if (!return_item(self, record->items[j], error)) {
					exec_simple(self, "ROLLBACK", NULL);
					return NULL;
				}
			record->redelivered = TRUE;
			any_returned = TRUE;
		} else {
			record->reason = g_strdup(why);
		}

		if (!delete_turn(self, record->id, error)) {
			exec_simple(self, "ROLLBACK", NULL);
			return NULL;
		}
	}

	if (!exec_simple(self, "COMMIT", error)) {
		exec_simple(self, "ROLLBACK", NULL);
		return NULL;
	}

	if (any_returned)
		emit_depth_changed(self);

	return g_steal_pointer(&open);
}

guint
clawt_mailbox_turn_count_open(
	ClawtMailbox *self
){
	sqlite3_stmt *stmt = NULL;
	guint count = 0;

	g_return_val_if_fail(CLAWT_IS_MAILBOX(self), 0);

	if (sqlite3_prepare_v2(self->db, "SELECT COUNT(*) FROM turns", -1,
			&stmt, NULL) != SQLITE_OK)
		return 0;

	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = (guint)sqlite3_column_int(stmt, 0);

	sqlite3_finalize(stmt);
	return count;
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
