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
 * It is never written to the *transcript*, though.  The last thing in
 * a thread is what `clawt_task_manager_complete_on_turn_end()` reads as
 * a task's result, and "Ran 6 commands" must not become somebody's
 * delegated outcome.  Steps live in the daemon's own bounded per-room
 * history instead, which a client merges back into the conversation on
 * time -- so they stay where they happened without ever being one of
 * the messages that thread is made of.
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
 * CLAWT_STEP_MEMBER_TS:
 *
 * When the step happened, in microseconds since the epoch -- the same
 * unit every other timestamp in this tree uses, and the unit
 * clawt_time_ago_label() takes.
 *
 * A client rebuilding a conversation fetches messages and steps
 * separately and merges them on this; without it a room's steps would
 * all pile up at the bottom, under the answers they came before.
 */
#define CLAWT_STEP_MEMBER_TS     "ts"


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
 * clawt_turn_step_run_extent:
 * @steps: (element-type ClawtTurnStep): a list of steps
 * @from: where the run starts
 * @end: one past the last index to consider
 * @out_calls: (out): how many tool *calls* the run contains
 * @out_failed: (out): how many of them failed
 *
 * Finds the end of the run of tool steps starting at @from, and counts
 * what is in it.
 *
 * The counting is the part worth having here.  A provider reports a
 * tool twice -- once when the model asks for it, and again when it
 * finishes -- and only the first carries the tool's name and arguments,
 * because Claude Code's finish event carries neither.  So a call is a
 * step with `failed` clear and an outcome is a step with it set, which
 * holds because a call is recorded before anyone knows how it went.
 *
 * Counting entries instead would say "Ran 2 commands" for one command
 * that failed, which is not a rounding error: it is a different claim
 * about what the agent did.  It did exactly that until this existed.
 *
 * Both clients collapse runs, so the walk and the counting live in one
 * place rather than being written twice and diverging on the case
 * neither author thought about.
 *
 * Returns: one past the last index in the run
 */
guint clawt_turn_step_run_extent(GPtrArray *steps,
                                 guint      from,
                                 guint      end,
                                 guint     *out_calls,
                                 guint     *out_failed);

/**
 * clawt_turn_step_is_call:
 * @self: a step
 *
 * Whether @self is a tool *call* -- something to list when a run is
 * opened -- rather than the outcome marker that follows a failure.
 *
 * An outcome carries no name and no arguments on at least one provider,
 * so listing it renders a row reading "a tool", which says nothing and
 * looks like a bug because it is one.
 *
 * Returns: %TRUE for a tool call
 */
gboolean clawt_turn_step_is_call(ClawtTurnStep *self);

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

/**
 * clawt_turn_step_precedes:
 * @self: a step
 * @message_ts: a #ClawtMessage timestamp, in *seconds*
 *
 * Whether @self happened at or before @message_ts, for a client
 * merging a room's steps back into its transcript.
 *
 * Here, and taking the two units by name, because they are not the
 * same one.  A step is stamped from #ClawtEvent's clock, which is
 * g_get_real_time() -- microseconds; a #ClawtMessage stamps itself in
 * seconds.  Comparing them raw puts every step after every message,
 * which draws a whole conversation's tool calls in a heap at the
 * bottom under answers they came before -- and it looks like an
 * ordering preference rather than a bug, so nothing reports it. The
 * same mismatch rendered `20694d ago` on every task row once.
 *
 * At-or-before rather than strictly before: within the one second a
 * message's stamp resolves to, a step that ties with it came first.
 * The message that ends a turn is written after the steps that
 * produced it, and the message that starts one cannot tie with a step
 * of its own turn.
 *
 * Returns: %TRUE if @self belongs before that message
 */
gboolean clawt_turn_step_precedes(ClawtTurnStep *self, gint64 message_ts);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTurnStep, clawt_turn_step_free)

G_END_DECLS
