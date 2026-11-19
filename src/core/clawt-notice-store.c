/*
 * clawt-notice-store.c - The fleet's notifications, held once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The SQLite shape is the read store's (chat/clawt-read-store.c), which
 * is the handoff store's: one connection, prepared statements made per
 * call, and every write a single statement so there is nothing to roll
 * back.
 */

#include <sqlite3.h>

#include "clawtilla.h"
#include "core/clawt-notice-store.h"

#define DAY_SECONDS ((gint64)24 * 60 * 60)

/*
 * `read` and `dismissed` are separate because they answer separate
 * questions: read is "does it still count on the bell", dismissed is
 * "does it still show".  Opening the panel reads everything and hides
 * nothing; dismissing hides one row and, since a hidden row cannot be
 * looked at, reads it too.
 */
static const gchar *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS notices ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  agent TEXT,"
    "  tier TEXT NOT NULL,"
    "  event_kind TEXT NOT NULL,"
    "  source TEXT NOT NULL DEFAULT '',"
    "  subject TEXT,"
    "  title TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  read INTEGER NOT NULL DEFAULT 0,"
    "  dismissed INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS notices_created ON notices (created_at);"
    "CREATE TABLE IF NOT EXISTS mutes ("
    "  agent TEXT PRIMARY KEY,"
    "  until INTEGER NOT NULL DEFAULT 0"
    ");";

struct _ClawtNoticeStore {
    GObject   parent_instance;
    sqlite3  *db;
    gchar    *path;
};

G_DEFINE_FINAL_TYPE(ClawtNoticeStore, clawt_notice_store, G_TYPE_OBJECT)

/* ── The two row types ───────────────────────────────────────────── */

ClawtNotice *
clawt_notice_copy(const ClawtNotice *self)
{
    ClawtNotice *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtNotice, 1);
    copy->id = self->id;
    copy->agent = g_strdup(self->agent);
    copy->tier = self->tier;
    copy->event_kind = g_strdup(self->event_kind);
    copy->source = g_strdup(self->source);
    copy->subject = g_strdup(self->subject);
    copy->title = g_strdup(self->title);
    copy->created_at = self->created_at;
    copy->read = self->read;
    copy->dismissed = self->dismissed;

    return copy;
}

void
clawt_notice_free(ClawtNotice *self)
{
    if (self == NULL)
        return;

    g_free(self->agent);
    g_free(self->event_kind);
    g_free(self->source);
    g_free(self->subject);
    g_free(self->title);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtNotice, clawt_notice, clawt_notice_copy,
                    clawt_notice_free)

ClawtNoticeMute *
clawt_notice_mute_copy(const ClawtNoticeMute *self)
{
    ClawtNoticeMute *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtNoticeMute, 1);
    copy->agent = g_strdup(self->agent);
    copy->until = self->until;

    return copy;
}

void
clawt_notice_mute_free(ClawtNoticeMute *self)
{
    if (self == NULL)
        return;

    g_free(self->agent);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtNoticeMute, clawt_notice_mute,
                    clawt_notice_mute_copy, clawt_notice_mute_free)

const gchar *
clawt_notice_tier_nick(ClawtAlertTier tier)
{
    switch (tier) {
    case CLAWT_ALERT_ERROR:
        return "error";
    case CLAWT_ALERT_NOTICE:
        return "notice";
    case CLAWT_ALERT_ROUTINE:
        return "routine";
    case CLAWT_ALERT_SKIP:
    default:
        return "skip";
    }
}

ClawtAlertTier
clawt_notice_tier_from_nick(const gchar *nick)
{
    if (g_strcmp0(nick, "error") == 0)
        return CLAWT_ALERT_ERROR;

    if (g_strcmp0(nick, "routine") == 0)
        return CLAWT_ALERT_ROUTINE;

    return CLAWT_ALERT_NOTICE;
}

gboolean
clawt_notice_mute_for_parse(const gchar *text, gint64 *out_seconds)
{
    gchar *end = NULL;
    gint64 value;
    gint64 unit = 60;

    g_return_val_if_fail(out_seconds != NULL, FALSE);

    *out_seconds = 0;

    if (text == NULL || !g_ascii_isdigit(*text))
        return FALSE;

    value = g_ascii_strtoll(text, &end, 10);

    if (end == NULL || value <= 0)
        return FALSE;

    switch (*end) {
    case '\0':
    case 'm':
        unit = 60;
        break;
    case 's':
        unit = 1;
        break;
    case 'h':
        unit = 60 * 60;
        break;
    case 'd':
        unit = DAY_SECONDS;
        break;
    default:
        return FALSE;
    }

    /* One letter of unit, and nothing after it: "2hours" is a typo. */
    if (*end != '\0' && end[1] != '\0')
        return FALSE;

    /* A year is long enough to mean "until unmuted" without saying so. */
    if (value > (gint64)366 * DAY_SECONDS / unit)
        return FALSE;

    *out_seconds = value * unit;

    return TRUE;
}

/* A count member, or 0 for one that is missing or is not a number. */
static guint
count_member(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return 0;

    node = json_object_get_member(object, name);

    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64)
        return 0;

    return (guint)CLAMP(json_node_get_int(node), 0, G_MAXINT);
}

guint
clawt_notice_list_badge(JsonObject *reply)
{
    JsonObject *sources = NULL;

    if (reply == NULL)
        return 0;

    if (json_object_has_member(reply, "sources") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(reply, "sources")))
        sources = json_object_get_object_member(reply, "sources");

    return count_member(sources, "chats_unread") +
           count_member(sources, "decisions_pending") +
           count_member(sources, "grok_attention") +
           count_member(reply, "unread");
}

GHashTable *
clawt_notice_list_mutes(JsonObject *reply)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            NULL);
    JsonArray *mutes;
    guint i;

    if (reply == NULL || !json_object_has_member(reply, "mutes") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(reply, "mutes")))
        return set;

    mutes = json_object_get_array_member(reply, "mutes");

    for (i = 0; i < json_array_get_length(mutes); i++) {
        JsonNode *node = json_array_get_element(mutes, i);
        JsonObject *mute;
        const gchar *agent;

        if (!JSON_NODE_HOLDS_OBJECT(node))
            continue;

        mute = json_node_get_object(node);

        if (!json_object_has_member(mute, "agent"))
            continue;

        agent = json_object_get_string_member(mute, "agent");

        if (agent != NULL && *agent != '\0')
            g_hash_table_add(set, g_strdup(agent));
    }

    return set;
}

/* ── The store ───────────────────────────────────────────────────── */

static void
set_sqlite_error(GError **error, sqlite3 *db, const gchar *what)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                "%s: %s", what, sqlite3_errmsg(db));
}

static void
clawt_notice_store_dispose(GObject *object)
{
    ClawtNoticeStore *self = CLAWT_NOTICE_STORE(object);

    /* _v2: see clawt_read_store_dispose() */
    if (self->db != NULL) {
        sqlite3_close_v2(self->db);
        self->db = NULL;
    }

    g_clear_pointer(&self->path, g_free);

    G_OBJECT_CLASS(clawt_notice_store_parent_class)->dispose(object);
}

static void
clawt_notice_store_class_init(ClawtNoticeStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_notice_store_dispose;
}

static void
clawt_notice_store_init(ClawtNoticeStore *self)
{
    (void)self;
}

ClawtNoticeStore *
clawt_notice_store_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtNoticeStore) self = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_NOTICE_STORE, NULL);
    self->path = g_strdup(path);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open the notice store: %s",
                    sqlite3_errmsg(self->db));
        return NULL;
    }

    if (sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "creating the notice schema");
        return NULL;
    }

    return g_steal_pointer(&self);
}

/* Binds @value, or NULL for a missing one. */
static void
bind_text_or_null(sqlite3_stmt *stmt, gint index, const gchar *value)
{
    if (value != NULL)
        sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, index);
}

static gchar *
column_text_or_null(sqlite3_stmt *stmt, gint index)
{
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL)
        return NULL;

    return g_strdup((const gchar *)sqlite3_column_text(stmt, index));
}

/* The columns every read selects, in the order notice_from_row() wants. */
#define NOTICE_COLUMNS \
    "id, agent, tier, event_kind, source, subject, title, created_at," \
    " read, dismissed"

static ClawtNotice *
notice_from_row(sqlite3_stmt *stmt)
{
    ClawtNotice *notice = g_new0(ClawtNotice, 1);

    notice->id = (gint64)sqlite3_column_int64(stmt, 0);
    notice->agent = column_text_or_null(stmt, 1);
    notice->tier = clawt_notice_tier_from_nick(
        (const gchar *)sqlite3_column_text(stmt, 2));
    notice->event_kind = column_text_or_null(stmt, 3);
    notice->source = column_text_or_null(stmt, 4);
    notice->subject = column_text_or_null(stmt, 5);
    notice->title = column_text_or_null(stmt, 6);
    notice->created_at = (gint64)sqlite3_column_int64(stmt, 7);
    notice->read = sqlite3_column_int(stmt, 8) != 0;
    notice->dismissed = sqlite3_column_int(stmt, 9) != 0;

    return notice;
}

gint64
clawt_notice_store_add(ClawtNoticeStore   *self,
                       const ClawtNotice  *notice,
                       gboolean            read,
                       GError            **error)
{
    sqlite3_stmt *stmt = NULL;
    gint rc;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), 0);
    g_return_val_if_fail(notice != NULL, 0);

    if (notice->title == NULL || notice->event_kind == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a notice needs a kind and words");
        return 0;
    }

    if (sqlite3_prepare_v2(self->db,
                           "INSERT INTO notices"
                           " (agent, tier, event_kind, source, subject,"
                           "  title, created_at, read, dismissed)"
                           " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 0)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing a notice");
        return 0;
    }

    bind_text_or_null(stmt, 1, notice->agent);
    sqlite3_bind_text(stmt, 2, clawt_notice_tier_nick(notice->tier), -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, notice->event_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, notice->source != NULL ? notice->source : "",
                      -1, SQLITE_TRANSIENT);
    bind_text_or_null(stmt, 5, notice->subject);
    sqlite3_bind_text(stmt, 6, notice->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)notice->created_at);
    sqlite3_bind_int(stmt, 8, read ? 1 : 0);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "writing a notice");
        return 0;
    }

    return (gint64)sqlite3_last_insert_rowid(self->db);
}

ClawtNotice *
clawt_notice_store_get(ClawtNoticeStore *self, gint64 id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtNotice *notice = NULL;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), NULL);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT " NOTICE_COLUMNS " FROM notices"
                           " WHERE id = ?1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        notice = notice_from_row(stmt);

    sqlite3_finalize(stmt);

    return notice;
}

GPtrArray *
clawt_notice_store_list(ClawtNoticeStore *self,
                        const gchar      *agent,
                        gboolean          include_dismissed,
                        guint             limit)
{
    GPtrArray *notices =
        g_ptr_array_new_with_free_func((GDestroyNotify)clawt_notice_free);
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), notices);

    /*
     * One statement for every combination: a NULL parameter means "any",
     * so the filter is in the query rather than in four spellings of it.
     * Newest first by id, which is insertion order -- the list is read
     * from the top, the opposite of a transcript.
     */
    if (sqlite3_prepare_v2(self->db,
                           "SELECT " NOTICE_COLUMNS " FROM notices"
                           " WHERE (?1 IS NULL OR agent = ?1)"
                           "   AND (?2 = 1 OR dismissed = 0)"
                           " ORDER BY id DESC LIMIT ?3",
                           -1, &stmt, NULL) != SQLITE_OK)
        return notices;

    bind_text_or_null(stmt, 1, agent);
    sqlite3_bind_int(stmt, 2, include_dismissed ? 1 : 0);
    sqlite3_bind_int64(stmt, 3, limit > 0 ? (sqlite3_int64)limit : -1);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(notices, notice_from_row(stmt));

    sqlite3_finalize(stmt);

    return notices;
}

/*
 * Runs one UPDATE or DELETE and counts what it changed.  The statement's
 * ?1 is @number and its ?2 is @text; one that names only ?2 still has
 * two parameters to SQLite, which counts by the highest index.
 */
static guint
run_change(ClawtNoticeStore *self, const gchar *sql, gint64 number,
           const gchar *text)
{
    sqlite3_stmt *stmt = NULL;
    guint changed = 0;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        g_warning("notices: could not prepare a change: %s",
                  sqlite3_errmsg(self->db));
        return 0;
    }

    if (sqlite3_bind_parameter_count(stmt) >= 1)
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)number);

    if (sqlite3_bind_parameter_count(stmt) >= 2)
        bind_text_or_null(stmt, 2, text);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        changed = (guint)sqlite3_changes(self->db);
    else
        g_warning("notices: a change failed: %s", sqlite3_errmsg(self->db));

    sqlite3_finalize(stmt);

    return changed;
}

guint
clawt_notice_store_mark_read(ClawtNoticeStore *self, gint64 id)
{
    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), 0);

    return run_change(self,
                      "UPDATE notices SET read = 1"
                      " WHERE read = 0 AND (?1 = 0 OR id = ?1)",
                      id, NULL);
}

gboolean
clawt_notice_store_dismiss(ClawtNoticeStore *self, gint64 id)
{
    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), FALSE);

    if (id <= 0)
        return FALSE;

    return run_change(self,
                      "UPDATE notices SET dismissed = 1, read = 1"
                      " WHERE id = ?1 AND dismissed = 0",
                      id, NULL) > 0;
}

guint
clawt_notice_store_clear(ClawtNoticeStore *self, const gchar *agent)
{
    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), 0);

    return run_change(self,
                      "UPDATE notices SET dismissed = 1, read = 1"
                      " WHERE dismissed = 0"
                      "   AND (?2 IS NULL OR agent = ?2)",
                      0, agent);
}

guint
clawt_notice_store_count_unread(ClawtNoticeStore *self, GHashTable *muted)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), 0);

    /*
     * The agent of each unread row rather than a COUNT, so the muted set
     * -- which lives in the daemon's terms, with lapsed mutes already
     * left out -- can be applied without a second copy of the lapse rule
     * in SQL.  The unread rows are few: opening the panel reads them.
     */
    if (sqlite3_prepare_v2(self->db,
                           "SELECT agent FROM notices"
                           " WHERE read = 0 AND dismissed = 0",
                           -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *agent = (const gchar *)sqlite3_column_text(stmt, 0);

        if (agent != NULL && muted != NULL &&
            g_hash_table_contains(muted, agent))
            continue;

        count++;
    }

    sqlite3_finalize(stmt);

    return count;
}

guint
clawt_notice_store_prune(ClawtNoticeStore *self,
                         guint             keep,
                         guint             keep_days,
                         gint64            now)
{
    guint removed = 0;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), 0);

    /*
     * By age first, then by count: the order does not change the result,
     * but counting what is left after the old ones go is the cheaper of
     * the two.
     */
    if (keep_days > 0)
        removed += run_change(self,
                              "DELETE FROM notices WHERE created_at < ?1",
                              now - (gint64)keep_days * DAY_SECONDS, NULL);

    if (keep > 0)
        removed += run_change(self,
                              "DELETE FROM notices WHERE id NOT IN"
                              " (SELECT id FROM notices"
                              "  ORDER BY id DESC LIMIT ?1)",
                              (gint64)keep, NULL);

    /* A lapsed mute is no mute; dropping the row keeps mutes[] honest. */
    run_change(self,
               "DELETE FROM mutes WHERE until > 0 AND until <= ?1",
               now, NULL);

    return removed;
}

guint
clawt_notice_store_count(ClawtNoticeStore *self)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), 0);

    if (sqlite3_prepare_v2(self->db, "SELECT COUNT(*) FROM notices", -1,
                           &stmt, NULL) != SQLITE_OK)
        return 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);

    return count;
}

/* ── Mutes ───────────────────────────────────────────────────────── */

gboolean
clawt_notice_store_mute(ClawtNoticeStore  *self,
                        const gchar       *agent,
                        gint64             until,
                        GError           **error)
{
    sqlite3_stmt *stmt = NULL;
    gint rc;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), FALSE);

    if (agent == NULL || *agent == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "which agent should be muted?");
        return FALSE;
    }

    if (sqlite3_prepare_v2(self->db,
                           "INSERT INTO mutes (agent, until) VALUES (?1, ?2)"
                           " ON CONFLICT(agent) DO UPDATE SET"
                           "  until = excluded.until",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(error, self->db, "preparing a mute");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)MAX(until, 0));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        set_sqlite_error(error, self->db, "writing a mute");
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_notice_store_unmute(ClawtNoticeStore *self, const gchar *agent)
{
    sqlite3_stmt *stmt = NULL;
    gboolean changed = FALSE;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), FALSE);

    if (agent == NULL)
        return FALSE;

    if (sqlite3_prepare_v2(self->db, "DELETE FROM mutes WHERE agent = ?1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        changed = sqlite3_changes(self->db) > 0;

    sqlite3_finalize(stmt);

    return changed;
}

/* The one rule for "in force", so is_muted and the listings agree. */
#define MUTE_IN_FORCE " (until = 0 OR until > ?1) "

gboolean
clawt_notice_store_is_muted(ClawtNoticeStore *self,
                            const gchar      *agent,
                            gint64            now)
{
    sqlite3_stmt *stmt = NULL;
    gboolean muted = FALSE;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), FALSE);

    if (agent == NULL || *agent == '\0')
        return FALSE;

    if (sqlite3_prepare_v2(self->db,
                           "SELECT 1 FROM mutes WHERE" MUTE_IN_FORCE
                           " AND agent = ?2",
                           -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);
    sqlite3_bind_text(stmt, 2, agent, -1, SQLITE_TRANSIENT);

    muted = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);

    return muted;
}

GPtrArray *
clawt_notice_store_list_mutes(ClawtNoticeStore *self, gint64 now)
{
    GPtrArray *mutes = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_notice_mute_free);
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), mutes);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT agent, until FROM mutes WHERE"
                           MUTE_IN_FORCE " ORDER BY agent",
                           -1, &stmt, NULL) != SQLITE_OK)
        return mutes;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ClawtNoticeMute *mute = g_new0(ClawtNoticeMute, 1);

        mute->agent = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        mute->until = (gint64)sqlite3_column_int64(stmt, 1);
        g_ptr_array_add(mutes, mute);
    }

    sqlite3_finalize(stmt);

    return mutes;
}

GHashTable *
clawt_notice_store_muted_set(ClawtNoticeStore *self, gint64 now)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            NULL);
    g_autoptr(GPtrArray) mutes = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), set);

    mutes = clawt_notice_store_list_mutes(self, now);

    for (i = 0; i < mutes->len; i++) {
        ClawtNoticeMute *mute = g_ptr_array_index(mutes, i);

        g_hash_table_add(set, g_strdup(mute->agent));
    }

    return set;
}

gint64
clawt_notice_store_next_lapse(ClawtNoticeStore *self, gint64 now)
{
    sqlite3_stmt *stmt = NULL;
    gint64 next = 0;

    g_return_val_if_fail(CLAWT_IS_NOTICE_STORE(self), 0);

    if (sqlite3_prepare_v2(self->db,
                           "SELECT MIN(until) FROM mutes"
                           " WHERE until > ?1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);

    if (sqlite3_step(stmt) == SQLITE_ROW &&
        sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        next = (gint64)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);

    return next;
}
