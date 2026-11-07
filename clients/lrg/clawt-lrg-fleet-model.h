/*
 * clawt-lrg-fleet-model.h - What the fleet pages decide, without libregnum
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The decisions behind four pages of clawtilla-lrg: Flow and Activity
 * (the Work section), Skills and Memory (the Library section).  Which
 * rooms a conversation list shows and in what order, what the
 * conversation graph's nodes and wires are, what a running turn's card
 * says, which notes a skill carries and which buttons a recording
 * offers, what a recall hit reads as -- all of it here, tested by
 * tests/test-lrg-models-fleet.c with no libregnum and no display.  The
 * page files only draw what these return.
 */

#pragma once

#include "clawt-lrg-model.h"

G_BEGIN_DECLS

/* ── Flow: the conversation list ──────────────────────────────────── */

/**
 * ClawtLrgFlowRoom:
 * @id: the room id
 * @label: who is in it, "alpha and beta"
 * @subtitle: "4m ago · alpha: the first line of the last message"
 * @count: the message count, as the row's suffix shows it
 * @last_ts: when its last message was written, unix seconds
 * @members: (array zero-terminated=1): the member ids
 *
 * One row of the Flow page's conversation list.
 */
typedef struct {
    gchar  *id;
    gchar  *label;
    gchar  *subtitle;
    gchar  *count;
    gint64  last_ts;
    GStrv   members;
} ClawtLrgFlowRoom;

/**
 * clawt_lrg_flow_room_free:
 * @room: (nullable): a #ClawtLrgFlowRoom
 */
void clawt_lrg_flow_room_free(ClawtLrgFlowRoom *room);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtLrgFlowRoom, clawt_lrg_flow_room_free)

/**
 * clawt_lrg_flow_room_label:
 * @members: (nullable): a room's member ids
 *
 * "alpha and beta", "alpha, beta and you".  Built from the members
 * rather than by taking the id apart, because how a direct room is
 * named is the daemon's business.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_lrg_flow_room_label(JsonArray *members);

/**
 * clawt_lrg_flow_room_involves_user:
 * @members: (nullable): a room's member ids
 *
 * Returns: whether the operator ("user") is one of them
 */
gboolean clawt_lrg_flow_room_involves_user(JsonArray *members);

/**
 * clawt_lrg_flow_one_line:
 * @body: (nullable): a message body
 * @limit: how many characters to keep
 *
 * The first line of @body, trimmed, cut on a character boundary with an
 * ellipsis when longer than @limit.
 *
 * Returns: (transfer full): the line; "" for %NULL or invalid UTF-8
 */
gchar *clawt_lrg_flow_one_line(const gchar *body, glong limit);

/**
 * clawt_lrg_flow_rooms:
 * @reply: (nullable): a `room.list` reply
 * @include_user: whether rooms the operator is in are listed
 * @now_us: now, wall-clock microseconds, for the relative times
 *
 * The conversation list: every room anybody has said something in,
 * most recent first, without the operator's own rooms unless asked.
 *
 * Returns: (transfer full) (element-type ClawtLrgFlowRoom): the rows
 */
GPtrArray *clawt_lrg_flow_rooms(JsonNode *reply, gboolean include_user,
                                gint64 now_us);

/**
 * clawt_lrg_flow_room_members:
 * @room_list: (nullable): a `room.list` reply
 * @room_id: (nullable): a room id
 *
 * Returns: (transfer full) (nullable): the room's member ids, or %NULL
 *   when the listing does not have the room
 */
GStrv clawt_lrg_flow_room_members(JsonNode *room_list, const gchar *room_id);

/**
 * clawt_lrg_flow_count_label:
 * @n: how many messages
 *
 * Returns: (transfer full): "1 message", "12 messages"
 */
gchar *clawt_lrg_flow_count_label(guint n);

/**
 * clawt_lrg_flow_task_chip:
 * @task: (nullable): a task id
 *
 * Returns: (transfer full) (nullable): "task 1a2b3c4d", or %NULL
 */
gchar *clawt_lrg_flow_task_chip(const gchar *task);

/**
 * clawt_lrg_flow_hop_chip:
 * @depth: a message's hop depth
 *
 * From the second hop on: every ordinary message is one hop, and a
 * "hop 1" on all of them would make the number stop being read.
 *
 * Returns: (transfer full) (nullable): "hop 3", or %NULL below 2
 */
gchar *clawt_lrg_flow_hop_chip(gint64 depth);

/* ── Flow: the conversation graph ─────────────────────────────────── */

typedef struct _ClawtLrgFlowGraph ClawtLrgFlowGraph;

/**
 * ClawtLrgFlowNode:
 * @id: the agent id, or "user" for the operator
 * @name: what the node is titled
 * @color: (nullable): the agent's colour as the daemon spells it
 * @x: left edge, graph units
 * @y: top edge, graph units
 * @sent: messages this node sent that the graph has seen
 * @received: messages this node was a recipient of
 * @is_user: whether this is the operator
 *
 * One node of the graph, laid out.
 */
typedef struct {
    gchar    *id;
    gchar    *name;
    gchar    *color;
    gfloat    x;
    gfloat    y;
    guint     sent;
    guint     received;
    gboolean  is_user;
} ClawtLrgFlowNode;

/**
 * ClawtLrgFlowEdge:
 * @from: the sender's id
 * @to: the recipient's id
 * @count: how many messages went this way
 * @last_us: when the latest of them was written, wall-clock microseconds
 *
 * One direction of traffic between two participants.  Each direction
 * is its own edge, because "alpha asks beta forty times and beta never
 * answers" is exactly the shape the page exists to show.
 */
typedef struct {
    gchar  *from;
    gchar  *to;
    guint   count;
    gint64  last_us;
} ClawtLrgFlowEdge;

/* How long a wire stays lit after a message, and how dim it settles. */
#define CLAWT_LRG_FLOW_HEAT_SECONDS (600)
#define CLAWT_LRG_FLOW_HEAT_FLOOR   (0.25)

ClawtLrgFlowGraph *clawt_lrg_flow_graph_new(void);
void               clawt_lrg_flow_graph_free(ClawtLrgFlowGraph *graph);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtLrgFlowGraph, clawt_lrg_flow_graph_free)

/**
 * clawt_lrg_flow_graph_set_agents:
 * @graph: the graph
 * @agent_list: (nullable): an `agent.list` reply
 *
 * Takes the agents' names, colours and order.  Agents with no traffic
 * are not drawn; the order decides where the ones that are sit.
 */
void clawt_lrg_flow_graph_set_agents(ClawtLrgFlowGraph *graph,
                                     JsonNode          *agent_list);

/**
 * clawt_lrg_flow_graph_fold:
 * @graph: the graph
 * @message_id: (nullable): the message's id
 * @sender: the sender's id
 * @members: (array zero-terminated=1): the room's members
 * @ts_us: when it was written, wall-clock microseconds
 *
 * Counts one message: an edge from @sender to every other member.
 * A message already counted (same id) is not counted again, so a
 * history read twice and a live event for a message the history also
 * returned add up to one.  A message with no id is always counted.
 *
 * Returns: whether anything was added
 */
gboolean clawt_lrg_flow_graph_fold(ClawtLrgFlowGraph   *graph,
                                   const gchar         *message_id,
                                   const gchar         *sender,
                                   const gchar *const  *members,
                                   gint64               ts_us);

/**
 * clawt_lrg_flow_graph_fold_history:
 * @graph: the graph
 * @members: (array zero-terminated=1): the room's members
 * @reply: (nullable): a `room.history` reply for that room
 *
 * Returns: how many messages were new to the graph
 */
guint clawt_lrg_flow_graph_fold_history(ClawtLrgFlowGraph  *graph,
                                        const gchar *const *members,
                                        JsonNode           *reply);

/**
 * clawt_lrg_flow_graph_nodes:
 * @graph: the graph
 * @include_user: whether the operator is drawn
 *
 * Every participant with traffic, laid out: agents round a circle in
 * fleet order, the operator (when drawn) in the middle, because every
 * one of their conversations is with somebody on the ring.
 *
 * Returns: (transfer full) (element-type ClawtLrgFlowNode): the nodes
 */
GPtrArray *clawt_lrg_flow_graph_nodes(ClawtLrgFlowGraph *graph,
                                      gboolean           include_user);

/**
 * clawt_lrg_flow_graph_edges:
 * @graph: the graph
 * @include_user: whether edges to and from the operator are included
 *
 * Returns: (transfer full) (element-type ClawtLrgFlowEdge): the edges,
 *   ordered by sender then recipient so a redraw is stable
 */
GPtrArray *clawt_lrg_flow_graph_edges(ClawtLrgFlowGraph *graph,
                                      gboolean           include_user);

/**
 * clawt_lrg_flow_graph_edge:
 * @graph: the graph
 * @from: a sender
 * @to: a recipient
 *
 * Returns: (transfer none) (nullable): that edge
 */
const ClawtLrgFlowEdge *clawt_lrg_flow_graph_edge(ClawtLrgFlowGraph *graph,
                                                  const gchar       *from,
                                                  const gchar       *to);

/**
 * clawt_lrg_flow_graph_signature:
 * @graph: the graph
 * @include_user: as for clawt_lrg_flow_graph_nodes()
 *
 * One string naming every node and every edge with its count, so a view
 * can rebuild only when the picture changed -- a rebuild throws away
 * whatever the person had dragged.
 *
 * Returns: (transfer full): the signature
 */
gchar *clawt_lrg_flow_graph_signature(ClawtLrgFlowGraph *graph,
                                      gboolean           include_user);

/**
 * clawt_lrg_flow_graph_name:
 * @graph: the graph
 * @id: a participant id
 *
 * Returns: (transfer none): its display name; "You" for the operator,
 *   the id for an agent the listing does not have
 */
const gchar *clawt_lrg_flow_graph_name(ClawtLrgFlowGraph *graph,
                                       const gchar       *id);

/**
 * clawt_lrg_flow_edge_heat:
 * @edge: an edge
 * @now_us: now, wall-clock microseconds
 *
 * How lit the wire is: 1.0 for traffic within the last minute, falling
 * to %CLAWT_LRG_FLOW_HEAT_FLOOR at %CLAWT_LRG_FLOW_HEAT_SECONDS and
 * staying there, so an old conversation is still a visible wire.
 *
 * Returns: the heat, between the floor and 1.0
 */
gdouble clawt_lrg_flow_edge_heat(const ClawtLrgFlowEdge *edge,
                                 gint64                  now_us);

/**
 * clawt_lrg_flow_port_label:
 * @graph: the graph
 * @edge: an edge
 * @output: %TRUE for the sender's end, %FALSE for the recipient's
 *
 * The sender's end reads "to beta · 12", the recipient's "from alpha":
 * the count once per wire, at the end that did the talking.
 *
 * Returns: (transfer full): the port's name
 */
gchar *clawt_lrg_flow_port_label(ClawtLrgFlowGraph      *graph,
                                 const ClawtLrgFlowEdge *edge,
                                 gboolean                output);

/**
 * clawt_lrg_flow_room_between:
 * @room_list: (nullable): a `room.list` reply
 * @a: one participant
 * @b: another
 *
 * The conversation a click on the wire between @a and @b opens: a room
 * of exactly those two if there is one, else the most recently active
 * room holding both.
 *
 * Returns: (transfer full) (nullable): the room id
 */
gchar *clawt_lrg_flow_room_between(JsonNode    *room_list,
                                   const gchar *a,
                                   const gchar *b);

/* ── Activity ─────────────────────────────────────────────────────── */

/**
 * ClawtLrgTurn:
 * @key: agent and room, the identity a card keeps across refreshes
 * @agent_id: the agent
 * @title: "alpha in planning"
 * @origin: (nullable): whom the turn is for, when not the operator
 * @task: (nullable): the task it is working on
 * @started_us: when it began, wall-clock microseconds (daemon clock)
 * @running: how many turns the agent has running in all
 * @step_count: steps taken so far
 * @steerable: whether msg.send reaches this turn
 * @steps: (element-type ClawtTurnStep): its most recent steps
 *
 * One running turn, from `activity.list`.
 */
typedef struct {
    gchar     *key;
    gchar     *agent_id;
    gchar     *title;
    gchar     *origin;
    gchar     *task;
    gint64     started_us;
    guint      running;
    guint      step_count;
    gboolean   steerable;
    GPtrArray *steps;
} ClawtLrgTurn;

void clawt_lrg_turn_free(ClawtLrgTurn *turn);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtLrgTurn, clawt_lrg_turn_free)

/**
 * clawt_lrg_activity_turns:
 * @reply: (nullable): an `activity.list` reply
 * @now_us: (out) (optional): the daemon's clock when it answered, or this
 *   machine's when the reply did not say
 *
 * Returns: (transfer full) (element-type ClawtLrgTurn): the turns
 */
GPtrArray *clawt_lrg_activity_turns(JsonNode *reply, gint64 *now_us);

/**
 * clawt_lrg_activity_subtitle:
 * @turn: a turn
 * @now_us: now, on the daemon's clock
 *
 * "running 3m 12s · for alpha · task 1a2b · 4 step(s)"
 *
 * Returns: (transfer full): the line under the card's title
 */
gchar *clawt_lrg_activity_subtitle(const ClawtLrgTurn *turn, gint64 now_us);

/**
 * clawt_lrg_activity_order:
 * @turns: (element-type ClawtLrgTurn): the turns
 *
 * Returns: (transfer full): their keys in order, one string, so a view
 *   can tell a reorder from an update
 */
gchar *clawt_lrg_activity_order(GPtrArray *turns);

/**
 * ClawtLrgStepClip:
 * @start: seconds into the turn
 * @duration: how long until the next step (or now, for the last)
 * @label: the step's summary
 * @tone: the step's tone word
 *
 * One step placed on a timeline.
 */
typedef struct {
    gdouble      start;
    gdouble      duration;
    gchar       *label;
    const gchar *tone;
} ClawtLrgStepClip;

void clawt_lrg_step_clip_free(ClawtLrgStepClip *clip);

/**
 * clawt_lrg_activity_clips:
 * @turn: a turn
 * @now_us: now, on the daemon's clock
 *
 * The turn's steps as clips: each runs until the next begins, the last
 * until now.  Never shorter than a fifth of a second, so a burst of
 * steps stays readable, and never before the turn started.
 *
 * Returns: (transfer full) (element-type ClawtLrgStepClip): the clips
 */
GPtrArray *clawt_lrg_activity_clips(const ClawtLrgTurn *turn, gint64 now_us);

/**
 * clawt_lrg_activity_steer_body:
 * @typed: (nullable): what was typed
 *
 * Returns: (transfer full) (nullable): the text to send, stripped, or
 *   %NULL when there is nothing to send
 */
gchar *clawt_lrg_activity_steer_body(const gchar *typed);

/**
 * clawt_lrg_activity_steer_result:
 * @reply: (nullable): a `msg.send` reply
 *
 * A busy agent takes the text as a steer; one that finished meanwhile
 * takes it as an ordinary message, read at a different moment.
 *
 * Returns: (transfer none): what to tell the person
 */
const gchar *clawt_lrg_activity_steer_result(JsonNode *reply);

/**
 * clawt_lrg_activity_stop_result:
 * @agent_id: the agent
 * @reply: (nullable): an `agent.interrupt` reply
 *
 * Returns: (transfer full): what to tell the person
 */
gchar *clawt_lrg_activity_stop_result(const gchar *agent_id,
                                      JsonNode    *reply);

/* Said where a turn cannot be steered: msg.send reaches only the
 * agent's conversation with the operator. */
#define CLAWT_LRG_STEER_REFUSAL_TITLE \
    "Steering reaches only its conversation with you"
#define CLAWT_LRG_STEER_REFUSAL_BODY \
    "This turn is in another room. Message it there, or stop it."

/* ── Skills ───────────────────────────────────────────────────────── */

/**
 * ClawtLrgNote:
 * @title: the note's heading
 * @body: what it says
 * @tone: a tone word for its badge
 *
 * One labelled line on a card.
 */
typedef struct {
    gchar       *title;
    gchar       *body;
    const gchar *tone;
} ClawtLrgNote;

void clawt_lrg_note_free(ClawtLrgNote *note);

/**
 * clawt_lrg_skill_notes:
 * @skill: one entry of `skill.list`
 *
 * The lines a skill's card carries, in the order GTK draws them: the
 * "Not enabled" caution and every warning first -- above the buttons,
 * because a warning below the Enable button gets enabled around -- then
 * what was not copied, the provenance (with the digest) and the
 * directory.
 *
 * Returns: (transfer full) (element-type ClawtLrgNote): the notes
 */
GPtrArray *clawt_lrg_skill_notes(JsonObject *skill);

/**
 * clawt_lrg_skill_caption:
 * @skill: one entry of `skill.list`
 *
 * Returns: (transfer full): the tile's one-line state, "Enabled",
 *   "Disabled · 2 warnings"
 */
gchar *clawt_lrg_skill_caption(JsonObject *skill);

/**
 * clawt_lrg_skill_tone:
 * @skill: one entry of `skill.list`
 *
 * Returns: (transfer none): "warn" with warnings, "good" enabled,
 *   "neutral" otherwise
 */
const gchar *clawt_lrg_skill_tone(JsonObject *skill);

/**
 * clawt_lrg_skills_empty:
 * @payload: (nullable): the `skill.list` reply object
 * @title: (out): the heading
 * @body: (out): the reason
 *
 * Two causes, with different remedies: skills switched off, or none in
 * the directory.
 */
void clawt_lrg_skills_empty(JsonObject   *payload,
                            const gchar **title,
                            const gchar **body);

JsonNode *clawt_lrg_skill_enable_payload(const gchar *name, gboolean enabled);
JsonNode *clawt_lrg_skill_name_payload(const gchar *name);

/**
 * clawt_lrg_skill_import_payload:
 * @source: (nullable): the directory typed
 * @origin: (nullable): where it came from
 * @refusal: (out) (optional) (transfer full): why not, when %NULL
 *
 * Returns: (transfer full) (nullable): the `skill.import` payload
 */
JsonNode *clawt_lrg_skill_import_payload(const gchar  *source,
                                         const gchar  *origin,
                                         gchar       **refusal);

/**
 * clawt_lrg_skill_create_payload:
 * @name: (nullable): the name typed
 * @description: (nullable): the description typed
 * @refusal: (out) (optional) (transfer full): why not, when %NULL
 *
 * Only emptiness is refused here; the name's spelling is the daemon's
 * rule and its refusal is shown as it says it.
 *
 * Returns: (transfer full) (nullable): the `skill.create` payload
 */
JsonNode *clawt_lrg_skill_create_payload(const gchar  *name,
                                         const gchar  *description,
                                         gchar       **refusal);

/**
 * clawt_lrg_skill_remove_prompt:
 * @name: the skill
 *
 * Returns: (transfer full): the confirmation's body
 */
gchar *clawt_lrg_skill_remove_prompt(const gchar *name);

/* ── Teaching a task ─────────────────────────────────────────────── */

/**
 * ClawtLrgTeachAction:
 * @CLAWT_LRG_TEACH_STOP: end a recording in progress
 * @CLAWT_LRG_TEACH_SHOW: read what was recorded
 * @CLAWT_LRG_TEACH_SYNTHESIZE: have a model draft a skill from it
 * @CLAWT_LRG_TEACH_COMMIT: write the draft as a disabled skill
 * @CLAWT_LRG_TEACH_REMOVE: delete the recording
 *
 * What a recording's buttons do.  The page turns each into its frame
 * kind with a literal, so `make parity` sees every one.
 */
typedef enum {
    CLAWT_LRG_TEACH_STOP,
    CLAWT_LRG_TEACH_SHOW,
    CLAWT_LRG_TEACH_SYNTHESIZE,
    CLAWT_LRG_TEACH_COMMIT,
    CLAWT_LRG_TEACH_REMOVE
} ClawtLrgTeachAction;

/**
 * ClawtLrgTeachButton:
 * @action: what it does
 * @label: its label
 * @style: (nullable): "primary", "danger" or %NULL
 */
typedef struct {
    ClawtLrgTeachAction  action;
    const gchar         *label;
    const gchar         *style;
} ClawtLrgTeachButton;

/**
 * clawt_lrg_teach_buttons:
 * @active: whether the recording is still running
 * @n: (out): how many
 *
 * A running recording can only be stopped; a finished one can be read,
 * drafted, committed or removed.
 *
 * Returns: (transfer none) (array length=n): the buttons
 */
const ClawtLrgTeachButton *clawt_lrg_teach_buttons(gboolean active, guint *n);

/**
 * clawt_lrg_teach_done:
 * @action: what was done
 *
 * Returns: (transfer none) (nullable): the toast after it worked, or
 *   %NULL when the redrawn list says enough
 */
const gchar *clawt_lrg_teach_done(ClawtLrgTeachAction action);

/**
 * ClawtLrgRecording:
 * @id: the recording id
 * @title: its goal, or its id when it has none
 * @subtitle: "agent, agent alpha, 12 step(s) -- recording now"
 * @active: whether it is still recording
 * @caveats: (array zero-terminated=1): what to know before sharing it
 */
typedef struct {
    gchar    *id;
    gchar    *title;
    gchar    *subtitle;
    gboolean  active;
    GStrv     caveats;
} ClawtLrgRecording;

void clawt_lrg_recording_free(ClawtLrgRecording *recording);

/**
 * clawt_lrg_teach_recordings:
 * @reply: (nullable): a `teach.list` reply
 *
 * Returns: (transfer full) (element-type ClawtLrgRecording): the rows
 */
GPtrArray *clawt_lrg_teach_recordings(JsonNode *reply);

/**
 * clawt_lrg_teach_steps_text:
 * @reply: (nullable): a `teach.show` reply
 *
 * Returns: (transfer full): the steps, numbered, one per line with its
 *   detail indented under it; "Nothing was captured." for none
 */
gchar *clawt_lrg_teach_steps_text(JsonNode *reply);

/**
 * clawt_lrg_teach_draft_text:
 * @reply: (nullable): a `teach.synthesize` reply
 *
 * Returns: (transfer full) (nullable): the draft's name, description,
 *   preview and note as text to read before committing, or %NULL when
 *   the reply carried no draft
 */
gchar *clawt_lrg_teach_draft_text(JsonNode *reply);

/**
 * clawt_lrg_teach_start_payload:
 * @agent: (nullable): the agent chosen
 * @source_nick: (nullable): the #ClawtTeachSource nick chosen
 * @goal: (nullable): what is being taught
 * @refusal: (out) (optional) (transfer full): why not, when %NULL
 *
 * Returns: (transfer full) (nullable): the `teach.start` payload
 */
JsonNode *clawt_lrg_teach_start_payload(const gchar  *agent,
                                        const gchar  *source_nick,
                                        const gchar  *goal,
                                        gchar       **refusal);

/**
 * clawt_lrg_teach_sources:
 * @labels: (out) (transfer full): the labels, parallel to the result
 *
 * The recording kinds, walked from clawt_teach_source_*() rather than
 * listed here: a client with a list of its own can disagree with the
 * daemon about what exists.
 *
 * Returns: (transfer full): the nicks, %NULL-terminated
 */
GStrv clawt_lrg_teach_sources(GStrv *labels);

/* ── Memory ───────────────────────────────────────────────────────── */

/**
 * clawt_lrg_recall_payload:
 * @query: (nullable): what was typed
 *
 * Returns: (transfer full) (nullable): the `memory.recall` payload, or
 *   %NULL for an empty query (the daemon refuses one)
 */
JsonNode *clawt_lrg_recall_payload(const gchar *query);

/**
 * ClawtLrgRecallHit:
 * @body: what was said
 * @subtitle: "alpha in planning"
 * @stamp: "2026-11-07 14:02": a recall hit is a record matched against
 *   other records, so the clock and the day, not "2h ago"
 */
typedef struct {
    gchar *body;
    gchar *subtitle;
    gchar *stamp;
} ClawtLrgRecallHit;

void clawt_lrg_recall_hit_free(ClawtLrgRecallHit *hit);

/**
 * clawt_lrg_recall_hits:
 * @reply: (nullable): a `memory.recall` reply
 *
 * Returns: (transfer full) (element-type ClawtLrgRecallHit): the hits
 */
GPtrArray *clawt_lrg_recall_hits(JsonNode *reply);

/**
 * clawt_lrg_recall_empty:
 * @reply: (nullable): a `memory.recall` reply with no hits
 * @title: (out): the heading
 * @body: (out): the reason
 *
 * "Nothing matched" and "nothing has been said yet" need different
 * next steps, and only the index size tells them apart.
 */
void clawt_lrg_recall_empty(JsonNode     *reply,
                            const gchar **title,
                            const gchar **body);

/**
 * clawt_lrg_recall_is_substring:
 * @reply: (nullable): a `memory.recall` reply
 *
 * Returns: whether the daemon's sqlite has no FTS5, so the search was a
 *   substring scan rather than a ranked one
 */
gboolean clawt_lrg_recall_is_substring(JsonNode *reply);

/**
 * clawt_lrg_operator_learned:
 * @reply: (nullable): an `operator.get` reply
 *
 * What the fleet recorded about the operator, each with the agent and
 * the day behind it: an inference drawn six months ago and one drawn
 * this morning are not the same claim about a person.
 *
 * Returns: (transfer full) (element-type ClawtLrgNote): title is the
 *   content, body is "source, day"
 */
GPtrArray *clawt_lrg_operator_learned(JsonNode *reply);

/* Said when memories.operator_profile is off. */
#define CLAWT_LRG_OPERATOR_OFF \
    "memories.operator_profile is off, so no agent is told any of this."

/**
 * clawt_lrg_agent_memory_payload:
 * @agent: the agent
 * @query: (nullable): what was typed
 * @search: (out): %TRUE for `memory.search`, %FALSE for `memory.list`
 *
 * Returns: (transfer full): the payload
 */
JsonNode *clawt_lrg_agent_memory_payload(const gchar *agent,
                                         const gchar *query,
                                         gboolean    *search);

/**
 * ClawtLrgMemoryRow:
 * @title: the summary, or the content's first line
 * @body: the whole content
 * @category: (nullable): its category
 * @scope: (nullable): its scope nick
 * @scope_label: (nullable): its scope, as a person reads it
 * @pinned: whether it is pinned
 * @when: "3d ago"
 */
typedef struct {
    gchar    *title;
    gchar    *body;
    gchar    *category;
    gchar    *scope;
    gchar    *scope_label;
    gboolean  pinned;
    gchar    *when;
} ClawtLrgMemoryRow;

void clawt_lrg_memory_row_free(ClawtLrgMemoryRow *row);

/**
 * clawt_lrg_agent_memories:
 * @reply: (nullable): a `memory.list` or `memory.search` reply
 * @scope: (nullable): a scope nick to keep, or %NULL for every scope
 * @now_us: now, wall-clock microseconds
 *
 * Returns: (transfer full) (element-type ClawtLrgMemoryRow): the rows
 */
GPtrArray *clawt_lrg_agent_memories(JsonNode    *reply,
                                    const gchar *scope,
                                    gint64       now_us);

G_END_DECLS
