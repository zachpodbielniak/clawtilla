/*
 * clawt-crosstalk.h - Telling the operator about agent-to-agent exchanges
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_CROSSTALK_H
#define CLAWT_CROSSTALK_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <ai-glib.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * CLAWT_CROSSTALK_QUIET_SECONDS:
 *
 * How long an exchange must have been quiet -- no message, no step, no
 * turn starting or ending in its room -- before it counts as ended.
 * Long enough to cover the gap between a turn's typing indicator
 * dropping and its answer being posted, which is real: libreclaw stops
 * the indicator before it posts.
 */
#define CLAWT_CROSSTALK_QUIET_SECONDS (15)

/**
 * CLAWT_CROSSTALK_COALESCE_SECONDS:
 *
 * Summaries for one conversation that end within this long of each
 * other are posted as one note with a section each, rather than as a
 * burst of notes.
 */
#define CLAWT_CROSSTALK_COALESCE_SECONDS (5)

/**
 * CLAWT_CROSSTALK_STALE_SECONDS:
 *
 * How long an exchange may stay open while something still holds it --
 * a message waiting for a stopped agent, a task running for hours --
 * before it is summarised as it stands.  A summary that arrives with
 * "still working" is better than one that never arrives.
 */
#define CLAWT_CROSSTALK_STALE_SECONDS (3600)

/**
 * CLAWT_CROSSTALK_ASK_CHARS:
 *
 * The longest the quoted ask may be, in characters.
 */
#define CLAWT_CROSSTALK_ASK_CHARS (160)

/**
 * CLAWT_CROSSTALK_MODEL_TIMEOUT_MS:
 *
 * How long `orchestration.crosstalk_summary_model` has to answer before
 * the summary built from the record is posted instead.
 */
#define CLAWT_CROSSTALK_MODEL_TIMEOUT_MS (10000)

/**
 * CLAWT_CROSSTALK_MODEL_MAX_CHARS:
 *
 * The longest rewrite that is accepted, in characters.  Anything longer
 * is not the one paragraph that was asked for.
 */
#define CLAWT_CROSSTALK_MODEL_MAX_CHARS (1200)

/* ── The setting ─────────────────────────────────────────────────── */

/**
 * clawt_crosstalk_summary_to_count:
 *
 * How many values #ClawtCrosstalkSummaryTo has, for a client walking
 * them in order -- the position is the value.
 *
 * Returns: the count
 *
 * Since: 0.33.0
 */
guint clawt_crosstalk_summary_to_count(void);

/**
 * clawt_crosstalk_summary_to_nick:
 * @setting: a #ClawtCrosstalkSummaryTo
 *
 * Returns: (transfer none): what is written in clawtilla.yaml for it
 *
 * Since: 0.33.0
 */
const gchar *clawt_crosstalk_summary_to_nick(ClawtCrosstalkSummaryTo setting);

/**
 * clawt_crosstalk_summary_to_from_nick:
 * @nick: (nullable): a stored value
 *
 * Reads a stored value.  An empty or unknown one is
 * %CLAWT_CROSSTALK_SUMMARY_TO_SOURCE, the schema's default.
 *
 * Returns: the setting
 *
 * Since: 0.33.0
 */
ClawtCrosstalkSummaryTo clawt_crosstalk_summary_to_from_nick(const gchar *nick);

/**
 * clawt_crosstalk_summary_to_title:
 *
 * The control's title in every client's agent settings.
 *
 * Returns: (transfer none): "Where those summaries go"
 *
 * Since: 0.33.0
 */
const gchar *clawt_crosstalk_summary_to_title(void);

/**
 * clawt_crosstalk_summary_to_label:
 * @setting: a #ClawtCrosstalkSummaryTo
 *
 * How a client names one choice.
 *
 * Returns: (transfer none): the label
 *
 * Since: 0.33.0
 */
const gchar *clawt_crosstalk_summary_to_label(ClawtCrosstalkSummaryTo setting);

/**
 * clawt_crosstalk_summary_to_explanation:
 * @setting: a #ClawtCrosstalkSummaryTo
 *
 * What one choice does, in the words every client shows under it.
 *
 * Returns: (transfer none): the explanation
 *
 * Since: 0.33.0
 */
const gchar *clawt_crosstalk_summary_to_explanation(ClawtCrosstalkSummaryTo setting);

/**
 * clawt_crosstalk_summary_count:
 *
 * How many values #ClawtCrosstalkSummary has, for a client walking them
 * in order -- the position is the value.
 *
 * Returns: the count
 *
 * Since: 0.30.0
 */
guint clawt_crosstalk_summary_count(void);

/**
 * clawt_crosstalk_summary_nick:
 * @setting: a #ClawtCrosstalkSummary
 *
 * Returns: (transfer none): what is written in clawtilla.yaml for it
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_summary_nick(ClawtCrosstalkSummary setting);

/**
 * clawt_crosstalk_summary_from_nick:
 * @nick: (nullable): a stored value
 *
 * Reads a stored value.  An empty or unknown one is
 * %CLAWT_CROSSTALK_SUMMARY_CHIEF, the schema's default.
 *
 * Returns: the setting
 *
 * Since: 0.30.0
 */
ClawtCrosstalkSummary clawt_crosstalk_summary_from_nick(const gchar *nick);

/**
 * clawt_crosstalk_summary_title:
 *
 * The control's title in every client's agent settings.
 *
 * Returns: (transfer none): "Tell me about agent-to-agent conversations"
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_summary_title(void);

/**
 * clawt_crosstalk_summary_label:
 * @setting: a #ClawtCrosstalkSummary
 *
 * How a client names one choice.
 *
 * Returns: (transfer none): the label
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_summary_label(ClawtCrosstalkSummary setting);

/**
 * clawt_crosstalk_summary_explanation:
 * @setting: a #ClawtCrosstalkSummary
 *
 * The sentence under the control, following the choice, so three
 * clients say what the daemon does in one set of words.
 *
 * Returns: (transfer none): the explanation
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_summary_explanation(ClawtCrosstalkSummary setting);

/**
 * clawt_crosstalk_summary_applies:
 * @setting: the agent's `crosstalk_summary`, as resolved through the
 *   fleet's `orchestration.crosstalk_summary`
 * @is_chief: the agent is the fleet's chief of staff
 *
 * Whether the agent's conversation with its operator gets summaries.
 *
 * Returns: %TRUE when it does
 *
 * Since: 0.30.0
 */
gboolean clawt_crosstalk_summary_applies(ClawtCrosstalkSummary setting,
                                         gboolean              is_chief);

/* ── What counts as cross-talk ───────────────────────────────────── */

/**
 * clawt_crosstalk_room_counts:
 * @room_id: (nullable): a room
 * @members: (nullable) (array zero-terminated=1): its members
 *
 * Whether what is said in @room_id is cross-talk: the rule the Flow
 * page lists conversations by, so a summary is about something Flow
 * shows.  A room the operator is a member of is their own chat and
 * never counts; neither do a routine's, a trigger's or an incident
 * room, because work a schedule or a webhook started is not agents
 * reaching out to each other.
 *
 * Returns: %TRUE when the room is cross-talk
 *
 * Since: 0.30.0
 */
gboolean clawt_crosstalk_room_counts(const gchar        *room_id,
                                     const gchar *const *members);

/* ── How the clients draw a summary ──────────────────────────────── */

/**
 * clawt_crosstalk_mark_label:
 *
 * The badge every client draws on a summary note.
 *
 * Returns: (transfer none): the label
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_mark_label(void);

/**
 * clawt_crosstalk_mark_tooltip:
 *
 * The longer explanation behind clawt_crosstalk_mark_label().
 *
 * Returns: (transfer none): the explanation
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_mark_tooltip(void);

/**
 * clawt_crosstalk_flow_label:
 * @index: which exchange, counted from 0
 * @total: how many the note summarises
 *
 * The label of the action that opens one exchange in Flow: "View in
 * Flow" for a note about one, "View 2 in Flow" for the second section
 * of a note about several -- the number its section carries.
 *
 * Returns: (transfer full): the label
 *
 * Since: 0.30.0
 */
gchar *clawt_crosstalk_flow_label(guint index,
                                  guint total);

/**
 * clawt_crosstalk_split_targets:
 * @list: (nullable): a `crosstalk_room` or `crosstalk_message` member
 *
 * Splits one of a summary's space-separated target lists.
 *
 * Returns: (transfer full) (array zero-terminated=1): the items, empty
 *   for %NULL
 *
 * Since: 0.30.0
 */
GStrv clawt_crosstalk_split_targets(const gchar *list);

/**
 * clawt_crosstalk_anchor:
 * @room_id: a room
 *
 * The element id the web client's Flow page gives a conversation's
 * card, and the fragment a "View in Flow" link jumps to: `flow-` and
 * the room id with anything but letters, digits and `-` turned into
 * `-`.
 *
 * Returns: (transfer full): the anchor
 *
 * Since: 0.30.0
 */
gchar *clawt_crosstalk_anchor(const gchar *room_id);

/**
 * clawt_crosstalk_outcome_label:
 * @outcome: a #ClawtCrosstalkOutcome
 *
 * Returns: (transfer none): how a summary says it ended
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_outcome_label(ClawtCrosstalkOutcome outcome);

/* ── One exchange ────────────────────────────────────────────────── */

/**
 * ClawtCrosstalkExchange:
 *
 * One exchange in one cross-talk room, from its first message to the
 * moment it went quiet: who reached out and with what, who spoke, what
 * the agents in it did, and how it ended.  Built by #ClawtCrosstalk
 * from what the daemon observed, never from what a model said.
 *
 * Since: 0.30.0
 */
typedef struct _ClawtCrosstalkExchange ClawtCrosstalkExchange;

GType clawt_crosstalk_exchange_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_CROSSTALK_EXCHANGE (clawt_crosstalk_exchange_get_type())

/**
 * clawt_crosstalk_exchange_ref:
 * @self: a #ClawtCrosstalkExchange
 *
 * Returns: (transfer full): @self
 */
ClawtCrosstalkExchange *clawt_crosstalk_exchange_ref(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_unref:
 * @self: (transfer full): a #ClawtCrosstalkExchange
 */
void clawt_crosstalk_exchange_unref(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_get_room_id:
 * @self: a #ClawtCrosstalkExchange
 *
 * The accessors below read what #ClawtCrosstalk recorded.  Documented as
 * a group: the room, who sent the first message, the first line of what
 * they asked (escaped and clipped), the first and the last message's
 * ids, the group round (0 outside a group room) and how many messages
 * were said.
 *
 * Returns: (transfer none): the room
 */
const gchar *clawt_crosstalk_exchange_get_room_id(ClawtCrosstalkExchange *self);
const gchar *clawt_crosstalk_exchange_get_initiator(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_get_source_room:
 * @self: a #ClawtCrosstalkExchange
 *
 * The conversation whose turn began the exchange, when that is not the
 * exchange's own room: the agent that opened it was answering somebody
 * there (a Matrix room, a group) -- the request context
 * (clawt_message_get_request_room()) of the exchange's first message.
 *
 * Returns: (transfer none) (nullable): the room, or %NULL
 *
 * Since: 0.33.0
 */
const gchar *clawt_crosstalk_exchange_get_source_room(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_get_source_agent:
 * @self: a #ClawtCrosstalkExchange
 *
 * Whose turn in clawt_crosstalk_exchange_get_source_room() began the
 * exchange -- the agent the summary of it goes back for.
 *
 * Returns: (transfer none) (nullable): the agent, or %NULL
 *
 * Since: 0.33.0
 */
const gchar *clawt_crosstalk_exchange_get_source_agent(ClawtCrosstalkExchange *self);
const gchar *clawt_crosstalk_exchange_get_ask(ClawtCrosstalkExchange *self);
const gchar *clawt_crosstalk_exchange_get_first_message_id(ClawtCrosstalkExchange *self);
const gchar *clawt_crosstalk_exchange_get_last_message_id(ClawtCrosstalkExchange *self);
guint        clawt_crosstalk_exchange_get_round(ClawtCrosstalkExchange *self);
guint        clawt_crosstalk_exchange_get_message_count(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_get_outcome:
 * @self: a #ClawtCrosstalkExchange
 *
 * How it ended, decided when it ended: a task it carried settling
 * outranks a refusal, which outranks an answer.
 *
 * Returns: the outcome
 */
ClawtCrosstalkOutcome clawt_crosstalk_exchange_get_outcome(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_dup_participants:
 * @self: a #ClawtCrosstalkExchange
 *
 * Everybody the exchange involved: whoever spoke in it, and both
 * members of a two-member room even when one never answered.  Agents,
 * `grok:` bots, "user" and %CLAWT_SYSTEM_SENDER alike; the daemon picks
 * the agents out.
 *
 * Returns: (transfer full) (array zero-terminated=1): the ids
 */
GStrv clawt_crosstalk_exchange_dup_participants(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_dup_members:
 * @self: a #ClawtCrosstalkExchange
 *
 * The room's members when the exchange opened -- who could be holding
 * it open with a turn or a waiting message.
 *
 * Returns: (transfer full) (array zero-terminated=1): the ids
 */
GStrv clawt_crosstalk_exchange_dup_members(ClawtCrosstalkExchange *self);

/**
 * clawt_crosstalk_exchange_dup_open_tasks:
 * @self: a #ClawtCrosstalkExchange
 *
 * The tasks a message in the exchange carried that have not been seen
 * to finish.  While one of these is running the exchange is not over:
 * a delegation's end is its settlement.
 *
 * Returns: (transfer full) (array zero-terminated=1): the task ids
 */
GStrv clawt_crosstalk_exchange_dup_open_tasks(ClawtCrosstalkExchange *self);

/* ── The tracker ─────────────────────────────────────────────────── */

#define CLAWT_TYPE_CROSSTALK (clawt_crosstalk_get_type())

G_DECLARE_FINAL_TYPE(ClawtCrosstalk, clawt_crosstalk, CLAWT, CROSSTALK, GObject)

/**
 * ClawtCrosstalkBusyFunc:
 * @exchange: an exchange that has gone quiet
 * @user_data: as given to clawt_crosstalk_collect()
 *
 * Whether something still holds @exchange open: a member's turn running
 * in its room, a message waiting in a member's mailbox for it, or a
 * task it carried still running.
 *
 * Returns: %TRUE while it is held
 *
 * Since: 0.30.0
 */
typedef gboolean (*ClawtCrosstalkBusyFunc)(ClawtCrosstalkExchange *exchange,
                                           gpointer                user_data);

/**
 * clawt_crosstalk_new:
 *
 * Returns: (transfer full): a tracker with nothing open
 *
 * Since: 0.30.0
 */
ClawtCrosstalk *clawt_crosstalk_new(void);

/**
 * clawt_crosstalk_note_message:
 * @self: a #ClawtCrosstalk
 * @room_id: the room it was said in
 * @members: (nullable) (array zero-terminated=1): the room's members
 * @message_id: (nullable): the message's id
 * @sender: who said it
 * @body: what was said
 * @task_id: (nullable): the task it belongs to
 * @round: its group round, or 0
 * @now: wall-clock microseconds
 *
 * Folds one message into the open exchange in its room, opening one if
 * there is none.  A message in a room that is not cross-talk
 * (clawt_crosstalk_room_counts()) is ignored, and so is a summary note
 * itself, which lands in the operator's room.  A message from a new
 * group round closes the exchange the previous round left open, so
 * each round is summarised once.
 *
 * Returns: %TRUE when the message was folded in
 *
 * Since: 0.30.0
 */
gboolean clawt_crosstalk_note_message(ClawtCrosstalk     *self,
                                      const gchar        *room_id,
                                      const gchar *const *members,
                                      const gchar        *message_id,
                                      const gchar        *sender,
                                      const gchar        *body,
                                      const gchar        *task_id,
                                      guint               round,
                                      gint64              now);

/**
 * clawt_crosstalk_note_source:
 * @self: a #ClawtCrosstalk
 * @room_id: (nullable): the exchange's room
 * @message_id: (nullable): the message the request context came on
 * @agent_id: (nullable): who sent it
 * @source_room: (nullable): the room that sender's turn was in
 *
 * Where the exchange open in @room_id came from.  Taken only from the
 * message that opened it, and only when @agent_id opened it and
 * @source_room is another room: an answer echoes the asker's context,
 * which is not where the answer came from.  Call it after
 * clawt_crosstalk_note_message() for the same message.
 *
 * Since: 0.33.0
 */
void clawt_crosstalk_note_source(ClawtCrosstalk *self,
                                 const gchar    *room_id,
                                 const gchar    *message_id,
                                 const gchar    *agent_id,
                                 const gchar    *source_room);

/**
 * clawt_crosstalk_note_activity:
 * @self: a #ClawtCrosstalk
 * @room_id: (nullable): a room
 * @now: wall-clock microseconds
 *
 * Something happened in @room_id that is not a message -- a turn began
 * or ended there.  Restarts the quiet clock of the exchange open there,
 * so the quiet is counted from the turn's end and not from the message
 * that started it.
 *
 * Since: 0.30.0
 */
void clawt_crosstalk_note_activity(ClawtCrosstalk *self,
                                   const gchar    *room_id,
                                   gint64          now);

/**
 * clawt_crosstalk_note_step:
 * @self: a #ClawtCrosstalk
 * @agent_id: the agent whose turn took the step
 * @room_id: (nullable): the room the turn runs in
 * @tool: (nullable): the tool it called
 * @now: wall-clock microseconds
 *
 * Counts one tool call against the agent in the exchange open in
 * @room_id, by the tool's #ClawtActionClass.  A step in a room with no
 * open exchange is not cross-talk and is ignored.
 *
 * Since: 0.30.0
 */
void clawt_crosstalk_note_step(ClawtCrosstalk *self,
                               const gchar    *agent_id,
                               const gchar    *room_id,
                               const gchar    *tool,
                               gint64          now);

/**
 * clawt_crosstalk_note_task:
 * @self: a #ClawtCrosstalk
 * @task_id: the task
 * @room_id: (nullable): the room it was delegated from
 * @origin: (nullable): who delegated it
 * @assignee: (nullable): who it was handed to
 * @state: its #ClawtTaskState now
 * @now: wall-clock microseconds
 *
 * A task changed.  One delegated from a room with an open exchange, by
 * somebody in it, is recorded as something that agent did; every
 * exchange carrying or having delegated it learns its state.
 *
 * Since: 0.30.0
 */
void clawt_crosstalk_note_task(ClawtCrosstalk *self,
                               const gchar    *task_id,
                               const gchar    *room_id,
                               const gchar    *origin,
                               const gchar    *assignee,
                               gint            state,
                               gint64          now);

/**
 * clawt_crosstalk_note_decision:
 * @self: a #ClawtCrosstalk
 * @agent_id: who asked
 * @room_id: (nullable): the room it was asked from
 * @now: wall-clock microseconds
 *
 * Counts a decision the agent raised in the exchange open in @room_id.
 *
 * Since: 0.30.0
 */
void clawt_crosstalk_note_decision(ClawtCrosstalk *self,
                                   const gchar    *agent_id,
                                   const gchar    *room_id,
                                   gint64          now);

/**
 * clawt_crosstalk_note_refused:
 * @self: a #ClawtCrosstalk
 * @room_id: (nullable): the room the refused message was for
 * @now: wall-clock microseconds
 *
 * A message in @room_id was refused by a loop guard.
 *
 * Since: 0.30.0
 */
void clawt_crosstalk_note_refused(ClawtCrosstalk *self,
                                  const gchar    *room_id,
                                  gint64          now);

/**
 * clawt_crosstalk_collect:
 * @self: a #ClawtCrosstalk
 * @now: wall-clock microseconds
 * @busy: (scope call) (nullable): whether something still holds an
 *   exchange open
 * @user_data: data for @busy
 *
 * Takes every exchange that has ended out of the tracker.  An exchange
 * ends when a new round in its room closed it, or when it has been
 * quiet for %CLAWT_CROSSTALK_QUIET_SECONDS and @busy says nothing holds
 * it -- or, whatever @busy says, after %CLAWT_CROSSTALK_STALE_SECONDS
 * of quiet.  Each exchange is returned exactly once.
 *
 * Returns: (transfer full) (element-type ClawtCrosstalkExchange): the
 *   exchanges that ended, oldest first
 *
 * Since: 0.30.0
 */
GPtrArray *clawt_crosstalk_collect(ClawtCrosstalk        *self,
                                   gint64                 now,
                                   ClawtCrosstalkBusyFunc busy,
                                   gpointer               user_data);

/**
 * clawt_crosstalk_get_open_count:
 * @self: a #ClawtCrosstalk
 *
 * Returns: how many exchanges are open
 *
 * Since: 0.30.0
 */
guint clawt_crosstalk_get_open_count(ClawtCrosstalk *self);

/* ── The note ────────────────────────────────────────────────────── */

/**
 * clawt_crosstalk_compose:
 * @exchanges: (element-type ClawtCrosstalkExchange): ended exchanges,
 *   all about @for_agent
 * @for_agent: the agent whose conversation with the operator it goes
 *   into; its own line comes first in each section
 *
 * The summary note, built from the record and nothing else: who reached
 * out and what they asked, what each agent in it did, and how it
 * ended.  One exchange is one short block; several are numbered
 * sections under one heading, in the order their "View in Flow" actions
 * are numbered.  Everything quoted from a message is escaped with
 * clawt_preamble_escape_all(), because the note is the daemon's voice.
 *
 * Returns: (transfer full): the note, in markdown
 *
 * Since: 0.30.0
 */
gchar *clawt_crosstalk_compose(GPtrArray   *exchanges,
                               const gchar *for_agent);

/**
 * clawt_crosstalk_join_targets:
 * @exchanges: (element-type ClawtCrosstalkExchange): the exchanges a
 *   note summarises
 * @out_rooms: (out) (transfer full): their rooms, space-separated
 * @out_messages: (out) (transfer full): their first messages,
 *   space-separated and paired with @out_rooms
 *
 * The note's jump targets, for clawt_message_set_crosstalk().
 *
 * Since: 0.30.0
 */
void clawt_crosstalk_join_targets(GPtrArray  *exchanges,
                                  gchar     **out_rooms,
                                  gchar     **out_messages);

/* ── The optional rewrite ────────────────────────────────────────── */

/**
 * clawt_crosstalk_rewrite_system_prompt:
 *
 * Returns: (transfer none): the instruction the rewriting model gets
 *
 * Since: 0.30.0
 */
const gchar *clawt_crosstalk_rewrite_system_prompt(void);

/**
 * clawt_crosstalk_rewrite_prompt:
 * @summary: the note built from the record
 *
 * The rewriting model's user message: @summary fenced as data, with a
 * fence longer than any run of backticks in it.
 *
 * Returns: (transfer full): the prompt
 *
 * Since: 0.30.0
 */
gchar *clawt_crosstalk_rewrite_prompt(const gchar *summary);

/**
 * clawt_crosstalk_rewrite_accept:
 * @reply: (nullable): what the model wrote
 *
 * Whether a rewrite is usable, and the text to post when it is: trimmed,
 * escaped with clawt_preamble_escape_all(), non-empty and at most
 * %CLAWT_CROSSTALK_MODEL_MAX_CHARS characters.  Anything else is
 * garbage, and the summary built from the record is posted instead.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL
 *
 * Since: 0.30.0
 */
gchar *clawt_crosstalk_rewrite_accept(const gchar *reply);

/**
 * clawt_crosstalk_rewrite_async: (skip)
 * @provider: the model
 * @summary: the note built from the record
 * @timeout_ms: the hard limit, or 0 for %CLAWT_CROSSTALK_MODEL_TIMEOUT_MS
 * @callback: (scope async): called once, with the outcome
 * @user_data: data for @callback
 *
 * Asks @provider for the friendlier paragraph without blocking.  The
 * timer and the callback are attached to the thread-default context at
 * the time of this call.  At @timeout_ms the request is cancelled and
 * the result is a %CLAWT_ERROR_TIMEOUT error; a reply arriving after
 * that is discarded.
 *
 * Skipped for introspection: ai-glib ships no GIR.
 *
 * Since: 0.30.0
 */
void clawt_crosstalk_rewrite_async(AiProvider          *provider,
                                   const gchar         *summary,
                                   guint                timeout_ms,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);

/**
 * clawt_crosstalk_rewrite_finish: (skip)
 * @result: the #GAsyncResult
 * @error: (out) (optional): why there is no rewrite
 *
 * Returns: (transfer full) (nullable): the accepted rewrite
 *   (clawt_crosstalk_rewrite_accept()), or %NULL with @error set on a
 *   failure, a timeout or garbage
 *
 * Since: 0.30.0
 */
gchar *clawt_crosstalk_rewrite_finish(GAsyncResult  *result,
                                      GError       **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtCrosstalkExchange, clawt_crosstalk_exchange_unref)

G_END_DECLS

#endif /* CLAWT_CROSSTALK_H */
