/*
 * clawt-task.h - A unit of delegated work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What makes chief-of-staff orchestration observable rather than an
 * unbounded chat cascade: delegation creates a task, which can be listed,
 * followed and cancelled.
 *
 * A task does *not* get a libreclaw session of its own, though this said
 * so for a long time and clawt_task_new() still builds a session key
 * nothing reads.  lc_router_resolve_session_key() keys on channel, room
 * and sender and excludes the thread on purpose -- there a thread anchors
 * a reply rather than dividing a conversation -- so a task lands in the
 * sender's session and inherits whatever context is already in it.
 *
 * Isolating a job therefore means routing it into a room of its own,
 * which is what `routines.isolate` does.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TASK (clawt_task_get_type())

GType clawt_task_get_type(void) G_GNUC_CONST;

/**
 * clawt_task_new:
 * @origin_agent: who delegated it
 * @assignee: who is to do it
 * @prompt: what they are to do
 *
 * Creates a task in the ~pending~ state.  Ordinary code goes through
 * clawt_task_manager_create(), which also enforces the depth limit and
 * records the parent.
 *
 * Returns: (transfer full): a new #ClawtTask
 */
ClawtTask *clawt_task_new(const gchar *origin_agent,
                          const gchar *assignee,
                          const gchar *prompt);

/**
 * clawt_task_copy:
 * @self: a #ClawtTask
 *
 * Returns: (transfer full): a copy
 */
ClawtTask *clawt_task_copy(ClawtTask *self);
void       clawt_task_free(ClawtTask *self);

/**
 * clawt_task_get_id:
 * @self: a #ClawtTask
 *
 * The accessors below are plain reads of what clawt_task_new() and the
 * setters put there.  Documented as a group because a line each saying
 * "returns the assignee" would be noise; the fields themselves are
 * described on #ClawtTask.
 *
 * Every string getter is (transfer none) and may be %NULL for the
 * optional fields -- room, parent, reason, result and session key are
 * all absent until something sets them.
 *
 * Returns: (transfer none): the task's identifier
 */
const gchar   *clawt_task_get_id(ClawtTask *self);
const gchar   *clawt_task_get_origin(ClawtTask *self);
const gchar   *clawt_task_get_assignee(ClawtTask *self);
const gchar   *clawt_task_get_prompt(ClawtTask *self);
const gchar   *clawt_task_get_result(ClawtTask *self);
const gchar   *clawt_task_get_room(ClawtTask *self);
const gchar   *clawt_task_get_parent_id(ClawtTask *self);
const gchar   *clawt_task_get_reason(ClawtTask *self);
const gchar   *clawt_task_get_session_key(ClawtTask *self);
const gchar   *clawt_task_get_progress_note(ClawtTask *self);

/**
 * clawt_task_get_cancelled_by:
 * @self: a #ClawtTask
 *
 * Who cancelled it, or %NULL for a task that ended some other way.
 * Recorded so the settle notice knows when to stay quiet: telling a
 * delegator that the task it just cancelled is cancelled costs a model
 * turn to read a fact it already knows, while a cancellation by the
 * operator or a cascade from above is exactly what it is waiting to
 * hear.
 *
 * Returns: (transfer none) (nullable): the canceller
 */
const gchar   *clawt_task_get_cancelled_by(ClawtTask *self);

/**
 * clawt_task_set_cancelled_by:
 * @self: a #ClawtTask
 * @value: (nullable): who cancelled it
 *
 * Set by clawt_task_manager_cancel() before the state changes, so a
 * ::task-changed handler reading the task sees it on the one emission
 * that says the task ended.
 */
void           clawt_task_set_cancelled_by(ClawtTask *self,
                                           const gchar *value);
ClawtTaskState clawt_task_get_state(ClawtTask *self);
gint           clawt_task_get_depth(ClawtTask *self);
gint64         clawt_task_get_created_at(ClawtTask *self);
gint64         clawt_task_get_finished_at(ClawtTask *self);

/**
 * clawt_task_assignment_guidance:
 * @task_id: the task being delivered
 *
 * The paragraph appended to a delivered assignment -- delegation and
 * handoff alike -- telling the assignee how this ends: finishing the
 * turn is finishing the work, the delegator is notified by clawtilla
 * itself, progress goes through clawtilla_task_progress, and status
 * messages are not to be sent.  One spelling, because the contract in
 * two texts drifts into two contracts; and it names the tools, because
 * "report when done" has already been satisfied by an assignee ending
 * its turn into a void.
 *
 * Returns: (transfer full): the guidance, starting with a blank line
 */
gchar *clawt_task_assignment_guidance(const gchar *task_id);

/**
 * clawt_task_get_owner_history:
 * @self: a #ClawtTask
 *
 * Everyone who has owned this task, oldest first.
 *
 * The first entry is whoever it was created for and the last is
 * clawt_task_get_assignee(), so the two can never disagree -- there is
 * one place an owner changes and it appends here as it goes.
 *
 * A task nobody has handed on has one entry rather than none, because
 * "never moved" and "no history recorded" are different facts and a
 * reader who cannot tell them apart goes looking for a lost record.
 *
 * Returns: (transfer none) (element-type utf8): the owners, in order
 */
GPtrArray *clawt_task_get_owner_history(ClawtTask *self);

/**
 * clawt_task_transfer_owner:
 * @self: a #ClawtTask
 * @new_owner: who owns it now
 *
 * Moves ownership and records the move.
 *
 * There is deliberately no plain set_assignee(): an assignee changed
 * without an entry in the history is a task whose past cannot be
 * reconstructed, and the one caller that forgot would be the one whose
 * work went missing.  Handing it the current owner is a no-op rather
 * than a repeated entry.
 *
 * Returns: %TRUE if the owner changed
 */
gboolean clawt_task_transfer_owner(ClawtTask *self, const gchar *new_owner);

/**
 * clawt_task_set_room:
 * @self: a #ClawtTask
 * @room: (nullable): the room this task belongs to
 *
 * The setters below all take %NULL to clear the field.  They exist for
 * the manager and for reading a task back off the wire; ordinary code
 * goes through #ClawtTaskManager, which enforces the state machine.
 */
void clawt_task_set_room(ClawtTask *self, const gchar *room);
void clawt_task_set_parent_id(ClawtTask *self, const gchar *parent_id);
void clawt_task_set_reason(ClawtTask *self, const gchar *reason);
void clawt_task_set_result(ClawtTask *self, const gchar *result);
void clawt_task_set_progress_note(ClawtTask *self, const gchar *note);
void clawt_task_set_state(ClawtTask *self, ClawtTaskState state);
void clawt_task_set_depth(ClawtTask *self, gint depth);

/**
 * clawt_task_hold_completion:
 * @self: a #ClawtTask
 *
 * Records that the assignee ended a turn with the work unfinished.
 *
 * The daemon completes a task from the message that ends its assignee's
 * turn, because an AI CLI has no other way of saying "done".  That is an
 * inference, and it is wrong for the assignee that does the sensible
 * thing: finish part of the job, hand the rest on, and report once at
 * the end rather than narrating.  Such a turn ends with a status note
 * and the task closed under it -- so the delegator stopped polling and
 * the real answer arrived against a task nothing was waiting on.  The
 * lifecycle rewarded chatter, which is the opposite of what the rest of
 * the guidance asks for.
 *
 * This is how an assignee declines that inference for one turn.
 * Ordinary code goes through clawt_task_manager_note_progress().
 */
void clawt_task_hold_completion(ClawtTask *self);

/**
 * clawt_task_take_completion_hold:
 * @self: a #ClawtTask
 *
 * Reads the hold and clears it, in one step.
 *
 * One-shot on purpose: a hold that outlived the turn it was set in would
 * mean the task could never finish by inference again, and the assignee
 * is the one that knows -- so it says so on each turn the work is still
 * running.  Taking and clearing together is what stops two readers from
 * both deciding they are the one that consumed it.
 *
 * Returns: %TRUE if a hold was set, in which case it is now cleared
 */
gboolean clawt_task_take_completion_hold(ClawtTask *self);

/**
 * clawt_task_get_result_inferred:
 * @self: a #ClawtTask
 *
 * Whether the result was taken from the end of a turn rather than
 * reported through clawtilla_task_complete.
 *
 * "They said it was done" and "they stopped talking" are different
 * facts, and a delegator that cannot tell them apart either re-delegates
 * work that is finished or waits on work that is not.
 *
 * Returns: %TRUE if nothing reported this task complete
 */
gboolean clawt_task_get_result_inferred(ClawtTask *self);

/**
 * clawt_task_set_result_inferred:
 * @self: a #ClawtTask
 * @inferred: %TRUE if the result came from the end of a turn
 */
void clawt_task_set_result_inferred(ClawtTask *self, gboolean inferred);

/**
 * clawt_task_is_finished:
 * @self: a #ClawtTask
 *
 * Returns: %TRUE if the task will not change state again
 */
gboolean clawt_task_is_finished(ClawtTask *self);

/**
 * clawt_task_state_tone:
 * @state: a #ClawtTaskState
 *
 * How a badge for @state should read: one of "neutral", "good", "warn",
 * "bad" or "info".
 *
 * Here rather than in a client because both of them draw this badge, and
 * a rule two clients apply separately is a rule they will eventually
 * disagree about. They already did, in the worst way available: the web
 * client compared the state against "done" and "complete", neither of
 * which is a #ClawtTaskState nickname, so a finished task fell through to
 * "neutral" and had never once been drawn green -- and a colour that is
 * merely wrong looks like somebody's design choice, so nothing reported
 * it. The GTK client did not colour the badge at all.
 *
 * The switch names every value and has no `default:`, so a state added to
 * the enum draws a -Wswitch warning here rather than being one more thing
 * quietly drawn grey -- and tests/test-task.c walks the enum, so it fails
 * outright. `stalled` was added under the old code and got neither, which
 * is why a stalled task looked exactly like a pending one.
 *
 * "bad" covers cancelled as well as failed. A cancellation is deliberate
 * and not an error, which is an argument for warn -- but the reader of a
 * task list is asking which tasks produced their result, and neither of
 * these did. Kept together for that reason rather than by accident.
 *
 * Returns: (transfer none): the tone
 */
const gchar *clawt_task_state_tone(ClawtTaskState state);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTask, clawt_task_free)

G_END_DECLS
