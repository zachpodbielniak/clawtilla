/*
 * clawt-decision-store.c - Decisions that outlive the agent that asked
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <sqlite3.h>

#include "clawtilla.h"
#include "decision/clawt-decision-store.h"

static const gchar *SCHEMA =
    "CREATE TABLE IF NOT EXISTS decisions ("
    "  id TEXT PRIMARY KEY,"
    "  agent TEXT NOT NULL,"
    "  question TEXT NOT NULL,"
    "  options TEXT,"
    "  default_option TEXT,"
    "  default_reason TEXT,"
    "  task TEXT,"
    "  answer TEXT,"
    "  reversible_until INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  answered_at INTEGER NOT NULL DEFAULT 0,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  room TEXT,"
    "  settled_via TEXT,"
    "  routed_to TEXT,"
    "  answered_by TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS decisions_state"
    "  ON decisions(state, reversible_until, created_at);";

/*
 * Columns added after the table first shipped, each with the statement
 * that adds it to a store written before it existed.
 *
 * CREATE TABLE IF NOT EXISTS does nothing to a table that is already
 * there, so a daemon upgraded over an old store would prepare every
 * SELECT against a column that is not on disk -- and the store answers
 * a failed prepare with an empty list, which reads as an inbox with
 * nothing in it rather than as a schema that never moved.
 */
static const struct {
    const gchar *column;
    const gchar *add;
} LATER_COLUMNS[] = {
    { "room",        "ALTER TABLE decisions ADD COLUMN room TEXT" },
    { "settled_via", "ALTER TABLE decisions ADD COLUMN settled_via TEXT" },
    /*
     * Since 0.22.0: who a decision raised inside delegated work was put
     * to first, and the agent that answered it.  Absent means every
     * stored decision went to the operator alone, which each one did.
     */
    { "routed_to",   "ALTER TABLE decisions ADD COLUMN routed_to TEXT" },
    { "answered_by", "ALTER TABLE decisions ADD COLUMN answered_by TEXT" }
};

/* Whether the decisions table on disk already has @column. */
static gboolean
has_column(sqlite3 *db, const gchar *column)
{
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    if (sqlite3_prepare_v2(db, "PRAGMA table_info(decisions)", -1, &stmt,
                           NULL) != SQLITE_OK)
        return FALSE;

    while (!found && sqlite3_step(stmt) == SQLITE_ROW)
        found = g_strcmp0((const gchar *)sqlite3_column_text(stmt, 1),
                          column) == 0;

    sqlite3_finalize(stmt);

    return found;
}

static gboolean
add_later_columns(sqlite3 *db, GError **error)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(LATER_COLUMNS); i++) {
        gchar *message = NULL;

        if (has_column(db, LATER_COLUMNS[i].column))
            continue;

        if (sqlite3_exec(db, LATER_COLUMNS[i].add, NULL, NULL, &message) !=
            SQLITE_OK) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not add '%s' to the decision store: %s",
                        LATER_COLUMNS[i].column,
                        message != NULL ? message : "unknown");
            sqlite3_free(message);
            return FALSE;
        }
    }

    return TRUE;
}

struct _ClawtDecisionStore {
    GObject  parent_instance;
    sqlite3 *db;
    gchar   *path;
};

G_DEFINE_FINAL_TYPE(ClawtDecisionStore, clawt_decision_store, G_TYPE_OBJECT)

static void
clawt_decision_store_dispose(GObject *object)
{
    ClawtDecisionStore *self = CLAWT_DECISION_STORE(object);

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

    G_OBJECT_CLASS(clawt_decision_store_parent_class)->dispose(object);
}

static void
clawt_decision_store_class_init(ClawtDecisionStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_decision_store_dispose;
}

static void
clawt_decision_store_init(ClawtDecisionStore *self)
{
    (void)self;
}

ClawtDecisionStore *
clawt_decision_store_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtDecisionStore) self = NULL;
    g_autofree gchar *dir = NULL;
    gchar *message = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_DECISION_STORE, NULL);
    self->path = g_strdup(path);

    dir = g_path_get_dirname(path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open the decision store: %s",
                    sqlite3_errmsg(self->db));
        return NULL;
    }

    /*
     * Written on the daemon's main loop, so a write must not wait on the
     * disk: WAL with synchronous=NORMAL appends a commit to the log and
     * syncs only at a checkpoint, as the task store and the mailbox
     * already do.  At SQLite's defaults every write fsync()ed a rollback
     * journal and the database, and on a saturated disk one fsync held
     * the loop -- and the control socket's accept -- for tens of seconds
     * (tests/test-store-sync.c).  A power cut can lose the last commits;
     * it cannot corrupt the file.
     */
    sqlite3_exec(self->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(self->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    if (sqlite3_exec(self->db, SCHEMA, NULL, NULL, &message) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create the decision schema: %s",
                    message != NULL ? message : "unknown");
        sqlite3_free(message);
        return NULL;
    }

    if (!add_later_columns(self->db, error))
        return NULL;

    return g_steal_pointer(&self);
}

/* The options as one string, since sqlite has no list type. */
static gchar *
options_join(ClawtDecision *decision)
{
    const gchar * const *options = clawt_decision_get_options(decision);

    if (options == NULL)
        return NULL;

    /*
     * A newline rather than a comma: an option is a phrase a person
     * reads, and "yes, with caveats" is exactly the kind of thing an
     * agent writes.  A comma would split it in half on the way back.
     */
    return g_strjoinv("\n", (GStrv)options);
}

static void
options_apply(ClawtDecision *decision, const gchar *joined)
{
    g_auto(GStrv) options = NULL;

    if (joined == NULL || *joined == '\0')
        return;

    options = g_strsplit(joined, "\n", -1);
    clawt_decision_set_options(decision, (const gchar * const *)options);
}

gchar *
clawt_decision_store_post(
    ClawtDecisionStore *self,
    ClawtDecision      *decision,
    GError            **error
){
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *joined = NULL;
    gchar *id;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), NULL);
    g_return_val_if_fail(decision != NULL, NULL);

    if (sqlite3_prepare_v2(self->db,
            "INSERT OR REPLACE INTO decisions"
            " (id, agent, question, options, default_option, default_reason,"
            "  task, answer, reversible_until, created_at, answered_at,"
            "  state, room, settled_via, routed_to, answered_by)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not record the decision: %s",
                    sqlite3_errmsg(self->db));
        return NULL;
    }

    joined = options_join(decision);

    sqlite3_bind_text(stmt, 1, clawt_decision_get_id(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, clawt_decision_get_agent(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, clawt_decision_get_question(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, joined, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, clawt_decision_get_default(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, clawt_decision_get_default_reason(decision),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, clawt_decision_get_task(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, clawt_decision_get_answer(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 9,
                       clawt_decision_get_reversible_until(decision));
    sqlite3_bind_int64(stmt, 10, clawt_decision_get_created_at(decision));
    sqlite3_bind_int64(stmt, 11, clawt_decision_get_answered_at(decision));
    sqlite3_bind_int(stmt, 12, (gint)clawt_decision_get_state(decision));
    sqlite3_bind_text(stmt, 13, clawt_decision_get_room(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, clawt_decision_get_settled_via(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, clawt_decision_get_routed_to(decision), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, clawt_decision_get_answered_by(decision), -1,
                      SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not record the decision: %s",
                    sqlite3_errmsg(self->db));
        sqlite3_finalize(stmt);
        return NULL;
    }

    id = g_strdup(clawt_decision_get_id(decision));
    sqlite3_finalize(stmt);

    return id;
}

static const gchar *
column_text(sqlite3_stmt *stmt, gint index)
{
    return (const gchar *)sqlite3_column_text(stmt, index);
}

static ClawtDecision *
decision_from_row(sqlite3_stmt *stmt)
{
    ClawtDecision *decision = clawt_decision_new(column_text(stmt, 0),
                                                 column_text(stmt, 1),
                                                 column_text(stmt, 2));

    options_apply(decision, column_text(stmt, 3));
    clawt_decision_set_default(decision, column_text(stmt, 4),
                               column_text(stmt, 5));
    clawt_decision_set_task(decision, column_text(stmt, 6));
    clawt_decision_set_reversible_until(decision,
                                        sqlite3_column_int64(stmt, 8));
    clawt_decision_set_created_at(decision, sqlite3_column_int64(stmt, 9));

    /*
     * The answer is applied before the state, because
     * clawt_decision_answer() sets the state to ANSWERED -- and a row
     * that was DEFAULTED or DISMISSED would come back as answered,
     * which is the one distinction this type exists to keep.
     */
    if (column_text(stmt, 7) != NULL)
        clawt_decision_answer(decision, column_text(stmt, 7),
                              sqlite3_column_int64(stmt, 10));

    clawt_decision_set_answered_at(decision, sqlite3_column_int64(stmt, 10));
    clawt_decision_set_state(
        decision, (ClawtDecisionState)sqlite3_column_int(stmt, 11));
    clawt_decision_set_room(decision, column_text(stmt, 12));
    clawt_decision_set_settled_via(decision, column_text(stmt, 13));
    clawt_decision_set_routed_to(decision, column_text(stmt, 14));
    clawt_decision_set_answered_by(decision, column_text(stmt, 15));

    return decision;
}

#define SELECT_COLUMNS \
    "SELECT id, agent, question, options, default_option, default_reason," \
    " task, answer, reversible_until, created_at, answered_at, state," \
    " room, settled_via, routed_to, answered_by" \
    " FROM decisions"

GPtrArray *
clawt_decision_store_list(ClawtDecisionStore *self, gboolean open_only)
{
    GPtrArray *out;
    sqlite3_stmt *stmt = NULL;
    const gchar *sql;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_decision_free);

    /*
     * A stated deadline first and the soonest of those at the top, then
     * everything undated by age.  `reversible_until = 0` sorting last
     * is why the CASE is there: plain ASC would put every item that
     * named no deadline above every item that did.
     */
    sql = open_only
        ? SELECT_COLUMNS " WHERE state = 0"
          " ORDER BY CASE WHEN reversible_until > 0 THEN 0 ELSE 1 END,"
          " reversible_until ASC, created_at ASC"
        : SELECT_COLUMNS
          " ORDER BY state ASC,"
          " CASE WHEN reversible_until > 0 THEN 0 ELSE 1 END,"
          " reversible_until ASC, created_at ASC";

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return out;

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, decision_from_row(stmt));

    sqlite3_finalize(stmt);

    return out;
}

GPtrArray *
clawt_decision_store_list_room(ClawtDecisionStore *self, const gchar *room)
{
    GPtrArray *out;
    sqlite3_stmt *stmt = NULL;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_decision_free);

    if (room == NULL || *room == '\0')
        return out;

    /*
     * Every state and in the order asked, because this is a
     * conversation's view and a conversation is read in time order: a
     * settled one stays where it was asked, showing how it ended, rather
     * than vanishing out of the middle of the transcript.
     */
    if (sqlite3_prepare_v2(self->db,
                           SELECT_COLUMNS " WHERE room = ?"
                           " ORDER BY created_at ASC, id ASC",
                           -1, &stmt, NULL) != SQLITE_OK)
        return out;

    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(out, decision_from_row(stmt));

    sqlite3_finalize(stmt);

    return out;
}

ClawtDecision *
clawt_decision_store_get(ClawtDecisionStore *self, const gchar *id)
{
    sqlite3_stmt *stmt = NULL;
    ClawtDecision *decision = NULL;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    if (sqlite3_prepare_v2(self->db, SELECT_COLUMNS " WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        decision = decision_from_row(stmt);

    sqlite3_finalize(stmt);

    return decision;
}

static ClawtDecision *answer_full(ClawtDecisionStore *self, const gchar *id,
                                  const gchar *answer, const gchar *via,
                                  const gchar *who, GError **error);

ClawtDecision *
clawt_decision_store_answer(
    ClawtDecisionStore *self,
    const gchar        *id,
    const gchar        *answer,
    GError            **error
){
    return answer_full(self, id, answer, NULL, NULL, error);
}

ClawtDecision *
clawt_decision_store_answer_via(
    ClawtDecisionStore *self,
    const gchar        *id,
    const gchar        *answer,
    const gchar        *via,
    GError            **error
){
    return answer_full(self, id, answer, via, NULL, error);
}

ClawtDecision *
clawt_decision_store_answer_as(
    ClawtDecisionStore *self,
    const gchar        *id,
    const gchar        *answer,
    const gchar        *who,
    GError            **error
){
    return answer_full(self, id, answer, NULL, who, error);
}

/*
 * Every answer, from whichever surface and whoever gives it: a person
 * through the chat, the inbox or the CLI (@via), or the delegator a
 * decision was routed to (@who).  One body, so the refusal of a second
 * answer is the same sentence whoever pressed.
 */
static ClawtDecision *
answer_full(
    ClawtDecisionStore *self,
    const gchar        *id,
    const gchar        *answer,
    const gchar        *via,
    const gchar        *who,
    GError            **error
){
    g_autoptr(ClawtDecision) decision = NULL;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    decision = clawt_decision_store_get(self, id);

    if (decision == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no decision called '%s'", id);
        return NULL;
    }

    /*
     * Answering twice is refused rather than allowed to overwrite.  The
     * first answer has already been routed to the agent and may already
     * have changed what it did, so a second one would be a change of
     * mind that nothing downstream would ever hear about -- which is
     * worse than being told to raise it again.
     */
    /*
     * The refusal says what *did* happen, because the person reading it
     * pressed a button on a view that was out of date -- a second
     * window, the inbox, or the clock got there first -- and "already
     * settled" alone sends them looking for which.  The same text is
     * what a double tap gets, since the second press of one is exactly
     * a press on a decision the first one settled.
     */
    if (clawt_decision_get_state(decision) != CLAWT_DECISION_OPEN) {
        g_autofree gchar *refusal = clawt_decision_refusal_text(decision);

        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, refusal);
        return NULL;
    }

    clawt_decision_answer(decision, answer,
                          g_get_real_time() / G_USEC_PER_SEC);
    clawt_decision_set_settled_via(decision, via);
    clawt_decision_set_answered_by(decision, who);

    /*
     * The id is transfer-full and is not wanted here -- the caller
     * already has the decision.  Held in an autofree rather than
     * discarded, which leaked one id per answer.
     */
    {
        g_autofree gchar *stored =
            clawt_decision_store_post(self, decision, error);

        if (stored == NULL)
            return NULL;
    }

    return g_steal_pointer(&decision);
}

gboolean
clawt_decision_store_dismiss(
    ClawtDecisionStore *self,
    const gchar        *id,
    GError            **error
){
    g_autoptr(ClawtDecision) decision = NULL;
    g_autofree gchar *stored = NULL;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    decision = clawt_decision_store_get(self, id);

    if (decision == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no decision called '%s'", id);
        return FALSE;
    }

    clawt_decision_set_state(decision, CLAWT_DECISION_DISMISSED);
    stored = clawt_decision_store_post(self, decision, error);

    return stored != NULL;
}

gboolean
clawt_decision_store_delete(
    ClawtDecisionStore *self,
    const gchar        *id,
    GError            **error
){
    sqlite3_stmt *stmt = NULL;
    gint rc;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    if (sqlite3_prepare_v2(self->db, "DELETE FROM decisions WHERE id = ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not delete '%s': %s", id, sqlite3_errmsg(self->db));
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /*
     * "It said no" and "it did not say" kept apart: a failed write is not
     * a missing decision, or a client would report a row as already gone
     * while it is still on disk.
     */
    if (rc != SQLITE_DONE) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not delete '%s': %s", id, sqlite3_errmsg(self->db));
        return FALSE;
    }

    if (sqlite3_changes(self->db) == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no decision called '%s'", id);
        return FALSE;
    }

    return TRUE;
}

ClawtDecision *
clawt_decision_store_settle_default(
    ClawtDecisionStore *self,
    const gchar        *id,
    GError            **error
){
    g_autoptr(ClawtDecision) decision = NULL;
    g_autofree gchar *stored = NULL;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), NULL);
    g_return_val_if_fail(id != NULL, NULL);

    decision = clawt_decision_store_get(self, id);
    if (decision == NULL || clawt_decision_get_state(decision) != CLAWT_DECISION_OPEN ||
        clawt_decision_get_default(decision) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "'%s' is not an open decision with a default", id);
        return NULL;
    }

    clawt_decision_answer(decision, clawt_decision_get_default(decision),
                          g_get_real_time() / G_USEC_PER_SEC);
    clawt_decision_set_state(decision, CLAWT_DECISION_DEFAULTED);

    stored = clawt_decision_store_post(self, decision, error);
    if (stored == NULL)
        return NULL;

    return g_steal_pointer(&decision);
}

guint
clawt_decision_store_count_open(ClawtDecisionStore *self)
{
    sqlite3_stmt *stmt = NULL;
    guint count = 0;

    g_return_val_if_fail(CLAWT_IS_DECISION_STORE(self), 0);

    if (sqlite3_prepare_v2(self->db,
            "SELECT COUNT(*) FROM decisions WHERE state = 0",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);

    return count;
}
