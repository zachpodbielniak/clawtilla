/*
 * clawt-office-presence.h - What each participant in the office is doing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The office view draws the fleet as people in a building: at a desk
 * when a turn is running, on the phone while two of them message, round
 * a table when a group round is under way, in the lounge when idle.  It
 * must never invent any of that, so this is the one fold from things the
 * daemon actually saw -- turn edges, tool steps, messages, decisions,
 * incidents -- to an activity per participant.  The daemon runs it and
 * publishes the result; every client and renderer draws that result, so
 * the GTK roster, the web page, the libregnum office and the CLI agree.
 *
 * Pure and testable: the daemon translates events into the note_*()
 * calls below, and resolution is a function of what was noted and the
 * time.  Keyed per participant *and* per room, because an agent can run
 * turns in several rooms at once and a shared scalar is the bug this
 * shape has had before.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * ClawtOfficeActivity:
 * @CLAWT_OFFICE_OFFLINE: not running
 * @CLAWT_OFFICE_IDLE: running, nothing to do
 * @CLAWT_OFFICE_WORKING: a turn is running and using tools
 * @CLAWT_OFFICE_THINKING: a turn is running with no tool in hand
 * @CLAWT_OFFICE_ON_CALL: exchanging messages with one other participant
 * @CLAWT_OFFICE_IN_MEETING: in a group round with several others
 * @CLAWT_OFFICE_DELIVERING: just handed work to somebody
 * @CLAWT_OFFICE_WAITING_ON_YOU: asked you something, not yet answered
 * @CLAWT_OFFICE_ERROR: its last turn timed out, looped or broke
 * @CLAWT_OFFICE_ARRIVING: an ephemeral helper that has just been made
 * @CLAWT_OFFICE_LEAVING: an ephemeral helper being reaped
 *
 * What a participant is doing, as the office draws it.
 */
typedef enum {
    CLAWT_OFFICE_OFFLINE,
    CLAWT_OFFICE_IDLE,
    CLAWT_OFFICE_WORKING,
    CLAWT_OFFICE_THINKING,
    CLAWT_OFFICE_ON_CALL,
    CLAWT_OFFICE_IN_MEETING,
    CLAWT_OFFICE_DELIVERING,
    CLAWT_OFFICE_WAITING_ON_YOU,
    CLAWT_OFFICE_ERROR,
    CLAWT_OFFICE_ARRIVING,
    CLAWT_OFFICE_LEAVING
} ClawtOfficeActivity;

/**
 * ClawtOfficeStation:
 * @CLAWT_OFFICE_STATION_NONE: no tool in hand
 * @CLAWT_OFFICE_STATION_DESK: editing or writing
 * @CLAWT_OFFICE_STATION_LIBRARY: reading, searching, researching
 * @CLAWT_OFFICE_STATION_LAB: running commands and tests
 * @CLAWT_OFFICE_STATION_WHITEBOARD: planning, tasks, delegation
 * @CLAWT_OFFICE_STATION_TERMINAL: a browser, a screen or a computer
 *
 * Where in the office the tool a turn is using belongs.  A few stations
 * read at a glance; one per tool would read as noise.
 */
typedef enum {
    CLAWT_OFFICE_STATION_NONE,
    CLAWT_OFFICE_STATION_DESK,
    CLAWT_OFFICE_STATION_LIBRARY,
    CLAWT_OFFICE_STATION_LAB,
    CLAWT_OFFICE_STATION_WHITEBOARD,
    CLAWT_OFFICE_STATION_TERMINAL
} ClawtOfficeStation;

/**
 * ClawtOfficeParticipantKind:
 * @CLAWT_OFFICE_KIND_AGENT: an agent in this fleet
 * @CLAWT_OFFICE_KIND_EPHEMERAL: an ephemeral helper
 * @CLAWT_OFFICE_KIND_GROK_BOT: a Grok bot, drawn as a remote worker
 * @CLAWT_OFFICE_KIND_EXTERNAL: anything else: another agent system, or
 *   a participant from another fleet
 *
 * Who a participant is.  A new kind of agent system is a new kind here
 * and a fold in the daemon, not a new path in every renderer.
 */
typedef enum {
    CLAWT_OFFICE_KIND_AGENT,
    CLAWT_OFFICE_KIND_EPHEMERAL,
    CLAWT_OFFICE_KIND_GROK_BOT,
    CLAWT_OFFICE_KIND_EXTERNAL
} ClawtOfficeParticipantKind;

/* How long an edge keeps its meaning without being renewed, in seconds. */
#define CLAWT_OFFICE_CALL_SECONDS      (60)
#define CLAWT_OFFICE_MEETING_SECONDS   (180)
#define CLAWT_OFFICE_DELIVERY_SECONDS  (20)
#define CLAWT_OFFICE_ARRIVAL_SECONDS   (15)
#define CLAWT_OFFICE_STEP_SECONDS      (45)

#define CLAWT_TYPE_OFFICE_PRESENCE (clawt_office_presence_get_type())
G_DECLARE_FINAL_TYPE(ClawtOfficePresence, clawt_office_presence,
                     CLAWT, OFFICE_PRESENCE, GObject)

/**
 * clawt_office_presence_new:
 * @fleet_id: (nullable): this fleet's id, carried on every participant
 *   so a future merged office can tell fleets apart
 * @stale_seconds: how long a turn edge that is never closed is believed
 *
 * Returns: (transfer full): a new, empty fold
 */
ClawtOfficePresence *clawt_office_presence_new(const gchar *fleet_id,
                                               gint64       stale_seconds);

/**
 * clawt_office_presence_set_participant:
 * @self: the fold
 * @kind: who it is
 * @id: its id, unique within this fleet
 * @name: (nullable): how to show it
 * @team: (nullable): its team's id
 * @parent: (nullable): for a helper, who spawned it
 * @running: whether its process is up
 * @now: the current Unix time, seconds
 *
 * Adds a participant or updates who it is.  A participant that was not
 * running and now is, or a helper just added, arrives.
 *
 * Returns: %TRUE when what is drawn for it changed
 */
gboolean clawt_office_presence_set_participant(ClawtOfficePresence        *self,
                                               ClawtOfficeParticipantKind  kind,
                                               const gchar                *id,
                                               const gchar                *name,
                                               const gchar                *team,
                                               const gchar                *parent,
                                               gboolean                    running,
                                               gint64                      now);

/**
 * clawt_office_presence_set_role:
 * @self: the fold
 * @id: a participant
 * @role: (nullable): `chief`, `lead` or %NULL -- which office is theirs
 */
void clawt_office_presence_set_role(ClawtOfficePresence *self, const gchar *id,
                                    const gchar *role);

/**
 * clawt_office_presence_set_color:
 * @self: the fold
 * @id: a participant
 * @color: (nullable): its colour, `#rrggbb`, as its fleet draws it
 *
 * Carried in the snapshot, so a fleet drawing this one's people as
 * visitors draws them in their own colours, which it cannot look up.
 */
void clawt_office_presence_set_color(ClawtOfficePresence *self, const gchar *id,
                                     const gchar *color);

/**
 * clawt_office_presence_remove:
 * @self: the fold
 * @id: a participant
 *
 * Returns: %TRUE when it was there
 */
gboolean clawt_office_presence_remove(ClawtOfficePresence *self, const gchar *id);

/**
 * clawt_office_presence_note_turn:
 * @self: the fold
 * @id: the participant
 * @room: the room the turn is in
 * @running: the rising (%TRUE) or falling edge
 * @now: the current Unix time, seconds
 *
 * A turn edge -- never a repeated typing frame: the daemon passes the
 * edges clawt_agent_note_typing() reports.
 */
void clawt_office_presence_note_turn(ClawtOfficePresence *self, const gchar *id,
                                     const gchar *room, gboolean running, gint64 now);

/**
 * clawt_office_presence_note_step:
 * @self: the fold
 * @id: the participant
 * @room: (nullable): the room the step's turn is in
 * @tool: (nullable): the tool it used, or %NULL for a text or thinking step
 * @now: the current Unix time, seconds
 */
void clawt_office_presence_note_step(ClawtOfficePresence *self, const gchar *id,
                                     const gchar *room, const gchar *tool, gint64 now);

/**
 * clawt_office_presence_note_call:
 * @self: the fold
 * @from: who sent a message
 * @to: the participant it was for, in a room of two
 * @now: the current Unix time, seconds
 */
void clawt_office_presence_note_call(ClawtOfficePresence *self, const gchar *from,
                                     const gchar *to, gint64 now);

/**
 * clawt_office_presence_note_meeting:
 * @self: the fold
 * @room: the group room
 * @members: (array zero-terminated=1): who is in the round
 * @town_hall: whether it is everybody's (the chief or you, with every lead)
 * @now: the current Unix time, seconds
 */
void clawt_office_presence_note_meeting(ClawtOfficePresence *self, const gchar *room,
                                        const gchar *const *members, gboolean town_hall,
                                        gint64 now);

/**
 * clawt_office_presence_note_delivery:
 * @self: the fold
 * @from: who handed work over
 * @to: who received it
 * @now: the current Unix time, seconds
 */
void clawt_office_presence_note_delivery(ClawtOfficePresence *self, const gchar *from,
                                         const gchar *to, gint64 now);

/**
 * clawt_office_presence_note_waiting:
 * @self: the fold
 * @id: the participant
 * @decision: the decision id
 * @open: whether it was asked (%TRUE) or settled
 */
void clawt_office_presence_note_waiting(ClawtOfficePresence *self, const gchar *id,
                                        const gchar *decision, gboolean open);

/**
 * clawt_office_presence_note_background:
 * @self: the fold
 * @id: the participant
 * @waits_pending: how many clawtilla waits it has pending
 *
 * The background work it is waiting on (since 0.78.0).  Between turns an
 * agent with a wait pending is at its desk working rather than in the
 * lounge: clawt_board_agent_activity() decides, the same rule agent.list
 * answers with.  A running turn still wins, and so does everything that
 * outranks a turn.
 */
void clawt_office_presence_note_background(ClawtOfficePresence *self,
                                           const gchar         *id,
                                           guint                waits_pending);

/**
 * clawt_office_presence_note_error:
 * @self: the fold
 * @id: the participant
 * @reason: (nullable): what went wrong, or %NULL to clear it
 * @now: the current Unix time, seconds
 */
void clawt_office_presence_note_error(ClawtOfficePresence *self, const gchar *id,
                                      const gchar *reason, gint64 now);

/**
 * clawt_office_presence_note_leaving:
 * @self: the fold
 * @id: a helper being reaped
 * @now: the current Unix time, seconds
 */
void clawt_office_presence_note_leaving(ClawtOfficePresence *self, const gchar *id,
                                        gint64 now);

/**
 * clawt_office_presence_resolve:
 * @self: the fold
 * @id: a participant
 * @now: the current Unix time, seconds
 * @out_station: (out) (optional): its station while working
 * @out_peer: (out) (optional) (transfer none): who it is on a call with,
 *   or delivering to
 * @out_room: (out) (optional) (transfer none): the meeting's room
 *
 * What to draw for @id now, by priority: waiting on you, error, in a
 * meeting, on a call, delivering, working or thinking, working at the
 * desk while a wait it set is pending, arriving, idle, offline.  Stale
 * edges have already decayed.
 *
 * Returns: the activity
 */
ClawtOfficeActivity clawt_office_presence_resolve(ClawtOfficePresence  *self,
                                                  const gchar          *id,
                                                  gint64                now,
                                                  ClawtOfficeStation   *out_station,
                                                  const gchar         **out_peer,
                                                  const gchar         **out_room);

/**
 * clawt_office_presence_changed_since:
 * @self: the fold
 * @now: the current Unix time, seconds
 *
 * The participants whose resolved activity, station, peer or room is not
 * what it was the last time this was asked -- the edges the daemon
 * publishes, never a repeat.
 *
 * Returns: (transfer full) (element-type utf8): their ids
 */
GPtrArray *clawt_office_presence_changed_since(ClawtOfficePresence *self, gint64 now);

/**
 * clawt_office_presence_add_participant_json:
 * @self: the fold
 * @id: a participant
 * @now: the current Unix time, seconds
 * @builder: where to write one object
 *
 * One participant as the wire has it: `fleet`, `kind`, `id`, `name`,
 * `team`, `parent`, `role`, `activity`, `station`, `peer`, `room`,
 * `since`, `detail`.
 *
 * Returns: %FALSE when there is no such participant
 */
gboolean clawt_office_presence_add_participant_json(ClawtOfficePresence *self,
                                                    const gchar         *id,
                                                    gint64               now,
                                                    JsonBuilder         *builder);

/**
 * clawt_office_presence_snapshot:
 * @self: the fold
 * @now: the current Unix time, seconds
 *
 * Every participant, in a stable order, as `office.snapshot` sends it.
 *
 * Returns: (transfer full): `{fleet, now, participants: [...]}`
 */
JsonNode *clawt_office_presence_snapshot(ClawtOfficePresence *self, gint64 now);

/**
 * clawt_office_station_for_tool:
 * @tool: (nullable): a tool's name, as a turn step carries it
 *
 * Returns: the station the tool belongs to
 */
ClawtOfficeStation clawt_office_station_for_tool(const gchar *tool);

/**
 * clawt_office_activity_nick:
 * @activity: an activity
 *
 * Returns: (transfer none): its wire name
 */
const gchar *clawt_office_activity_nick(ClawtOfficeActivity activity);

/**
 * clawt_office_activity_from_nick:
 * @nick: (nullable): a wire name
 * @out: (out): the activity
 *
 * Returns: %TRUE when @nick is an activity
 */
gboolean clawt_office_activity_from_nick(const gchar *nick, ClawtOfficeActivity *out);

/**
 * clawt_office_activity_count:
 *
 * Returns: how many activities there are, for walking them
 */
guint clawt_office_activity_count(void);

/**
 * clawt_office_activity_label:
 * @activity: an activity
 *
 * Returns: (transfer none): how to say it to a person: "at their desk"
 */
const gchar *clawt_office_activity_label(ClawtOfficeActivity activity);

/**
 * clawt_office_activity_tone:
 * @activity: an activity
 *
 * Returns: (transfer none): the badge tone (`good`, `warn`, `bad`,
 *   `info` or `neutral`)
 */
const gchar *clawt_office_activity_tone(ClawtOfficeActivity activity);

/**
 * clawt_office_station_nick:
 * @station: a station
 *
 * Returns: (transfer none): its wire name
 */
const gchar *clawt_office_station_nick(ClawtOfficeStation station);

/**
 * clawt_office_station_from_nick:
 * @nick: (nullable): a wire name
 *
 * Returns: the station, %CLAWT_OFFICE_STATION_NONE for an unknown one
 */
ClawtOfficeStation clawt_office_station_from_nick(const gchar *nick);

/**
 * clawt_office_kind_nick:
 * @kind: a kind
 *
 * Returns: (transfer none): its wire name
 */
const gchar *clawt_office_kind_nick(ClawtOfficeParticipantKind kind);

/**
 * clawt_office_roster_line:
 * @participant: one participant from `office.snapshot` or `office.presence`
 * @now: the current Unix time, seconds
 *
 * The who-is-where line every client draws: "on a call with kudu · 2m",
 * "in a meeting in #launch", "at the lab bench running tests".
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_office_roster_line(JsonObject *participant, gint64 now);

G_END_DECLS
