/*
 * clawt-saved-steps.h - A desktop task's input, saved, and replayed with
 * no model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent that has once worked out which buttons to press on a screen
 * spends a model's turn working it out again every time it is asked.
 * Saved steps keep what it pressed: every acting desktop call of a turn
 * is recorded at the one place that sees them all -- the gate in front
 * of the MCP relay, `computer.control` -- and the operator can save a
 * finished turn's calls as a named list and replay it later, with no
 * model, through the same input path a person's takeover uses.
 *
 * Conservative on purpose, because a replayed click lands wherever the
 * screen has put something since:
 *
 *   - **Only what can be repeated exactly is saved.**  A call becomes a
 *     step only when it maps onto one #ClawtInputEvent with every value
 *     it needs (clawt_saved_step_from_call()): a key, typed text, a
 *     pointer move, a click, a scroll.  Anything else that acts --
 *     focusing a window by id, launching an application, a drag -- makes
 *     the whole turn unsaveable, with the reason.  Half a task replayed
 *     is worse than none.
 *   - **Typed text that looks like a secret is refused**, by the same
 *     redaction every log line goes through: a saved step is stored as
 *     typed, and a password in a file somebody may copy is a leak.
 *   - **A checkpoint is required.**  Two halves: the screen's hash
 *     (SHA-256 of the last picture taken after the turn began and before
 *     its first step) and the title of the window that had the focus.
 *     A list is checked by "picture and title" (the default) or by
 *     "title only", chosen per saved list; title only is for a screen
 *     with a ticking clock, which no picture can match twice.  A turn
 *     with no title cannot be saved at all, one with no picture only as
 *     title only, and a replay starts only when the screen matches the
 *     way the list says.  Pictures and titles are taken only of a screen
 *     somebody is watching, so a turn is saveable only if its screen was
 *     watched when it began.  Exact on purpose: a moved clock is a
 *     different picture, and the refusal says so rather than clicking on
 *     a guess.
 *   - **Only while the operator holds the screen.**  Replay is input a
 *     person sends, through `computer.input`'s path and behind the same
 *     takeover: the agent is refused its own acting calls for as long as
 *     it is held, so the two cannot fight for the pointer.  Releasing the
 *     screen mid-replay stops it at the next step.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "computer/clawt-observable.h"

G_BEGIN_DECLS

/**
 * CLAWT_SAVED_STEPS_MAX:
 *
 * The most steps one turn records.  A turn past it is not saveable --
 * a truncated task replayed is a different task.
 */
#define CLAWT_SAVED_STEPS_MAX (200)

/**
 * ClawtStepCheck:
 * @CLAWT_STEP_CHECK_PICTURE_AND_TITLE: a replay needs the screen's
 *   picture to hash the same and the focused window's title to match
 * @CLAWT_STEP_CHECK_TITLE_ONLY: a replay needs only the focused window's
 *   title to match -- for a screen with a clock on it
 *
 * How a saved list's checkpoint is compared before a replay.
 */
typedef enum {
	CLAWT_STEP_CHECK_PICTURE_AND_TITLE = 0,
	CLAWT_STEP_CHECK_TITLE_ONLY
} ClawtStepCheck;

GType
clawt_step_check_get_type(
	void
) G_GNUC_CONST;
#define CLAWT_TYPE_STEP_CHECK (clawt_step_check_get_type())

/**
 * clawt_step_check_label:
 * @check: a check
 *
 * "Picture and title" or "Title only", as the three clients say it.
 *
 * Returns: (transfer none): the label
 */
const gchar *
clawt_step_check_label(
	ClawtStepCheck check
);

/**
 * clawt_step_check_other:
 * @check: a check
 *
 * The one a saved list would switch to -- the clients offer a single
 * button rather than a list of two.
 *
 * Returns: the other check
 */
ClawtStepCheck
clawt_step_check_other(
	ClawtStepCheck check
);

/**
 * clawt_step_check_describe:
 * @check: how the list is checked
 * @title: (nullable): the title it was saved with
 *
 * "Checked by picture and title (“Report — Editor”)": the line the
 * three clients draw under a saved list.
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_step_check_describe(
	ClawtStepCheck  check,
	const gchar    *title
);

/**
 * clawt_saved_steps_check_screen:
 * @check: how the list is checked
 * @saved_checkpoint: (nullable): the picture's hash it was saved with
 * @saved_title: (nullable): the focused title it was saved with
 * @hash: (nullable): the screen's hash now; not read for title only
 * @title: (nullable): the focused title now, %NULL when unknown
 * @why_not: (out) (transfer full) (optional): when it does not match,
 *   the sentence saying which half did not
 *
 * Whether the screen now is the screen the list was saved from.  An
 * unknown title never matches, nor does a list with no title, nor a
 * picture-and-title list with no picture: refused, never guessed.
 *
 * Returns: %TRUE when a replay may start
 */
gboolean
clawt_saved_steps_check_screen(
	ClawtStepCheck   check,
	const gchar     *saved_checkpoint,
	const gchar     *saved_title,
	const gchar     *hash,
	const gchar     *title,
	gchar          **why_not
);

/**
 * ClawtStepRun:
 * @events: (element-type ClawtInputEvent): the steps, in order
 * @calls: how many acting calls the turn made
 * @saveable: whether its steps can be repeated exactly; a checkpoint is
 *   asked about separately, per check
 * @why_not: (nullable): when they cannot, the sentence saying why
 * @checkpoint: (nullable): SHA-256 of the screen before the first step
 * @finished_at: unix seconds the turn ended, 0 while it runs
 * @title: (nullable): the focused window's title when the turn began,
 *   "" for none focused, %NULL when it could not be read
 *
 * One turn's desktop input, as recorded.
 */
typedef struct {
	GPtrArray *events;
	guint      calls;
	gboolean   saveable;
	gchar     *why_not;
	gchar     *checkpoint;
	gint64     finished_at;
	gchar     *title;
} ClawtStepRun;

#define CLAWT_TYPE_STEP_RUN (clawt_step_run_get_type())

GType
clawt_step_run_get_type(
	void
) G_GNUC_CONST;

/**
 * clawt_step_run_new:
 *
 * Returns: (transfer full): an empty, saveable run
 */
ClawtStepRun *
clawt_step_run_new(
	void
);

/**
 * clawt_step_run_copy:
 * @self: a run
 *
 * Returns: (transfer full): a deep copy
 */
ClawtStepRun *
clawt_step_run_copy(
	const ClawtStepRun *self
);

/**
 * clawt_step_run_free:
 * @self: (nullable): a run
 */
void
clawt_step_run_free(
	ClawtStepRun *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtStepRun, clawt_step_run_free)

/**
 * clawt_step_run_can_save:
 * @self: a run
 * @check: how it would be checked
 * @why_not: (out) (transfer none) (optional): when it cannot, why
 *
 * Whether @self can be saved to be checked by @check: its steps can be
 * repeated, there is at least one, the title was read, and -- for
 * picture and title -- there is a picture.
 *
 * Returns: %TRUE when it can
 */
gboolean
clawt_step_run_can_save(
	const ClawtStepRun  *self,
	ClawtStepCheck       check,
	const gchar        **why_not
);

/**
 * clawt_saved_step_from_call:
 * @tool: the desktop tool the agent called
 * @arguments: (nullable): its arguments
 * @pointer_x: (inout): where the pointer is, as far as the run knows;
 *   -1 when unknown
 * @pointer_y: (inout): the same, y
 * @why_not: (out) (transfer full) (optional): when the call cannot be a
 *   step, why
 *
 * One acting call as a step, for gowl's and gnome-desktop-mcp's
 * spellings: `send_key`/`key_combo`/`key_press`, `send_text`/`type_text`,
 * `send_mouse_move`/`mouse_move`, `send_mouse` (a click where the pointer
 * is)/`mouse_click`, `send_scroll`/`mouse_scroll`.  A gowl click needs a
 * move before it, so the pointer is carried across calls.
 *
 * Returns: (transfer full) (nullable): the step, or %NULL
 */
ClawtInputEvent *
clawt_saved_step_from_call(
	const gchar  *tool,
	JsonObject   *arguments,
	gint         *pointer_x,
	gint         *pointer_y,
	gchar       **why_not
);

/**
 * clawt_saved_step_describe:
 * @event: a step
 *
 * "Click at 120, 340", "Type “hello”", "Press ctrl+s" -- what the three
 * clients draw for one step, with typed text passed through redaction.
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_saved_step_describe(
	ClawtInputEvent *event
);

/**
 * clawt_saved_step_to_json:
 * @event: a step
 *
 * Returns: (transfer full): the step as an object
 */
JsonNode *
clawt_saved_step_to_json(
	ClawtInputEvent *event
);

/**
 * clawt_saved_step_from_json:
 * @object: what clawt_saved_step_to_json() wrote
 *
 * Returns: (transfer full) (nullable): the step, or %NULL when it is not
 *   one
 */
ClawtInputEvent *
clawt_saved_step_from_json(
	JsonObject *object
);

#define CLAWT_TYPE_STEP_RECORDER (clawt_step_recorder_get_type())

G_DECLARE_FINAL_TYPE(ClawtStepRecorder, clawt_step_recorder, CLAWT,
                     STEP_RECORDER, GObject)

/**
 * clawt_step_recorder_new:
 *
 * Returns: (transfer full): a recorder holding nothing
 */
ClawtStepRecorder *
clawt_step_recorder_new(
	void
);

/**
 * clawt_step_recorder_begin_turn:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose turn
 * @now_usec: wall-clock microseconds
 *
 * Starts a fresh run for @agent_id.  The previous finished run stays
 * until this one has a step, so a turn that never touched the screen
 * does not throw away the last one that did.
 */
void
clawt_step_recorder_begin_turn(
	ClawtStepRecorder *self,
	const gchar       *agent_id,
	gint64             now_usec
);

/**
 * clawt_step_recorder_get_began:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose turn
 *
 * Returns: wall-clock microseconds the running turn began, 0 when none
 */
gint64
clawt_step_recorder_get_began(
	ClawtStepRecorder *self,
	const gchar       *agent_id
);

/**
 * clawt_step_recorder_note_call:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose turn
 * @tool: the acting tool it called
 * @arguments: (nullable): the call's arguments
 * @checkpoint: (nullable): SHA-256 of the screen before this call, read
 *   only for the first call of the run
 *
 * Records one acting call.  A call that is not a step marks the run
 * unsaveable, with the first reason kept.
 */
void
clawt_step_recorder_note_call(
	ClawtStepRecorder *self,
	const gchar       *agent_id,
	const gchar       *tool,
	JsonObject        *arguments,
	const gchar       *checkpoint
);

/**
 * clawt_step_recorder_wants_checkpoint:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose turn
 *
 * Returns: %TRUE when the running turn has not made an acting call yet,
 *   so the next one needs the screen's hash
 */
gboolean
clawt_step_recorder_wants_checkpoint(
	ClawtStepRecorder *self,
	const gchar       *agent_id
);

/**
 * clawt_step_recorder_note_title:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose screen
 * @title: (nullable): the focused window's title, %NULL when unread
 * @asked_usec: wall-clock microseconds the title was asked for
 *
 * Keeps the last title read of @agent_id's screen.  The running turn's
 * first acting call takes it only when it was asked for after the turn
 * began, as the picture is.
 */
void
clawt_step_recorder_note_title(
	ClawtStepRecorder *self,
	const gchar       *agent_id,
	const gchar       *title,
	gint64             asked_usec
);

/**
 * clawt_step_recorder_end_turn:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose turn
 * @now_usec: wall-clock microseconds
 *
 * Finishes the running turn.  A run with calls becomes the agent's last
 * run; one with none is dropped.
 *
 * Returns: %TRUE when a new last run was kept
 */
gboolean
clawt_step_recorder_end_turn(
	ClawtStepRecorder *self,
	const gchar       *agent_id,
	gint64             now_usec
);

/**
 * clawt_step_recorder_get_last:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose
 *
 * Returns: (transfer none) (nullable): the last finished run with calls
 */
const ClawtStepRun *
clawt_step_recorder_get_last(
	ClawtStepRecorder *self,
	const gchar       *agent_id
);

/**
 * clawt_step_recorder_forget:
 * @self: a #ClawtStepRecorder
 * @agent_id: whose
 *
 * Drops everything about @agent_id: its process ended.
 */
void
clawt_step_recorder_forget(
	ClawtStepRecorder *self,
	const gchar       *agent_id
);

/**
 * clawt_saved_steps_save:
 * @dir: `<state_dir>/saved-steps`
 * @agent_id: whose
 * @name: what to call it: letters, digits, spaces, `-`, `_` and `.`, at
 *   most 64 characters
 * @run: the run to save
 * @check: how a replay is to be checked
 * @error: (out) (optional): return location for a #GError
 *
 * Saves @run under @name in `<dir>/<agent>.yaml`, 0600, replacing one of
 * the same name, with its picture's hash, its title and @check.  Refused
 * for a run clawt_step_run_can_save() refuses for @check, and for a name
 * that is not one.
 *
 * Returns: %TRUE when saved
 */
gboolean
clawt_saved_steps_save(
	const gchar         *dir,
	const gchar         *agent_id,
	const gchar         *name,
	const ClawtStepRun  *run,
	ClawtStepCheck       check,
	GError             **error
);

/**
 * clawt_saved_steps_set_check:
 * @dir: `<state_dir>/saved-steps`
 * @agent_id: whose
 * @name: which
 * @check: the check from now on
 * @error: (out) (optional): return location for a #GError
 *
 * Switches how one saved list is checked.  Picture and title is refused
 * for a list saved with no picture.
 *
 * Returns: %TRUE when changed
 */
gboolean
clawt_saved_steps_set_check(
	const gchar     *dir,
	const gchar     *agent_id,
	const gchar     *name,
	ClawtStepCheck   check,
	GError         **error
);

/**
 * clawt_saved_steps_list:
 * @dir: `<state_dir>/saved-steps`
 * @agent_id: whose
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): an array of `name`, `created_at`,
 *   `checkpoint`, `title`, `check` and `steps` objects, empty when
 *   nothing is saved
 */
JsonNode *
clawt_saved_steps_list(
	const gchar  *dir,
	const gchar  *agent_id,
	GError      **error
);

/**
 * clawt_saved_steps_get:
 * @dir: `<state_dir>/saved-steps`
 * @agent_id: whose
 * @name: which
 * @checkpoint: (out) (transfer full) (nullable): the screen hash it
 *   starts from; %NULL for a list saved with none
 * @title: (out) (transfer full) (nullable): the focused title it starts
 *   from; %NULL for a list saved before titles were recorded
 * @check: (out) (optional): how it is checked
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type ClawtInputEvent) (nullable): the
 *   steps, or %NULL when there is no such list
 */
GPtrArray *
clawt_saved_steps_get(
	const gchar     *dir,
	const gchar     *agent_id,
	const gchar     *name,
	gchar          **checkpoint,
	gchar          **title,
	ClawtStepCheck  *check,
	GError         **error
);

/**
 * clawt_saved_steps_delete:
 * @dir: `<state_dir>/saved-steps`
 * @agent_id: whose
 * @name: which
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was there and is gone
 */
gboolean
clawt_saved_steps_delete(
	const gchar  *dir,
	const gchar  *agent_id,
	const gchar  *name,
	GError      **error
);

G_END_DECLS
