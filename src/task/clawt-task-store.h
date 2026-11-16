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

G_END_DECLS
