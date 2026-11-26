/*
 * clawt-task-store.h - Every task, and every problem report, on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Tasks and the incidents that describe them were held in memory, and a
 * daemon restart cleared both: a delegator waiting on work could no
 * longer ask what became of it, a parent waiting on its children forgot
 * them, and the operator's Tasks list came back empty.  This is the
 * durable half.  #ClawtTaskManager still serves every live question from
 * memory; it writes each change through to here, loads what is live from
 * here on start, and reads anything older from here on demand.
 *
 * A task is stored as two things at once: a few indexed columns that
 * lists filter and order on, and the whole task as clawt_task_to_json()
 * writes it.  The columns are for finding a row; the JSON is what is
 * restored.  So a field added to #ClawtTask later needs no migration --
 * it is one more member, read as its zero value from older rows.
 *
 * Every change to a task also leaves a journal row: when, the state it
 * was left in, and what happened.  That is the task's history, which a
 * single row cannot be, and which the operator reads to see how a piece
 * of work got where it is.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "task/clawt-task.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TASK_STORE (clawt_task_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtTaskStore, clawt_task_store, CLAWT, TASK_STORE,
                     GObject)

/**
 * ClawtTaskQuery:
 * @assignee: (nullable): only tasks assigned to this agent
 * @state: only tasks in this #ClawtTaskState, or -1 for any
 * @before: only tasks created before this Unix time, or 0 for no bound;
 *   the paging cursor, given the oldest @created_at of the last page
 * @limit: at most this many, or 0 for the default of 100
 *
 * Narrows clawt_task_store_list().  Zeroed is "everything, newest first,
 * one page".
 */
typedef struct {
    const gchar *assignee;
    gint         state;
    gint64       before;
    guint        limit;
} ClawtTaskQuery;

/**
 * CLAWT_TASK_STORE_PAGE:
 *
 * How many tasks one page holds when a query does not say.
 */
#define CLAWT_TASK_STORE_PAGE (100)

/**
 * clawt_task_store_new:
 * @path: the database file, created with its directory when missing
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the store, or %NULL when the file
 *   cannot be opened or its schema applied
 */
ClawtTaskStore *clawt_task_store_new(const gchar *path, GError **error);

/**
 * clawt_task_store_get_path:
 * @self: a #ClawtTaskStore
 *
 * Returns: (transfer none): the database file
 */
const gchar *clawt_task_store_get_path(ClawtTaskStore *self);

/**
 * clawt_task_store_save:
 * @self: a #ClawtTaskStore
 * @task: the task as it is now
 * @what: (nullable): what just happened to it, for the journal
 * @error: (out) (optional): return location for a #GError
 *
 * Writes @task over its stored copy, or adds it.
 *
 * A journal row is added in the same transaction when the state changed
 * from what was stored, or when @what is given: a save that only
 * refreshes a field the state machine did not move (a model, a room) is
 * not an event in the task's history.
 *
 * Returns: %TRUE when written
 */
gboolean clawt_task_store_save(ClawtTaskStore  *self,
                               ClawtTask       *task,
                               const gchar     *what,
                               GError         **error);

/**
 * clawt_task_store_get:
 * @self: a #ClawtTaskStore
 * @task_id: which task
 *
 * Returns: (transfer full) (nullable): a copy of the stored task, or
 *   %NULL when there is none or it could not be read
 */
ClawtTask *clawt_task_store_get(ClawtTaskStore *self, const gchar *task_id);

/**
 * clawt_task_store_load_live:
 * @self: a #ClawtTaskStore
 * @finished_since: also the tasks that finished at or after this Unix
 *   time
 * @error: (out) (optional): return location for a #GError
 *
 * What a starting daemon keeps in memory: every task that has not
 * finished, and the ones that finished recently enough to still be on a
 * client's list.  Oldest first, which is the order the manager lists
 * from.  A row that cannot be read is skipped with a warning naming it,
 * not allowed to cost every other task.
 *
 * Returns: (transfer full) (element-type ClawtTask) (nullable): the
 *   tasks, or %NULL on error
 */
GPtrArray *clawt_task_store_load_live(ClawtTaskStore  *self,
                                      gint64           finished_since,
                                      GError         **error);

/**
 * clawt_task_store_list:
 * @self: a #ClawtTaskStore
 * @query: (nullable): what to narrow to, or %NULL for one page of all
 * @error: (out) (optional): return location for a #GError
 *
 * Tasks newest first, one page.  The next page is the same query with
 * @before set to the last task's created_at; tasks created in the same
 * second as that boundary are ordered by insertion and are on one side
 * of it or the other, never both.
 *
 * Returns: (transfer full) (element-type ClawtTask) (nullable): the
 *   tasks, or %NULL on error
 */
GPtrArray *clawt_task_store_list(ClawtTaskStore        *self,
                                 const ClawtTaskQuery  *query,
                                 GError               **error);

/**
 * clawt_task_store_journal:
 * @self: a #ClawtTaskStore
 * @task_id: which task
 * @error: (out) (optional): return location for a #GError
 *
 * The task's history, oldest first: an array of objects with `at` (Unix
 * time), `state` (nickname) and, when one was recorded, `what`.
 *
 * Returns: (transfer full) (nullable): an array node, empty for a task
 *   with no history, or %NULL on error
 */
/**
 * clawt_task_store_project_tasks:
 * @self: a #ClawtTaskStore
 * @project: a project id (`projects.id`)
 * @since: only tasks created at or after this Unix time
 * @error: (out) (optional): return location for a #GError
 *
 * Every task of @project made since @since, oldest first, without a page
 * limit: what a project's month of spend is summed over (since 0.40.0).
 *
 * Returns: (transfer full) (element-type ClawtTask) (nullable): the tasks
 */
GPtrArray *clawt_task_store_project_tasks(ClawtTaskStore  *self,
                                          const gchar     *project,
                                          gint64           since,
                                          GError         **error);

JsonNode *clawt_task_store_journal(ClawtTaskStore  *self,
                                   const gchar     *task_id,
                                   GError         **error);

/**
 * clawt_task_store_prune:
 * @self: a #ClawtTaskStore
 * @finished_before: remove tasks that finished before this Unix time
 * @error: (out) (optional): return location for a #GError
 *
 * Forgets finished tasks older than the retention window, with their
 * journals.  A task that has not finished is never removed, however old:
 * somebody may still be waiting on it.
 *
 * Returns: how many tasks were removed
 */
guint clawt_task_store_prune(ClawtTaskStore  *self,
                             gint64           finished_before,
                             GError         **error);

/**
 * CLAWT_TASK_NOTE_COMMENT:
 *
 * A note somebody wrote on a task: the operator, or an agent asked to.
 */
#define CLAWT_TASK_NOTE_COMMENT "comment"

/**
 * CLAWT_TASK_NOTE_PROGRESS:
 *
 * The assignee's word on work it has not finished
 * (clawtilla_task_progress).
 */
#define CLAWT_TASK_NOTE_PROGRESS "progress"

/**
 * CLAWT_TASK_NOTE_RESULT:
 *
 * What a task finished with.  One per attempt: a retried task that
 * finishes again adds another.
 */
#define CLAWT_TASK_NOTE_RESULT "result"

/**
 * CLAWT_TASK_NOTE_REVIEW:
 *
 * A reviewer's verdict on a result: an accept, or the changes asked for.
 */
#define CLAWT_TASK_NOTE_REVIEW "review"

/**
 * clawt_task_store_add_note:
 * @self: a #ClawtTaskStore
 * @task_id: which task
 * @author: (nullable): who wrote it -- an agent id, `user` for the
 *   operator, or %NULL for clawtilla itself
 * @kind: what it is, one of the CLAWT_TASK_NOTE_* spellings or a later
 *   one; the store keeps whatever it is given
 * @body: the text
 * @error: (out) (optional): return location for a #GError
 *
 * Adds a note to a task's thread.  Notes are what was *said* about a
 * task, where the journal is what *happened* to it: a comment, each
 * progress report, each result.  Kept and pruned with the task.
 *
 * Returns: the note's id, or 0 on error
 */
gint64 clawt_task_store_add_note(ClawtTaskStore  *self,
                                 const gchar     *task_id,
                                 const gchar     *author,
                                 const gchar     *kind,
                                 const gchar     *body,
                                 GError         **error);

/**
 * clawt_task_store_notes:
 * @self: a #ClawtTaskStore
 * @task_id: which task
 * @error: (out) (optional): return location for a #GError
 *
 * A task's notes, oldest first: an array of objects with `id`, `at`
 * (Unix time), `author` (absent for clawtilla's own), `kind` and `body`.
 *
 * Returns: (transfer full) (nullable): an array node, empty when there
 *   are none, or %NULL on error
 */
JsonNode *clawt_task_store_notes(ClawtTaskStore  *self,
                                 const gchar     *task_id,
                                 GError         **error);

/**
 * clawt_task_store_add_link:
 * @self: a #ClawtTaskStore
 * @blocker: the task that must finish first
 * @blocked: the task that waits for it
 * @error: (out) (optional): return location for a #GError
 *
 * Records that @blocked waits for @blocker.  Adding a link that exists is
 * not an error.  Whether the link would make a cycle is the manager's
 * question (clawt_task_manager_link()); the store keeps what it is told.
 *
 * Returns: %TRUE when the link is there
 */
gboolean clawt_task_store_add_link(ClawtTaskStore  *self,
                                   const gchar     *blocker,
                                   const gchar     *blocked,
                                   GError         **error);

/**
 * clawt_task_store_remove_link:
 * @self: a #ClawtTaskStore
 * @blocker: the task that was to finish first
 * @blocked: the task that waited for it
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when there is no longer such a link
 */
gboolean clawt_task_store_remove_link(ClawtTaskStore  *self,
                                      const gchar     *blocker,
                                      const gchar     *blocked,
                                      GError         **error);

/**
 * clawt_task_store_blockers_of:
 * @self: a #ClawtTaskStore
 * @task_id: a task
 *
 * Returns: (transfer full) (element-type utf8): the ids of the tasks
 *   @task_id waits for, in the order they were linked
 */
GPtrArray *clawt_task_store_blockers_of(ClawtTaskStore *self,
                                        const gchar    *task_id);

/**
 * clawt_task_store_blocked_by:
 * @self: a #ClawtTaskStore
 * @task_id: a task
 *
 * Returns: (transfer full) (element-type utf8): the ids of the tasks
 *   waiting for @task_id
 */
GPtrArray *clawt_task_store_blocked_by(ClawtTaskStore *self,
                                       const gchar    *task_id);

/**
 * clawt_task_store_hold_delivery:
 * @self: a #ClawtTaskStore
 * @task_id: a blocked task
 * @delivery: everything needed to hand the work over later, as the
 *   caller wrote it
 * @error: (out) (optional): return location for a #GError
 *
 * Keeps the delivery a blocked task will be handed over with, so it
 * survives a restart while it waits.  One per task; holding again
 * replaces it.
 *
 * Returns: %TRUE when kept
 */
gboolean clawt_task_store_hold_delivery(ClawtTaskStore  *self,
                                        const gchar     *task_id,
                                        JsonNode        *delivery,
                                        GError         **error);

/**
 * clawt_task_store_take_delivery:
 * @self: a #ClawtTaskStore
 * @task_id: a blocked task
 *
 * Takes the held delivery out of the store: it is deleted as it is read,
 * because a delivery released twice is work done twice.
 *
 * Returns: (transfer full) (nullable): the delivery, or %NULL when none
 *   is held
 */
JsonNode *clawt_task_store_take_delivery(ClawtTaskStore *self,
                                         const gchar    *task_id);

/**
 * clawt_task_store_has_delivery:
 * @self: a #ClawtTaskStore
 * @task_id: a task
 *
 * Returns: %TRUE when a delivery is held for @task_id
 */
gboolean clawt_task_store_has_delivery(ClawtTaskStore *self,
                                       const gchar    *task_id);

/**
 * clawt_task_store_save_incident:
 * @self: a #ClawtTaskStore
 * @incident_id: the incident's id
 * @opened_at: when it opened, which is the order incidents are listed in
 * @data: what the incident desk wrote for it
 * @error: (out) (optional): return location for a #GError
 *
 * Writes one incident over its stored copy, or adds it.  The desk owns
 * the shape of @data; the store keeps it whole.
 *
 * Returns: %TRUE when written
 */
gboolean clawt_task_store_save_incident(ClawtTaskStore  *self,
                                        const gchar     *incident_id,
                                        gint64           opened_at,
                                        JsonNode        *data,
                                        GError         **error);

/**
 * clawt_task_store_delete_incident:
 * @self: a #ClawtTaskStore
 * @incident_id: the incident's id
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when there is no longer such a row
 */
gboolean clawt_task_store_delete_incident(ClawtTaskStore  *self,
                                          const gchar     *incident_id,
                                          GError         **error);

/**
 * clawt_task_store_load_incidents:
 * @self: a #ClawtTaskStore
 * @error: (out) (optional): return location for a #GError
 *
 * Every stored incident, oldest first, as the desk wrote it.
 *
 * Returns: (transfer full) (element-type JsonNode) (nullable): the
 *   nodes, or %NULL on error
 */
GPtrArray *clawt_task_store_load_incidents(ClawtTaskStore  *self,
                                           GError         **error);

/* ── The VENTURE mirror (0.44.0) ─────────────────────────────────── */

/**
 * ClawtVentureOutboxRow:
 * @id: the row
 * @task_id: the task it is about
 * @kind: `sync` (the ticket should match the task) or `note` (one note
 *   should become a comment)
 * @note_id: the note, for `note`; 0 otherwise
 * @attempts: failed sends so far
 * @gen: the generation read; see clawt_task_store_outbox_finish()
 *
 * One thing the VENTURE mirror owes VENTURE.
 */
typedef struct {
    gint64  id;
    gchar  *task_id;
    gchar  *kind;
    gint64  note_id;
    gint    attempts;
    gint64  gen;
} ClawtVentureOutboxRow;

/**
 * clawt_venture_outbox_row_free: (skip)
 * @row: (nullable) (transfer full): a row
 */
void clawt_venture_outbox_row_free(ClawtVentureOutboxRow *row);

/**
 * ClawtVentureRef:
 * @task_id: the task
 * @instance: (nullable): the connector the ticket is on
 * @ticket_id: the VENTURE ticket, or 0 when there is none
 * @organization_id: the VENTURE organization the ticket is in, or 0 when
 *   not known -- what the link switches the browser's scope to
 * @version: the ticket's version after the mirror's last write
 * @snapshot: (nullable): the mirror-owned fields as the mirror last
 *   wrote them, a JSON object -- its write receipt
 * @state: `synced`, `skipped` (not mirrored, and @reason says why) or
 *   `dead` (the mirror gave up; @reason says why)
 * @reason: (nullable): why, for `skipped` and `dead`
 * @url: (nullable): the ticket's page
 * @updated_at: when the ref was last written
 *
 * What the mirror knows of one task in VENTURE.
 */
typedef struct {
    gchar  *task_id;
    gchar  *instance;
    gint64  ticket_id;
    gint64  organization_id;
    gint64  version;
    gchar  *snapshot;
    gchar  *state;
    gchar  *reason;
    gchar  *url;
    gint64  updated_at;
} ClawtVentureRef;

/**
 * clawt_venture_ref_free: (skip)
 * @ref: (nullable) (transfer full): a ref
 */
void clawt_venture_ref_free(ClawtVentureRef *ref);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtVentureRef, clawt_venture_ref_free)

/**
 * clawt_task_store_set_mirror:
 * @self: a #ClawtTaskStore
 * @mirror: whether the VENTURE mirror is on
 *
 * With the mirror on, every task save queues a coalesced `sync` row and
 * every note a `note` row, each in the same transaction as the change.
 */
void clawt_task_store_set_mirror(ClawtTaskStore *self, gboolean mirror);

/**
 * clawt_task_store_get_mirror:
 * @self: a #ClawtTaskStore
 *
 * Returns: whether outbox rows are being written
 */
gboolean clawt_task_store_get_mirror(ClawtTaskStore *self);

/**
 * clawt_task_store_outbox_due: (skip)
 * @self: a #ClawtTaskStore
 * @now: Unix time
 * @limit: at most this many
 * @error: (out) (optional): return location for a #GError
 *
 * The rows to send now: the oldest queued row of each task, when due.
 *
 * Returns: (transfer full) (element-type ClawtVentureOutboxRow)
 *   (nullable): the rows, or %NULL on error
 */
GPtrArray *clawt_task_store_outbox_due(ClawtTaskStore  *self,
                                       gint64           now,
                                       guint            limit,
                                       GError         **error);

/**
 * clawt_task_store_outbox_finish:
 * @self: a #ClawtTaskStore
 * @id: the row
 * @gen: the generation that was sent
 *
 * The row was sent.  It is deleted -- unless its generation moved on
 * while it was in flight, when it is made due again for the newer
 * change.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_task_store_outbox_finish(ClawtTaskStore *self, gint64 id,
                                        gint64 gen);

/**
 * clawt_task_store_outbox_retry:
 * @self: a #ClawtTaskStore
 * @id: the row
 * @attempts: failed sends now
 * @next_at: when to try again, Unix time
 * @why: (nullable): what went wrong
 *
 * Returns: %TRUE on success
 */
gboolean clawt_task_store_outbox_retry(ClawtTaskStore *self, gint64 id,
                                       gint attempts, gint64 next_at,
                                       const gchar *why);

/**
 * clawt_task_store_outbox_kill:
 * @self: a #ClawtTaskStore
 * @id: the row
 * @why: what went wrong, for the operator
 *
 * The row is dead: kept, with @why, until the operator retries the task.
 * The task's later rows wait behind it.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_task_store_outbox_kill(ClawtTaskStore *self, gint64 id,
                                      const gchar *why);

/**
 * clawt_task_store_outbox_revive:
 * @self: a #ClawtTaskStore
 * @task_id: the task
 *
 * The operator's Retry: the task's dead rows are queued again, due now.
 *
 * Returns: how many were revived
 */
guint clawt_task_store_outbox_revive(ClawtTaskStore *self,
                                     const gchar *task_id);

/**
 * clawt_task_store_outbox_state:
 * @self: a #ClawtTaskStore
 * @task_id: the task
 * @queued: (out) (optional): rows still to send
 * @dead: (out) (optional): rows given up on
 * @last_error: (out) (optional) (transfer full) (nullable): the latest
 *   error any of them records
 */
void clawt_task_store_outbox_state(ClawtTaskStore  *self,
                                   const gchar     *task_id,
                                   guint           *queued,
                                   guint           *dead,
                                   gchar          **last_error);

/**
 * clawt_task_store_venture_ref: (skip)
 * @self: a #ClawtTaskStore
 * @task_id: the task
 *
 * Returns: (transfer full) (nullable): what the mirror knows of it, or
 *   %NULL when it has never looked
 */
ClawtVentureRef *clawt_task_store_venture_ref(ClawtTaskStore *self,
                                              const gchar *task_id);

/**
 * clawt_task_store_put_venture_ref: (skip)
 * @self: a #ClawtTaskStore
 * @ref: the ref to keep, over any for the same task
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when written
 */
gboolean clawt_task_store_put_venture_ref(ClawtTaskStore        *self,
                                          const ClawtVentureRef *ref,
                                          GError               **error);

/**
 * clawt_task_store_venture_cursor:
 * @self: a #ClawtTaskStore
 * @key: which cursor
 *
 * Returns: (transfer full) (nullable): where the VENTURE poll got to
 */
gchar *clawt_task_store_venture_cursor(ClawtTaskStore *self,
                                       const gchar    *key);

/**
 * clawt_task_store_put_venture_cursor:
 * @self: a #ClawtTaskStore
 * @key: which cursor
 * @value: where it got to
 */
void clawt_task_store_put_venture_cursor(ClawtTaskStore *self,
                                         const gchar    *key,
                                         const gchar    *value);

/**
 * clawt_task_store_team_selinux_levels:
 * @self: a #ClawtTaskStore
 *
 * Every team's stored SELinux MCS level; see
 * computer.container.selinux_level.
 *
 * Returns: (transfer full) (element-type utf8 utf8): team id to level,
 *   empty when no team has one yet
 */
GHashTable *clawt_task_store_team_selinux_levels(ClawtTaskStore *self);

/**
 * clawt_task_store_team_selinux_level:
 * @self: a #ClawtTaskStore
 * @team: the team
 *
 * Returns: (transfer full) (nullable): the team's stored level, or %NULL
 *   when it has none yet
 */
gchar *clawt_task_store_team_selinux_level(ClawtTaskStore *self,
                                           const gchar    *team);

/**
 * clawt_task_store_add_team_selinux_level:
 * @self: a #ClawtTaskStore
 * @team: the team
 * @level: its newly allocated level
 * @error: (out) (optional): return location for a #GError
 *
 * Stores a team's level once.  Refused when the team already has one or
 * another team holds @level, so a level is never silently replaced.
 *
 * Returns: %TRUE when written
 */
gboolean clawt_task_store_add_team_selinux_level(ClawtTaskStore  *self,
                                                 const gchar     *team,
                                                 const gchar     *level,
                                                 GError         **error);

/**
 * clawt_task_store_venture_live_tickets:
 * @self: a #ClawtTaskStore
 *
 * The tickets of mirrored tasks that have not finished: ticket id (as
 * decimal text) to task id.  Whose comments the mirror reads back.
 *
 * Returns: (transfer full) (element-type utf8 utf8): the map, empty for
 *   none
 */
GHashTable *clawt_task_store_venture_live_tickets(ClawtTaskStore *self);

/**
 * clawt_task_store_venture_task_for_ticket:
 * @self: a #ClawtTaskStore
 * @ticket_id: a VENTURE ticket
 *
 * Returns: (transfer full) (nullable): the task mirrored as it, or %NULL
 */
gchar *clawt_task_store_venture_task_for_ticket(ClawtTaskStore *self,
                                                gint64          ticket_id);

/**
 * clawt_task_store_note:
 * @self: a #ClawtTaskStore
 * @note_id: the note
 *
 * One note, shaped as clawt_task_store_notes() shapes each, plus its
 * `task_id`.
 *
 * Returns: (transfer full) (nullable): the note, or %NULL
 */
JsonNode *clawt_task_store_note(ClawtTaskStore *self, gint64 note_id);

/**
 * clawt_task_store_venture_comment:
 * @self: a #ClawtTaskStore
 * @note_id: the note
 *
 * Returns: the VENTURE comment the mirror made of it, or 0
 */
gint64 clawt_task_store_venture_comment(ClawtTaskStore *self,
                                        gint64 note_id);

/**
 * clawt_task_store_put_venture_comment:
 * @self: a #ClawtTaskStore
 * @note_id: the note
 * @task_id: its task
 * @comment_id: the VENTURE comment made of it
 *
 * The mirror's receipt for a comment: what lets it recognise its own
 * comment when it comes back.
 *
 * Returns: %TRUE when written
 */
gboolean clawt_task_store_put_venture_comment(ClawtTaskStore *self,
                                              gint64 note_id,
                                              const gchar *task_id,
                                              gint64 comment_id);

G_END_DECLS
