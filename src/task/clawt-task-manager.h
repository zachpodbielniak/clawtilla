/*
 * clawt-task-manager.h - Every delegated task
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "task/clawt-task.h"
#include "task/clawt-task-store.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TASK_MANAGER (clawt_task_manager_get_type())

G_DECLARE_FINAL_TYPE(ClawtTaskManager, clawt_task_manager,
                     CLAWT, TASK_MANAGER, GObject)

/**
 * clawt_task_manager_new:
 *
 * Live tasks are served from memory.  Give the manager a store
 * (clawt_task_manager_set_store()) and every change is also written
 * there, the live set is restored from it on start, and older tasks are
 * read back from it on demand; without one, a restart forgets them.
 *
 * Returns: (transfer full): a new #ClawtTaskManager
 */
ClawtTaskManager *clawt_task_manager_new(void);

/**
 * clawt_task_manager_create:
 * @self: a #ClawtTaskManager
 * @origin_agent: who delegated it
 * @assignee: (nullable): who is to do it, or %NULL for the backlog
 * @prompt: what to do
 * @parent_id: (nullable): the task this one was spawned from
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a task, inheriting depth from its parent.  With a %NULL
 * @assignee the task goes on the operator's backlog as
 * %CLAWT_TASK_UNASSIGNED until clawt_task_manager_assign() gives it one.
 *
 * Returns: (transfer none) (nullable): the task, or %NULL if it would be
 *   too deep
 */
ClawtTask *clawt_task_manager_create(ClawtTaskManager  *self,
                                     const gchar       *origin_agent,
                                     const gchar       *assignee,
                                     const gchar       *prompt,
                                     const gchar       *parent_id,
                                     GError           **error);

/**
 * clawt_task_manager_get:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 *
 * The live set first, then any task read back from the store earlier,
 * then the store itself: a task that finished long ago is still
 * answerable (its result, its reason), without reappearing in a live
 * listing.  A borrowed pointer stays valid until the next
 * clawt_task_manager_forget_finished().
 *
 * Returns: (transfer none) (nullable): the task, or %NULL
 */
ClawtTask *clawt_task_manager_get(ClawtTaskManager *self,
                                  const gchar      *task_id);

/**
 * clawt_task_manager_list:
 * @self: a #ClawtTaskManager
 * @assignee: (nullable): only this agent's tasks, or %NULL for all
 * @include_finished: whether to include tasks that have ended
 *
 * Returns: (transfer container) (element-type ClawtTask): matching tasks,
 *   newest first
 */
GPtrArray *clawt_task_manager_list(ClawtTaskManager *self,
                                   const gchar      *assignee,
                                   gboolean          include_finished);

/**
 * clawt_task_manager_list_involving:
 * @self: a #ClawtTaskManager
 * @agent_id: the agent whose tasks are wanted
 * @include_finished: whether to include tasks that have ended
 *
 * Every task @agent_id delegated **or** was assigned, newest first.
 *
 * Both roles, because "my tasks" means both to the agent asking and the
 * two are answered from the same place.  clawt_task_manager_list() takes
 * an assignee alone, which is the operator's question -- an agent that
 * handed work out is not the assignee of any of it, so filtering that
 * way returns nothing to the caller who most needs an answer.
 *
 * The caller tells the two apart by comparing the origin itself; that is
 * presentation rather than filtering.
 *
 * Returns: (transfer container) (element-type ClawtTask): matching tasks
 */
GPtrArray *clawt_task_manager_list_involving(ClawtTaskManager *self,
                                             const gchar      *agent_id,
                                             gboolean          include_finished);

/**
 * clawt_task_manager_list_descendants:
 * @self: a #ClawtTaskManager
 * @agent_id: the agent whose fan-out is wanted
 * @include_finished: whether to include tasks that have ended
 *
 * Every task somewhere below a task @agent_id delegated, newest first.
 *
 * Deliberately excludes what @agent_id is itself a party to --
 * clawt_task_manager_list_involving() answers that, and a task returned
 * by both would be drawn twice under headings that mean different
 * things.  Together they are the whole tree an agent can account for.
 *
 * Without this a delegator could see one level and no further: a chief
 * that gave a lead a job could not list what the lead gave anybody, so a
 * fan-out was unobservable from the one place watching it.
 *
 * Returns: (transfer container) (element-type ClawtTask): matching tasks
 */
GPtrArray *clawt_task_manager_list_descendants(ClawtTaskManager *self,
                                               const gchar      *agent_id,
                                               gboolean          include_finished);

/**
 * clawt_task_manager_count_unfinished_children:
 * @self: a #ClawtTaskManager
 * @task_id: (nullable): a task id
 *
 * How many tasks @task_id handed on have not ended.
 *
 * Direct children only, which is enough by induction: a task cannot end
 * by inference while a child of its own is running, so an unfinished
 * grandchild keeps its parent unfinished and this sees the parent.
 *
 * Returns: the number of unfinished children, or 0 for an unknown task
 */
guint clawt_task_manager_count_unfinished_children(ClawtTaskManager *self,
                                                   const gchar      *task_id);

/**
 * clawt_task_manager_note_progress:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @progress: (nullable): what the assignee wants the delegator to know
 *
 * Records progress without ending the task, and holds off the
 * turn-end inference for one turn.
 *
 * Also moves a pending task to running, because a progress note is proof
 * somebody picked it up.  clawtilla_delegate does not mark a task
 * running -- only the operator and routine paths do -- so an
 * agent-delegated task otherwise reads `pending` for its whole life, and
 * a delegator taking that at face value delegates it a second time.
 *
 * Returns: %TRUE if the task existed and had not ended
 */
gboolean clawt_task_manager_note_progress(ClawtTaskManager *self,
                                          const gchar      *task_id,
                                          const gchar      *progress);

/**
 * clawt_task_manager_complete_on_turn_end:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @who: the agent whose turn ended
 * @result: what that turn ended with
 * @held_reason: (out) (optional) (transfer full): why it was not
 *   completed, when it was not
 *
 * Completes a task because its assignee's turn ended -- unless something
 * says the work is still going.  An AI CLI cannot end a turn without
 * writing something, so the last thing the assignee wrote is the only
 * evidence there is; right for work-answer-done, and wrong for the
 * assignee that hands part of the job on and reports once at the end --
 * such a turn closed the task under itself, the delegator stopped
 * polling, and the real answer arrived against a task nothing was
 * waiting on.
 *
 * Only the assignee's turn is evidence at all, which is why the caller
 * says whose it was.  Everyone in a task's thread ends turns there --
 * the delegator's, started by a progress note, ends with an
 * acknowledgement -- and completing on any of them recorded "Thanks,
 * carry on" as the result of running work.  Anyone else's turn end
 * leaves the task as it was, with @held_reason naming whose could end
 * it -- and it is checked before the hold below, which is consumed by
 * being checked: another agent's turn must not spend it.
 *
 * Two things then veto it for the assignee itself, here rather than at
 * the call site so there is one answer: it said so through
 * clawtilla_task_progress, and the task has children of its own still
 * running.  Only the second records @result as the task's progress note
 * -- a task held open by its own fan-out has no deliberate note, while
 * the first already carries one chosen for the purpose, and overwriting
 * that with whatever an AI CLI wrote to end its turn is a strictly
 * worse answer.  The child veto could not fire at all until
 * clawtilla_delegate began recording a parent.
 *
 * Returns: %TRUE if the task was completed
 */
gboolean clawt_task_manager_complete_on_turn_end(ClawtTaskManager  *self,
                                                 const gchar       *task_id,
                                                 const gchar       *who,
                                                 const gchar       *result,
                                                 gchar            **held_reason);

/**
 * clawt_task_manager_note_continuation:
 * @self: a #ClawtTaskManager
 * @task_id: the task the turn was on
 * @who: whose turn it was
 * @known: whether the turn was observed at all (a streamed step or a
 *   clawtilla call) -- %FALSE is neither a strike nor a reset
 * @work_calls: successful tool calls the turn made, not counting
 *   clawtilla_task_progress
 * @error: (nullable): the text of the turn's last failure
 *
 * The progress guard, for a turn that ended with its task still open:
 * a *continuation*.  Two in a row with no successful tool call, or
 * three in a row ending in the same error, end the task as stalled with
 * a reason that says which -- the settle notice then tells the
 * delegator, as for any other stall.
 *
 * Only the assignee's own turns count.  A turn that waits on the task's
 * own children is not a continuation and clears the record, and so
 * does the task ending.  This is the cross-turn half of what
 * #ClawtRepeatWatch does inside one turn; neither sees the other's
 * case.
 *
 * Returns: %TRUE when the task was stalled by this call
 */
gboolean clawt_task_manager_note_continuation(ClawtTaskManager *self,
                                              const gchar      *task_id,
                                              const gchar      *who,
                                              gboolean          known,
                                              guint             work_calls,
                                              const gchar      *error);

/**
 * clawt_task_manager_complete:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @result: what it produced
 *
 * Returns: %TRUE if the task existed and was running
 */
gboolean clawt_task_manager_complete(ClawtTaskManager *self,
                                     const gchar      *task_id,
                                     const gchar      *result);

/**
 * clawt_task_manager_fail:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @reason: what went wrong
 *
 * Returns: %TRUE if the task existed and had not already ended
 */
/**
 * clawt_task_manager_retry:
 * @self: a #ClawtTaskManager
 * @task_id: the task
 * @who: who is retrying it: an agent id, or "user"
 * @error: (out) (optional): why it was refused
 *
 * Puts a failed or stalled task back to `pending`, for its assignee to
 * be handed the work again.  The one way a finished task becomes
 * unfinished, and deliberately narrow: a completed task would be redone,
 * a cancelled one was stopped on purpose, and a pending or running one
 * has nothing to retry.  Emits ::task-changed.
 *
 * Returns: %TRUE if the task was reopened
 */
gboolean clawt_task_manager_retry(ClawtTaskManager  *self,
                                  const gchar       *task_id,
                                  const gchar       *who,
                                  GError           **error);

/**
 * clawt_task_manager_stall:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @reason: which limit ended it, in words somebody can act on
 *
 * Ends a task that a loop limit stopped. Distinct from
 * clawt_task_manager_fail() on purpose: stalled work can be picked up
 * and finished, failed work has to be diagnosed first, and a reader who
 * cannot tell them apart goes looking for the wrong thing.
 *
 * Returns: %TRUE if the task existed and had not already ended
 */
gboolean clawt_task_manager_stall(ClawtTaskManager *self,
                                  const gchar      *task_id,
                                  const gchar      *reason);

gboolean clawt_task_manager_fail(ClawtTaskManager *self,
                                 const gchar      *task_id,
                                 const gchar      *reason);

/**
 * clawt_task_manager_cancel:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @reason: (nullable): why
 * @who: (nullable): who is cancelling -- an agent id, or "user"
 *
 * Cancels a task and everything it spawned.
 *
 * Cancelling only the parent would leave its children running and reporting
 * into a task nobody is waiting for -- which is exactly the runaway
 * cancellation is meant to stop.  @who is recorded on every task in the
 * cascade, so the settle notice can tell a delegator being informed
 * from a delegator being told what it just did itself.
 *
 * Returns: how many tasks were cancelled
 */
guint clawt_task_manager_cancel(ClawtTaskManager *self,
                                const gchar      *task_id,
                                const gchar      *reason,
                                const gchar      *who);

/**
 * clawt_task_manager_start:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 *
 * Marks a task as being worked on.
 *
 * Returns: %TRUE if it was pending
 */
gboolean clawt_task_manager_start(ClawtTaskManager *self,
                                  const gchar      *task_id);

/**
 * clawt_task_manager_orphan_agent_tasks:
 * @self: a #ClawtTaskManager
 * @agent_id: an agent that has gone away
 *
 * Fails the running tasks of an agent that has stopped.
 *
 * Without this a delegator waits for a result that can never arrive,
 * because the agent that owed it no longer exists.
 *
 * Returns: how many tasks were failed
 */
guint clawt_task_manager_orphan_agent_tasks(ClawtTaskManager *self,
                                            const gchar      *agent_id);

/**
 * clawt_task_manager_set_max_depth:
 * @self: a #ClawtTaskManager
 * @max_depth: how deep delegation may nest
 */
void clawt_task_manager_set_max_depth(ClawtTaskManager *self,
                                      guint             max_depth);

/**
 * clawt_task_manager_set_store:
 * @self: a #ClawtTaskManager
 * @store: (nullable): where to write every change, or %NULL for memory
 *   only
 *
 * From here on every transition is written to @store before
 * ::task-changed is emitted, with a journal row naming what happened.
 * A save that fails is logged and survived: the transition stands.
 */
void clawt_task_manager_set_store(ClawtTaskManager *self,
                                  ClawtTaskStore   *store);

/**
 * clawt_task_manager_get_store:
 * @self: a #ClawtTaskManager
 *
 * Returns: (transfer none) (nullable): the store, or %NULL
 */
ClawtTaskStore *clawt_task_manager_get_store(ClawtTaskManager *self);

/**
 * clawt_task_manager_load:
 * @self: a #ClawtTaskManager
 * @finished_since: also load tasks that finished at or after this Unix
 *   time
 * @error: (out) (optional): return location for a #GError
 *
 * Restores the live set from the store: every task that has not finished
 * and the ones that finished since @finished_since, in creation order.
 *
 * Silent: nothing is emitted, because nothing changed.  Called by a
 * starting daemon before anything listens, so a restored task does not
 * send its settle notice, raise its incident or wake its delegator a
 * second time.  Tasks already in memory are left as they are.
 *
 * Returns: how many tasks were restored
 */
guint clawt_task_manager_load(ClawtTaskManager  *self,
                              gint64             finished_since,
                              GError           **error);

/**
 * clawt_task_manager_save:
 * @self: a #ClawtTaskManager
 * @task: a task this manager holds
 * @what: (nullable): what happened, for the task's journal
 *
 * Writes @task to the store after a change made outside the manager --
 * a field set with clawt_task_set_*() straight after creation, or an
 * owner change from a handoff.  Every transition the manager makes
 * itself is already saved; this is for the rest, which would otherwise
 * be lost to a restart.  A parent set after creation is also refiled in
 * the children index here, which the cancel cascade and
 * clawt_task_manager_count_unfinished_children() read.
 */
void clawt_task_manager_save(ClawtTaskManager *self,
                             ClawtTask        *task,
                             const gchar      *what);

/**
 * clawt_task_manager_forget_finished:
 * @self: a #ClawtTaskManager
 * @finished_before: drop tasks that finished before this Unix time
 *
 * Takes finished tasks out of memory.  They stay in the store, so
 * clawt_task_manager_get() still answers for them and
 * clawt_task_manager_list_history() still lists them; they only leave
 * the live listings.  A finished task with a child still running stays.
 * Does nothing without a store, where forgetting would lose the task.
 *
 * Returns: how many were dropped from memory
 */
guint clawt_task_manager_forget_finished(ClawtTaskManager *self,
                                         gint64            finished_before);

/**
 * clawt_task_manager_list_history:
 * @self: a #ClawtTaskManager
 * @query: (nullable): what to narrow to
 * @error: (out) (optional): return location for a #GError
 *
 * Every task, finished or not, newest first and one page at a time
 * (clawt_task_store_list()).  From memory when there is no store.
 *
 * Returns: (transfer full) (element-type ClawtTask) (nullable): copies of
 *   the tasks, or %NULL on error
 */
GPtrArray *clawt_task_manager_list_history(ClawtTaskManager     *self,
                                           const ClawtTaskQuery *query,
                                           GError              **error);

/**
 * clawt_task_manager_record:
 * @self: a #ClawtTaskManager
 * @task_id: which task
 * @what: what happened, for its history
 *
 * Saves the task and adds @what to its journal, without a state change
 * and so without `task-changed` -- for something done to a task's
 * surroundings, such as its checkout being removed (since 0.39.0).
 *
 * Returns: %TRUE if there is such a task
 */
gboolean clawt_task_manager_record(ClawtTaskManager *self,
                                   const gchar      *task_id,
                                   const gchar      *what);

/**
 * clawt_task_manager_journal:
 * @self: a #ClawtTaskManager
 * @task_id: which task
 * @error: (out) (optional): return location for a #GError
 *
 * The task's history from the store (clawt_task_store_journal()).
 *
 * Returns: (transfer full) (nullable): an array node, or %NULL on error or
 *   without a store
 */
JsonNode *clawt_task_manager_journal(ClawtTaskManager  *self,
                                     const gchar       *task_id,
                                     GError           **error);

/**
 * clawt_task_manager_assign:
 * @self: a #ClawtTaskManager
 * @task_id: an unassigned task
 * @assignee: who gets it
 * @who: (nullable): who assigned it, for the journal
 * @error: (out) (optional): return location for a #GError
 *
 * Takes a task off the backlog: gives it @assignee and makes it
 * `pending`.  Delivering the work is the caller's -- the daemon sends it
 * as the operator's.  Only an `unassigned` task is assigned this way; one
 * somebody already has moves with a handoff.
 *
 * Returns: %TRUE when assigned
 */
gboolean clawt_task_manager_assign(ClawtTaskManager  *self,
                                   const gchar       *task_id,
                                   const gchar       *assignee,
                                   const gchar       *who,
                                   GError           **error);

/**
 * clawt_task_manager_add_note:
 * @self: a #ClawtTaskManager
 * @task_id: which task
 * @author: (nullable): who wrote it, `user` for the operator
 * @kind: a CLAWT_TASK_NOTE_* spelling
 * @body: the text, not empty
 * @error: (out) (optional): return location for a #GError
 *
 * Adds a note to the task's thread (clawt_task_store_add_note()).  The
 * manager adds the progress and result notes itself as they happen;
 * this is for everything else, a comment above all.  Emits nothing: a
 * note changes no state, and ::task-changed is what settle notices and
 * incidents hang off.
 *
 * Returns: the note's id, or 0 on error (no such task, empty text, or no
 *   store)
 */
gint64 clawt_task_manager_add_note(ClawtTaskManager  *self,
                                   const gchar       *task_id,
                                   const gchar       *author,
                                   const gchar       *kind,
                                   const gchar       *body,
                                   GError           **error);

/**
 * clawt_task_manager_notes:
 * @self: a #ClawtTaskManager
 * @task_id: which task
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the notes, as
 *   clawt_task_store_notes() gives them, or %NULL without a store
 */
JsonNode *clawt_task_manager_notes(ClawtTaskManager  *self,
                                   const gchar       *task_id,
                                   GError           **error);

/**
 * ClawtTaskReviewOutcome:
 * @CLAWT_TASK_REVIEW_REFUSED: no verdict was recorded; see the error
 * @CLAWT_TASK_REVIEW_ACCEPTED: the task is completed
 * @CLAWT_TASK_REVIEW_CHANGES: the task is back with its assignee
 * @CLAWT_TASK_REVIEW_ESCALATED: changes were asked for once too often;
 *   the task stays in review, now with the operator
 *
 * What a review verdict did (since 0.38.0).
 */
typedef enum {
    CLAWT_TASK_REVIEW_REFUSED = 0,
    CLAWT_TASK_REVIEW_ACCEPTED,
    CLAWT_TASK_REVIEW_CHANGES,
    CLAWT_TASK_REVIEW_ESCALATED
} ClawtTaskReviewOutcome;

/**
 * clawt_task_manager_review:
 * @self: a #ClawtTaskManager
 * @task_id: a task in review
 * @who: the reviewer, or `user` -- the operator may always decide
 * @accept: %TRUE to accept the result, %FALSE to ask for changes
 * @notes: (nullable): what to change (required when asking for changes)
 * @max_rounds: how many times changes may be asked for before the
 *   operator decides; 0 for no limit
 * @error: (out) (optional): return location for a #GError
 *
 * Records a verdict on a result in review.  Accepting completes the task.
 * Asking for changes sends it back to its assignee as `pending` with the
 * notes in its thread; the @max_rounds'th time hands the decision to the
 * operator instead.  Handing the notes to the assignee is the caller's.
 *
 * Returns: what happened
 */
ClawtTaskReviewOutcome clawt_task_manager_review(ClawtTaskManager  *self,
                                                 const gchar       *task_id,
                                                 const gchar       *who,
                                                 gboolean           accept,
                                                 const gchar       *notes,
                                                 guint              max_rounds,
                                                 GError           **error);

/**
 * clawt_task_manager_link:
 * @self: a #ClawtTaskManager
 * @task_id: the task that is to wait
 * @blocker_id: the task it waits for
 * @error: (out) (optional): return location for a #GError
 *
 * Makes @task_id wait for @blocker_id to complete.  Refused for a task
 * that has ended, for a task waiting for itself, and for a link that
 * would close a cycle -- each with the reason.  Linking does not change
 * the task's state: the caller decides whether it is held now
 * (clawt_task_manager_block()).  Emits ::blockers-changed.  Needs a
 * store.
 *
 * Returns: %TRUE when linked
 */
gboolean clawt_task_manager_link(ClawtTaskManager  *self,
                                 const gchar       *task_id,
                                 const gchar       *blocker_id,
                                 GError           **error);

/**
 * clawt_task_manager_unlink:
 * @self: a #ClawtTaskManager
 * @task_id: the task that waited
 * @blocker_id: the task it no longer waits for
 * @error: (out) (optional): return location for a #GError
 *
 * Emits ::blockers-changed, which is what lets a held task go once the
 * last thing it waited for is taken away.  Needs a store.
 *
 * Returns: %TRUE when there is no longer such a link
 */
gboolean clawt_task_manager_unlink(ClawtTaskManager  *self,
                                   const gchar       *task_id,
                                   const gchar       *blocker_id,
                                   GError           **error);

/**
 * clawt_task_manager_blockers:
 * @self: a #ClawtTaskManager
 * @task_id: a task
 *
 * Returns: (transfer full) (element-type utf8): every task @task_id
 *   waits for, finished or not; empty without a store
 */
GPtrArray *clawt_task_manager_blockers(ClawtTaskManager *self,
                                       const gchar      *task_id);

/**
 * clawt_task_manager_dependents:
 * @self: a #ClawtTaskManager
 * @task_id: a task
 *
 * Returns: (transfer full) (element-type utf8): every task waiting for
 *   @task_id; empty without a store
 */
GPtrArray *clawt_task_manager_dependents(ClawtTaskManager *self,
                                         const gchar      *task_id);

/**
 * clawt_task_manager_waiting_on:
 * @self: a #ClawtTaskManager
 * @task_id: a task
 *
 * The blockers that still hold @task_id: every one that has not
 * completed.  A blocker that failed or was cancelled still holds it --
 * its work was meant to come first -- and one pruned long ago holds
 * nothing.
 *
 * Returns: (transfer full) (element-type utf8): their ids, empty when
 *   the task may go
 */
GPtrArray *clawt_task_manager_waiting_on(ClawtTaskManager *self,
                                         const gchar      *task_id);

/**
 * clawt_task_manager_block:
 * @self: a #ClawtTaskManager
 * @task_id: a pending or unassigned task
 *
 * Holds the task as %CLAWT_TASK_BLOCKED until its blockers complete.
 *
 * Returns: %TRUE when it is now blocked
 */
gboolean clawt_task_manager_block(ClawtTaskManager *self,
                                  const gchar      *task_id);

/**
 * clawt_task_manager_unblock:
 * @self: a #ClawtTaskManager
 * @task_id: a blocked task
 *
 * Lets a blocked task go: to `pending` when it has an assignee, back to
 * the backlog when it does not.  Handing the work over is the caller's
 * (the held delivery, clawt_task_manager_take_delivery()).
 *
 * Returns: %TRUE when it was blocked and is not now
 */
gboolean clawt_task_manager_unblock(ClawtTaskManager *self,
                                    const gchar      *task_id);

/**
 * clawt_task_manager_hold_delivery:
 * @self: a #ClawtTaskManager
 * @task_id: a blocked task
 * @delivery: what the daemon needs to hand it over later
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when kept (clawt_task_store_hold_delivery())
 */
gboolean clawt_task_manager_hold_delivery(ClawtTaskManager  *self,
                                          const gchar       *task_id,
                                          JsonNode          *delivery,
                                          GError           **error);

/**
 * clawt_task_manager_take_delivery:
 * @self: a #ClawtTaskManager
 * @task_id: a blocked task
 *
 * Returns: (transfer full) (nullable): the held delivery, removed as it
 *   is taken (clawt_task_store_take_delivery())
 */
JsonNode *clawt_task_manager_take_delivery(ClawtTaskManager *self,
                                           const gchar      *task_id);

G_END_DECLS
