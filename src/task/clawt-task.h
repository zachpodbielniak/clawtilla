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
#include <json-glib/json-glib.h>

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

/**
 * clawt_task_get_model:
 * @self: a #ClawtTask
 *
 * The model the delegator asked the work to run on, for the assignee's
 * turn that receives it (`clawtilla_delegate`'s `model`, a routine's
 * `model`).  That turn only: the assignee's later turns -- its follow-ups
 * on the same task included -- are on its own model.
 *
 * Returns: (transfer none) (nullable): the model, or %NULL for the
 *   assignee's own
 */
const gchar   *clawt_task_get_model(ClawtTask *self);
const gchar   *clawt_task_get_session_key(ClawtTask *self);

/**
 * clawt_task_get_thread_room:
 * @self: a #ClawtTask
 *
 * The room the work runs in when it was delegated with `thread: new`:
 * `task:<id>`, a room and so a libreclaw session of its own, with the
 * delegator and the assignee as its members.  The delegator and every
 * client follow the work there.  Not where the settle notice goes --
 * that is clawt_task_get_room(), the conversation that delegated.
 *
 * Returns: (transfer none) (nullable): the room, or %NULL for work
 *   delivered into the room the two already share
 */
const gchar   *clawt_task_get_thread_room(ClawtTask *self);

/**
 * clawt_task_set_thread_room:
 * @self: a #ClawtTask
 * @room: (nullable): the task's own room, or %NULL
 *
 * Recorded by clawtilla_delegate when it made the room.
 */
void           clawt_task_set_thread_room(ClawtTask *self, const gchar *room);

/**
 * clawt_task_get_consult:
 * @self: a #ClawtTask
 *
 * Whether the task was delegated with `mode: consult`: the assignee's
 * turn on it is offered no tool that acts, and its answer is the
 * result.
 *
 * Returns: %TRUE for a consultation
 */
gboolean       clawt_task_get_consult(ClawtTask *self);

/**
 * clawt_task_set_consult:
 * @self: a #ClawtTask
 * @consult: %TRUE for a consultation
 *
 * Recorded by clawtilla_delegate before the work is delivered, which
 * is when the daemon reads it to mark the assignment.
 */
void           clawt_task_set_consult(ClawtTask *self, gboolean consult);
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
/**
 * clawt_task_get_retried_by:
 * @self: a #ClawtTask
 *
 * Who last sent it round again with clawtilla_task_retry or a client's
 * Retry -- an agent id, or "user" for the operator.  Kept on the task as
 * well as on the incident, because a listing of tasks is where somebody
 * asks why one is running again.
 *
 * Returns: (transfer none) (nullable): the retrier
 */
const gchar   *clawt_task_get_retried_by(ClawtTask *self);

/**
 * clawt_task_get_retries:
 * @self: a #ClawtTask
 *
 * Returns: how many times it has been retried
 */
guint          clawt_task_get_retries(ClawtTask *self);

/**
 * clawt_task_reopen:
 * @self: a #ClawtTask
 * @who: who is retrying it
 *
 * Puts a failed or stalled task back to `pending` for another attempt:
 * clears the result, the reason, the finish time and both completion
 * flags, and records @who.  The state check is
 * clawt_task_manager_retry()'s, which is what callers use.
 */
void           clawt_task_reopen(ClawtTask *self, const gchar *who);
ClawtTaskState clawt_task_get_state(ClawtTask *self);
gint           clawt_task_get_depth(ClawtTask *self);
gint64         clawt_task_get_created_at(ClawtTask *self);
gint64         clawt_task_get_finished_at(ClawtTask *self);

/**
 * clawt_task_get_started_at:
 * @self: a #ClawtTask
 *
 * When the task first went to `running`, as a Unix time.  Stamped once:
 * a retry is a second attempt at the same task, not a new start.
 *
 * Returns: the time, or 0 when it has never run
 */
gint64         clawt_task_get_started_at(ClawtTask *self);

/**
 * clawt_task_get_title:
 * @self: a #ClawtTask
 *
 * What the task is called in a list: the title it was given, or the
 * first line of its prompt that says anything, cut on a character
 * boundary (clawt_task_title_from_prompt()).
 *
 * Returns: (transfer none): the title, never %NULL
 */
const gchar   *clawt_task_get_title(ClawtTask *self);

/**
 * clawt_task_set_title:
 * @self: a #ClawtTask
 * @title: (nullable): the title, or %NULL / empty to derive one again
 *
 * Gives the task a title of its own.  Cut to the same length as a
 * derived one.
 */
void           clawt_task_set_title(ClawtTask *self, const gchar *title);

/**
 * clawt_task_title_from_prompt:
 * @prompt: (nullable): what the task asks for
 *
 * The title a task without one is shown with: the first line of @prompt
 * that is not blank, with leading markdown markers (`#`, `*`, `-`, `>`)
 * and surrounding space removed, cut to 120 bytes on a character
 * boundary.
 *
 * Returns: (transfer full): the title, empty when @prompt says nothing
 */
gchar         *clawt_task_title_from_prompt(const gchar *prompt);

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
 * @room: (nullable): the room the delegating turn was running in
 *
 * Which conversation this task came out of.  clawtilla_delegate sets
 * it from the calling turn's room, and the settle notice returns
 * there: "You will be notified here" is a promise about a
 * conversation, and for an agent whose sessions are partitioned per
 * room, delivering the notice anywhere else wakes a context that
 * never heard the delegation.  The field sat here unset and unread
 * for its whole life while the notice went to the room the *assignee*
 * shares -- a decision whose recorded reason ("so the delegator's
 * session has the conversation behind it") described room-keyed
 * sessions the router does not run.
 *
 * %NULL when the delegating turn had no room on it (a tool call with
 * no turn_room); the notice then falls back to the delegator-assignee
 * room.
 *
 * The setters below all take %NULL to clear the field.  They exist for
 * the manager and for reading a task back off the wire; ordinary code
 * goes through #ClawtTaskManager, which enforces the state machine.
 */
void clawt_task_set_room(ClawtTask *self, const gchar *room);
void clawt_task_set_parent_id(ClawtTask *self, const gchar *parent_id);
void clawt_task_set_reason(ClawtTask *self, const gchar *reason);
void clawt_task_set_model(ClawtTask *self, const gchar *model);
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
/**
 * clawt_task_get_holds_completion:
 * @self: a #ClawtTask
 *
 * Whether the assignee said its turn is over and the work is not, and
 * that has not yet been spent by a turn ending.  Read, not taken: this
 * is clawt_task_take_completion_hold()'s question without its effect.
 *
 * Returns: %TRUE when a hold is armed
 */
gboolean clawt_task_get_holds_completion(ClawtTask *self);

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

/**
 * clawt_task_thread_summary:
 * @thread_room: (nullable): a task's `thread_room`, as the wire carries it
 *
 * The line every client draws on a task that runs in a room of its own,
 * beside the control that opens that room -- one wording, so three
 * clients cannot describe the same room three ways.
 *
 * Returns: (transfer full) (nullable): the line, or %NULL for a task
 *   with no room of its own
 */
gchar *clawt_task_thread_summary(const gchar *thread_room);

/**
 * clawt_task_consult_label:
 *
 * The badge every client draws on a task delegated with `mode: consult`.
 *
 * Returns: (transfer none): the label
 */
const gchar *clawt_task_consult_label(void);

/**
 * clawt_task_state_is_retryable:
 * @state: a task state
 *
 * Whether a task in @state can be sent round again with
 * clawtilla_task_retry or a client's Retry: failed and stalled, and
 * nothing else.  One answer for the daemon's refusal and every client's
 * button, so no client offers a Retry the daemon will refuse.
 *
 * Returns: %TRUE for a failed or stalled task
 */
gboolean clawt_task_state_is_retryable(ClawtTaskState state);

/**
 * clawt_task_state_is_finished:
 * @state: a #ClawtTaskState
 *
 * Whether @state is one a task does not leave on its own: completed,
 * failed, cancelled or stalled.  clawt_task_is_finished() asks this.
 *
 * Returns: %TRUE for a terminal state
 */
gboolean clawt_task_state_is_finished(ClawtTaskState state);

/**
 * clawt_task_state_is_active:
 * @state: a #ClawtTaskState
 *
 * Whether the task is with its assignee: `pending` or `running`.  What a
 * turn timeout, an agent crash, orphaning or the idle-continuation guard
 * is about.  An unassigned, blocked or in-review task is waiting on
 * somebody other than its assignee and is none of those things' concern.
 *
 * Returns: %TRUE for pending and running
 */
gboolean clawt_task_state_is_active(ClawtTaskState state);

/**
 * clawt_task_state_is_cancellable:
 * @state: a #ClawtTaskState
 *
 * Whether a Cancel can do anything: every state that has not finished.
 * The one answer for the daemon and every client's button, so a blocked
 * or in-review task is not left without one.
 *
 * Returns: %TRUE when there is something to stop
 */
gboolean clawt_task_state_is_cancellable(ClawtTaskState state);

/**
 * clawt_task_state_nick_is_cancellable:
 * @nick: (nullable): a state as the wire spells it, e.g. `blocked`
 *
 * clawt_task_state_is_cancellable() for a client holding the wire's
 * nickname.  An unknown nickname is not cancellable.
 *
 * Returns: %TRUE when a Cancel button belongs on the row
 */
gboolean clawt_task_state_nick_is_cancellable(const gchar *nick);

/**
 * clawt_task_to_json:
 * @self: a #ClawtTask
 *
 * Every field of the task, as the task store keeps it.  The member names
 * are a file format: they are never renamed, and a member added later
 * reads as its zero value from a task saved before it existed.  The
 * state is written by nickname, so the file survives the enum being
 * reordered.
 *
 * This is not the IPC task object, which is shaped for a client and
 * leaves fields out; clawt_daemon_add_task_object() builds that.
 *
 * Returns: (transfer full): an object node
 */
JsonNode  *clawt_task_to_json(ClawtTask *self);

/**
 * clawt_task_new_from_json:
 * @node: what clawt_task_to_json() wrote
 * @error: (out) (optional): return location for a #GError
 *
 * The task @node describes, with every field restored, including the
 * owner history, the completion hold and the retry count.
 *
 * A node with no id or no prompt is refused.  A state this build does
 * not know is read as `failed`, with a warning: restoring it as
 * `pending` would be work that runs again.
 *
 * Returns: (transfer full) (nullable): the task, or %NULL on error
 */
ClawtTask *clawt_task_new_from_json(JsonNode *node, GError **error);

/**
 * clawt_task_build_held_delivery:
 * @via: `tool` (handed over through the delegation path) or `router`
 *   (sent as a message, as the operator's are)
 * @from: who it is from
 * @target: the assignee or the task's own room
 * @body: the work with its contract
 * @depth: the hop depth it carries
 * @priority: a #ClawtPriority
 * @turn_room: (nullable): the conversation it was delegated from
 *
 * The record a blocked task's delivery is kept as until its blockers
 * complete (clawt_task_manager_hold_delivery()): everything the daemon
 * needs to hand it over later exactly as it would have been then.
 *
 * Returns: (transfer full): an object node
 */
JsonNode *clawt_task_build_held_delivery(const gchar *via,
                                       const gchar *from,
                                       const gchar *target,
                                       const gchar *body,
                                       gint         depth,
                                       gint         priority,
                                       const gchar *turn_room);

/**
 * clawt_task_waiting_caption:
 * @waiting_on: (nullable): a task object's `waiting_on` array
 *
 * The line a held task's row shows, in every client's words: "waiting
 * for task-a, task-b to complete", three named and the rest counted.
 *
 * Returns: (transfer full): the caption, empty when nothing holds it
 */
gchar *clawt_task_waiting_caption(JsonArray *waiting_on);

/**
 * clawt_task_get_reviewer:
 * @self: a #ClawtTask
 *
 * Who checks the result before the task counts as done: an agent id, or
 * `user` for the operator (since 0.38.0).
 *
 * Returns: (transfer none) (nullable): the reviewer, or %NULL for none
 */
const gchar *clawt_task_get_reviewer(ClawtTask *self);

/**
 * clawt_task_set_reviewer:
 * @self: a #ClawtTask
 * @reviewer: (nullable): an agent id, `user`, or %NULL / empty for none
 */
void clawt_task_set_reviewer(ClawtTask *self, const gchar *reviewer);

/**
 * clawt_task_get_review_round:
 * @self: a #ClawtTask
 *
 * Returns: how many times changes have been asked for
 */
guint clawt_task_get_review_round(ClawtTask *self);

/**
 * clawt_task_set_review_round:
 * @self: a #ClawtTask
 * @round: the count
 */
void clawt_task_set_review_round(ClawtTask *self, guint round);

/**
 * clawt_task_resolve_reviewer:
 * @config: (nullable): the fleet, for `lead` and to check an agent id
 * @spec: (nullable): `lead`, `delegator`, `operator`, an agent id, or
 *   empty / `none` for no review
 * @origin: (nullable): who delegated the task
 * @assignee: (nullable): who does it
 * @error: (out) (optional): return location for a #GError
 *
 * Who reviews a task, resolved once when it is made: an agent id, `user`
 * for the operator, or %NULL for no review.  Nobody reviews their own
 * work: `lead` or `delegator` landing on the assignee means no review,
 * and naming the assignee outright is an error, as is an unknown id.
 *
 * Returns: (transfer full) (nullable): the reviewer, or %NULL (with
 *   @error set only when @spec was refused)
 */
gchar *clawt_task_resolve_reviewer(ClawtConfig  *config,
                                   const gchar  *spec,
                                   const gchar  *origin,
                                   const gchar  *assignee,
                                   GError      **error);

/**
 * clawt_task_assignment_guidance_for:
 * @task: the task being assigned
 *
 * clawt_task_assignment_guidance() with what else the assignee must know
 * from the start: today, its checkout (clawt_task_checkout_guidance()).
 * Since 0.39.0.
 *
 * Returns: (transfer full): the closing text of the assignment
 */
gchar *clawt_task_assignment_guidance_for(ClawtTask *task);

/**
 * clawt_task_get_project:
 * @self: a #ClawtTask
 *
 * Returns: (transfer none) (nullable): the project it belongs to
 *   (`projects.id`), since 0.40.0
 */
const gchar *clawt_task_get_project(ClawtTask *self);

/**
 * clawt_task_get_project_brief:
 * @self: a #ClawtTask
 *
 * Returns: (transfer none) (nullable): what its assignee is told about
 *   the project, as it was when the task was made
 */
const gchar *clawt_task_get_project_brief(ClawtTask *self);

/**
 * clawt_task_set_project:
 * @self: a #ClawtTask
 * @project: (nullable): a project id, or %NULL / empty for none
 * @brief: (nullable): the sentence its assignee is told (the project's
 *   name and goal); kept only with a project
 */
void clawt_task_set_project(ClawtTask   *self,
                            const gchar *project,
                            const gchar *brief);

/**
 * clawt_task_set_checkout:
 * @self: a #ClawtTask
 * @repository: (nullable): the host path of the repository checked out
 * @path: (nullable): the host path of the checkout; %NULL clears it
 * @branch: (nullable): the checkout's branch
 * @base: (nullable): the commit it started from
 * @mode: how it was made
 *
 * Records the task's own checkout (since 0.39.0).  A task with none has
 * %CLAWT_CHECKOUT_NONE.
 */
void clawt_task_set_checkout(ClawtTask         *self,
                             const gchar       *repository,
                             const gchar       *path,
                             const gchar       *branch,
                             const gchar       *base,
                             ClawtCheckoutMode  mode);

/**
 * clawt_task_get_checkout_mode:
 * @self: a #ClawtTask
 *
 * Returns: how its checkout was made, or %CLAWT_CHECKOUT_NONE
 */
ClawtCheckoutMode clawt_task_get_checkout_mode(ClawtTask *self);

/**
 * clawt_task_get_checkout_repo:
 * @self: a #ClawtTask
 *
 * Returns: (transfer none) (nullable): the repository checked out
 */
const gchar *clawt_task_get_checkout_repo(ClawtTask *self);

/**
 * clawt_task_get_checkout_path:
 * @self: a #ClawtTask
 *
 * Returns: (transfer none) (nullable): the checkout, a host path
 */
const gchar *clawt_task_get_checkout_path(ClawtTask *self);

/**
 * clawt_task_get_checkout_branch:
 * @self: a #ClawtTask
 *
 * Returns: (transfer none) (nullable): the checkout's branch
 */
const gchar *clawt_task_get_checkout_branch(ClawtTask *self);

/**
 * clawt_task_get_checkout_base:
 * @self: a #ClawtTask
 *
 * Returns: (transfer none) (nullable): the commit the work started from
 */
const gchar *clawt_task_get_checkout_base(ClawtTask *self);

/**
 * clawt_task_get_checkout_guest:
 * @self: a #ClawtTask
 *
 * The checkout as the agent's computer sees it, when that differs from
 * the host path (a container's or VM's workspace mount).
 *
 * Returns: (transfer none) (nullable): the path inside, or %NULL
 */
const gchar *clawt_task_get_checkout_guest(ClawtTask *self);

/**
 * clawt_task_set_checkout_guest:
 * @self: a #ClawtTask
 * @guest: (nullable): the checkout's path inside the agent's computer
 */
void clawt_task_set_checkout_guest(ClawtTask *self, const gchar *guest);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTask, clawt_task_free)

G_END_DECLS
