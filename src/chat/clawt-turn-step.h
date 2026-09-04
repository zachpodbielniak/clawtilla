/*
 * clawt-turn-step.h - One step of a turn that is still running
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-enums.h"
#include "core/clawt-event.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TURN_STEP (clawt_turn_step_get_type())

GType clawt_turn_step_get_type(void) G_GNUC_CONST;

/**
 * ClawtTurnStep:
 *
 * One thing an agent did partway through a turn: a tool it reached for,
 * or a paragraph it wrote between two of them.
 *
 * Deliberately not a #ClawtMessage, and the difference is the whole
 * safety property rather than a detail.  A message has a sender, an id
 * and a room, so it can be routed -- and routing is what starts a turn
 * in whoever receives it.  An agent watching a peer work would then run
 * a turn per tool call the peer made, answer it, and be watched in turn
 * by the peer.  The "Still working..." notes libreclaw used to post
 * were a slower version of exactly that, and were removed for it.
 *
 * A step therefore travels as an event and stops at the clients.  It
 * never enters #ClawtMailboxRouter, never reaches a mailbox, and has no
 * `invites_reply` to get wrong -- there is no code path from a step to
 * a delivery, which is a stronger statement than a flag nobody forgot
 * to check.
 *
 * It is also never persisted.  A step is not part of the answer, and a
 * transcript that keeps it would offer the last one as the turn's
 * result to anything that reads the thread's tail -- which is what
 * `clawt_task_manager_complete_on_turn_end()` does.
 */
typedef struct _ClawtTurnStep ClawtTurnStep;

/**
 * CLAWT_STEP_MEMBER_KIND:
 *
 * The member name a step's kind travels under, in a `turn.step` event's
 * details and in a `room.steps` reply alike.
 *
 * These are constants rather than string literals spelled at each end
 * because a member name is checked by nothing.  A reader that asks for
 * `kind` where the producer wrote `step_kind` gets its fallback on
 * every single call, silently -- this project has shipped that exact
 * bug on `status`, `type`, `scopes`, `exit_status`, `guest_path` and
 * `result_inferred`, each of which drew a working thing as broken and
 * reported itself to nobody.  With one definition the producer and the
 * two readers cannot drift, and a rename is a compile error.
 *
 * `step_kind` and not `kind` because a #ClawtEvent already has a kind,
 * and a detail named the same thing as the field beside it is a reader
 * waiting to pick the wrong one.
 */
#define CLAWT_STEP_MEMBER_KIND   "step_kind"

/**
 * CLAWT_STEP_MEMBER_ROOM:
 *
 * The room whose turn produced the step.
 */
#define CLAWT_STEP_MEMBER_ROOM   "room_id"

/**
 * CLAWT_STEP_MEMBER_TEXT:
 *
 * The prose, for a text, thinking or status step.
 */
#define CLAWT_STEP_MEMBER_TEXT   "text"

/**
 * CLAWT_STEP_MEMBER_TOOL:
 *
 * The tool's name, for a tool step.
 */
#define CLAWT_STEP_MEMBER_TOOL   "tool"

/**
 * CLAWT_STEP_MEMBER_DETAIL:
 *
 * The one-line preview of a tool's arguments.
 */
#define CLAWT_STEP_MEMBER_DETAIL "detail"

/**
 * CLAWT_STEP_MEMBER_FAILED:
 *
 * Whether a tool step failed.  A JSON boolean on the wire and an int
 * detail on an event, so each reader must use its own type's reader --
 * reading a JSON boolean with the string reader is the same failure as
 * a misspelled name, with nothing misspelled in it to notice.
 */
#define CLAWT_STEP_MEMBER_FAILED "failed"


/**
 * clawt_turn_step_new:
 * @kind: what the step was
 * @agent_id: the agent whose turn this is
 * @room_id: (nullable): the room its turn is running in
 * @text: (nullable): the prose, for a text, thinking or status step
 * @tool_name: (nullable): the tool, for a %CLAWT_STEP_TOOL step
 * @detail: (nullable): a one-line preview of what the tool was asked
 *   to do
 * @failed: whether a tool step reported failure
 *
 * Returns: (transfer full): a new step
 */
ClawtTurnStep *clawt_turn_step_new(ClawtStepKind  kind,
                                   const gchar   *agent_id,
                                   const gchar   *room_id,
                                   const gchar   *text,
                                   const gchar   *tool_name,
                                   const gchar   *detail,
                                   gboolean       failed);

/**
 * clawt_turn_step_new_from_event:
 * @event: a `turn.step` event
 *
 * Reads a step off the event that carried it.
 *
 * The one place any client reads these member names.  A member name is
 * checked by nothing: a client reading `name` off a reply that sends
 * `tool` gets its fallback on every call, silently, and this project
 * has shipped that bug six times over -- a working connector reporting
 * "not authorised", a successful command drawn red.  With one reader
 * the two clients cannot disagree, and the test that pins these names
 * to the daemon's `json_builder_set_member_name()` calls has one place
 * to point at.
 *
 * Returns: (transfer full) (nullable): the step, or %NULL if @event is
 *   not a `turn.step`
 */
ClawtTurnStep *clawt_turn_step_new_from_event(ClawtEvent *event);

/**
 * clawt_turn_step_new_from_object:
 * @object: one element of a `room.steps` reply
 * @agent_id: (nullable): the agent the reply was about
 *
 * The array form of clawt_turn_step_new_from_event(), for a client
 * catching up on a turn that was already running when it opened the
 * room.  Reads the same member names, from the same constants.
 *
 * Returns: (transfer full) (nullable): the step
 */
ClawtTurnStep *clawt_turn_step_new_from_object(JsonObject  *object,
                                               const gchar *agent_id);

ClawtTurnStep *clawt_turn_step_copy(ClawtTurnStep *self);
void           clawt_turn_step_free(ClawtTurnStep *self);

ClawtStepKind  clawt_turn_step_get_kind(ClawtTurnStep *self);
const gchar   *clawt_turn_step_get_agent_id(ClawtTurnStep *self);
const gchar   *clawt_turn_step_get_room_id(ClawtTurnStep *self);
const gchar   *clawt_turn_step_get_text(ClawtTurnStep *self);
const gchar   *clawt_turn_step_get_tool_name(ClawtTurnStep *self);
const gchar   *clawt_turn_step_get_detail(ClawtTurnStep *self);
gboolean       clawt_turn_step_get_failed(ClawtTurnStep *self);
gint64         clawt_turn_step_get_timestamp(ClawtTurnStep *self);

/**
 * clawt_turn_step_tone:
 * @self: a step
 *
 * How this step should read: one of "neutral", "good", "warn", "bad" or
 * "info" -- the same vocabulary clawt_task_state_tone() uses, so a
 * client maps one set of names to one set of styles.
 *
 * In the library because both clients draw this, and the last time a
 * tone was decided in a client it was decided by comparing an enum
 * against a nickname the enum does not produce, so every task badge was
 * grey for a year and nothing reported it -- a wrong colour looks like
 * somebody's design choice.
 *
 * Returns: (transfer none): the tone
 */
const gchar *clawt_turn_step_tone(ClawtTurnStep *self);

/**
 * clawt_turn_step_run_label:
 * @tools: how many consecutive tool steps are being collapsed
 * @failed: how many of them failed
 *
 * The line that stands in for a run of tool calls: "Ran 6 commands",
 * "Ran 13 commands (1 failed)".
 *
 * A run rather than a line each because a turn makes tens of calls and
 * a transcript listing every one buries the prose between them, which
 * is the part a person is actually reading.  The failures are counted
 * separately and always shown, because a run that contains one means
 * something different from one that does not -- and a count that hides
 * them would make a struggling turn look like a productive one.
 *
 * Both clients collapse the same way, so the wording lives here.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_turn_step_run_label(guint tools, guint failed);

/**
 * clawt_turn_step_summary:
 * @self: a step
 *
 * A single line describing @self, for a client with one line to spend:
 * `Bash: ls -la`, `Read`, or the first line of the prose.
 *
 * Returns: (transfer full): the summary
 */
gchar *clawt_turn_step_summary(ClawtTurnStep *self);

/**
 * clawt_turn_step_joins_run:
 * @self: a step
 *
 * Whether @self is the kind of step that collapses into a run with its
 * neighbours, rather than being drawn on its own.
 *
 * Returns: %TRUE for a tool step
 */
gboolean clawt_turn_step_joins_run(ClawtTurnStep *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTurnStep, clawt_turn_step_free)

G_END_DECLS
