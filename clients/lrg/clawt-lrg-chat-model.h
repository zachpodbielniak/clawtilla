/*
 * clawt-lrg-chat-model.h - What the sidebar and the chat page decide
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The fleet sidebar and the Chat page of clawtilla-lrg draw; everything
 * they *decide* is here, with no libregnum header in sight, so
 * tests/test-lrg-models-chat.c can hold it to the GTK client's rules on
 * a machine with no display: which rooms count unread and when the count
 * clears, which agent is mid-turn (a level, kept as a set of rooms), the
 * order the sidebar draws the fleet in, what a drag sends, the slash
 * commands and their answers, where steps fall between messages, what an
 * attachment block names.
 */

#pragma once

#include "clawt-lrg-model.h"

G_BEGIN_DECLS

/* ── Unread ───────────────────────────────────────────────────────── */

/**
 * clawt_lrg_chat_row_rooms:
 * @agents: (nullable): an `agent.list` reply
 * @rooms: (nullable): a `room.list` reply
 *
 * The rooms the sidebar draws a row for: every agent's conversation with
 * the operator (`dm_room`) and every declared room.  This is the @rows
 * clawt_unread_should_count() wants, so peer traffic and routine rooms --
 * which have no row to click to clear them -- never count.
 *
 * Returns: (transfer full): a set of room ids
 */
GHashTable *clawt_lrg_chat_row_rooms(JsonNode *agents, JsonNode *rooms);

/**
 * clawt_lrg_unread_note:
 * @unread: room id -> GUINT_TO_POINTER(count)
 * @room_id: (nullable): where the message arrived
 * @viewing_room: (nullable): the room on screen
 * @from: (nullable): who sent it
 * @event_ts: when it happened, in microseconds
 * @connected_at: when this client connected, in microseconds
 * @rows: (nullable): clawt_lrg_chat_row_rooms()
 *
 * Counts one arriving message if clawt_unread_should_count() says so.
 *
 * Returns: %TRUE if a count went up
 */
gboolean clawt_lrg_unread_note(GHashTable  *unread,
                               const gchar *room_id,
                               const gchar *viewing_room,
                               const gchar *from,
                               gint64       event_ts,
                               gint64       connected_at,
                               GHashTable  *rows);

/**
 * clawt_lrg_unread_clear:
 * @unread: the table
 * @room_id: (nullable): a room that has just been opened
 *
 * Opening a conversation is the only thing that clears its count.
 *
 * Returns: %TRUE if there was a count to clear
 */
gboolean clawt_lrg_unread_clear(GHashTable *unread, const gchar *room_id);

/**
 * clawt_lrg_unread_prune:
 * @unread: the table
 * @rows: (nullable): the rooms the sidebar still draws
 *
 * Drops every count against a room the sidebar no longer draws, so a
 * removed agent's number does not stay in the total for ever.  A %NULL
 * @rows (no listing yet) prunes nothing: not knowing is not "gone".
 *
 * Returns: %TRUE if anything was dropped
 */
gboolean clawt_lrg_unread_prune(GHashTable *unread, GHashTable *rows);

/* The count for one room, and across all of them. */
guint clawt_lrg_unread_count(GHashTable *unread, const gchar *room_id);
guint clawt_lrg_unread_total(GHashTable *unread);

/**
 * clawt_lrg_unread_badge:
 * @unread: the table
 * @on_chat: whether the Chat page is the one showing
 *
 * The number on the Chat tab.  Hidden while the Chat page is on screen:
 * the sidebar beside it says *which* conversation, which is strictly
 * more, and this client's sidebar never collapses.
 *
 * Returns: the count to draw, 0 for none
 */
guint clawt_lrg_unread_badge(GHashTable *unread, gboolean on_chat);

/* ── Typing ───────────────────────────────────────────────────────── */

/**
 * clawt_lrg_typing_note:
 * @typing: agent id -> set of room ids
 * @agent_id: (nullable): the agent the frame is about
 * @room_id: (nullable): the room its turn is in ("" when unsaid)
 * @typing_now: what the frame says
 *
 * Folds one `agent.typing` frame into the table.
 *
 * A typing frame is a level, re-sent every 25 seconds per room while a
 * turn runs, and an agent can be mid-turn in several rooms at once -- so
 * this keeps the *set* of rooms and reports the edge of the whole set,
 * never of one frame.  An agent whose last turn ended keeps an empty set,
 * which is how clawt_lrg_typing_busy() tells "known idle" from "never
 * heard of".
 *
 * Returns: %TRUE when the agent went from idle to busy or back
 */
gboolean clawt_lrg_typing_note(GHashTable  *typing,
                               const gchar *agent_id,
                               const gchar *room_id,
                               gboolean     typing_now);

/**
 * clawt_lrg_typing_busy:
 * @typing: the table
 * @agent_id: (nullable): an agent
 * @listed_busy: what the last `agent.list` said
 *
 * Whether the agent is mid-turn: the frames when any have arrived for it
 * (they are newer than any listing), the listing otherwise -- an agent
 * already working when this client connected sends no edge at all.
 *
 * Returns: %TRUE if a turn is running anywhere
 */
gboolean clawt_lrg_typing_busy(GHashTable  *typing,
                               const gchar *agent_id,
                               gboolean     listed_busy);

/* Whether a turn of @agent_id is running in @room_id. */
gboolean clawt_lrg_typing_in_room(GHashTable  *typing,
                                  const gchar *agent_id,
                                  const gchar *room_id);

/**
 * clawt_lrg_typing_forget:
 * @typing: the table
 * @agent_id: (nullable): an agent whose turns all ended another way --
 *   it stopped, or its turn was interrupted
 *
 * State describing a process is cleared when the process ends, all of
 * it: left alone, the next frame is not a rising edge and the agent
 * would be drawn working for ever.
 *
 * Returns: %TRUE if the agent had been busy
 */
gboolean clawt_lrg_typing_forget(GHashTable *typing, const gchar *agent_id);

/**
 * clawt_lrg_typing_activity:
 * @typing_now: whether a turn is running
 * @peer: (nullable): who it is for
 *
 * The chat's activity line: "thinking", "working for <peer>" when the
 * turn is not for the operator, or %NULL when nothing is running.
 *
 * Returns: (transfer full) (nullable): the line
 */
gchar *clawt_lrg_typing_activity(gboolean typing_now, const gchar *peer);

/* ── The fleet sidebar ────────────────────────────────────────────── */

/**
 * ClawtLrgAgentRow:
 * @id: the agent's id
 * @name: what the row is titled
 * @subtitle: (nullable): what it is doing, or its description
 * @description: (nullable): its description, whatever the setting
 * @state: its lifecycle state
 * @state_tone: the tone that state is drawn in
 * @team: its team, "" for none
 * @dm_room: (nullable): its conversation with the operator
 * @color: (nullable): its colour
 * @computer: its computer type
 * @busy: whether a turn is running
 * @live: whether its process is up (the avatar glows)
 * @has_avatar: whether it has a picture
 * @host: whether it can run commands on this machine (HOST badge)
 * @machine: whether its computer has a machine to power
 * @stop_removes: whether stopping that machine destroys it
 * @can_interrupt: whether it offers Stop-this-turn
 * @standing: chief, lead or neither
 * @unread: messages in @dm_room nobody has read
 *
 * One agent row, decided.
 */
typedef struct {
    gchar          *id;
    gchar          *name;
    gchar          *subtitle;
    gchar          *description;
    gchar          *state;
    const gchar    *state_tone;
    gchar          *team;
    gchar          *dm_room;
    gchar          *color;
    gchar          *computer;
    gboolean        busy;
    gboolean        live;
    gboolean        has_avatar;
    gboolean        host;
    gboolean        machine;
    gboolean        stop_removes;
    gboolean        can_interrupt;
    ClawtTeamBadge  standing;
    guint           unread;
} ClawtLrgAgentRow;

/**
 * clawt_lrg_agent_row_new:
 * @agent: one agent from `agent.list`
 * @unread: (nullable): room id -> count
 * @typing: (nullable): agent id -> set of rooms
 * @show_descriptions: the appearance setting
 *
 * The subtitle is what the agent is doing (clawt_agent_activity_label(),
 * merged with a hold and the mailbox depth exactly as the GTK row does)
 * and, when it is doing nothing, its description -- unless descriptions
 * are turned off.  The activity is never hidden.
 *
 * Returns: (transfer full): the row
 */
ClawtLrgAgentRow *clawt_lrg_agent_row_new(JsonObject *agent,
                                          GHashTable *unread,
                                          GHashTable *typing,
                                          gboolean    show_descriptions);
void clawt_lrg_agent_row_free(ClawtLrgAgentRow *row);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtLrgAgentRow, clawt_lrg_agent_row_free)

/* An agent state's tone is clawt_lrg_agent_state_tone(), in the agent model. */

/**
 * clawt_lrg_agent_verb_allowed:
 * @state: (nullable): the agent's state
 * @verb: "start", "stop", "restart" or "delete"
 *
 * The context menu greys out what would fail: a shadow agent cannot
 * run, a running one cannot start, a stopped one cannot stop.
 *
 * Returns: %TRUE if the verb is offered
 */
gboolean clawt_lrg_agent_verb_allowed(const gchar *state, const gchar *verb);

/**
 * ClawtLrgEntryKind:
 * @CLAWT_LRG_ENTRY_TEAM: a team heading (also a drop target)
 * @CLAWT_LRG_ENTRY_AGENT: an agent
 * @CLAWT_LRG_ENTRY_ROOM: a declared room
 * @CLAWT_LRG_ENTRY_EMPTY: the "No agents yet" placeholder
 */
typedef enum {
    CLAWT_LRG_ENTRY_TEAM,
    CLAWT_LRG_ENTRY_AGENT,
    CLAWT_LRG_ENTRY_ROOM,
    CLAWT_LRG_ENTRY_EMPTY
} ClawtLrgEntryKind;

/**
 * ClawtLrgSidebarEntry:
 * @kind: what the row is
 * @id: the team id ("" for teamless), agent id or room id
 * @team: the group the row is drawn under, "" for teamless
 * @title: the row's title
 * @subtitle: (nullable): a room's membership line, a team's tally
 * @description: (nullable): a team's description
 * @running: a team's running agents
 * @total: a team's agents
 * @busy: a team's agents mid-turn
 * @collapsed: whether a team heading is folded
 * @object: (nullable): the agent or room object, borrowed from the reply
 */
typedef struct {
    ClawtLrgEntryKind  kind;
    gchar             *id;
    gchar             *team;
    gchar             *title;
    gchar             *subtitle;
    gchar             *description;
    guint              running;
    guint              total;
    guint              busy;
    gboolean           collapsed;
    JsonObject        *object;
} ClawtLrgSidebarEntry;

/**
 * clawt_lrg_sidebar_entries:
 * @agents: (nullable): `agent.list`, already grouped by the daemon
 * @teams: (nullable): `team.list`
 * @rooms: (nullable): `room.list`, already ordered by the daemon
 * @collapsed: (nullable): the set of folded team ids ("" is teamless)
 *
 * The sidebar, top to bottom, exactly as the GTK client lays it out: a
 * heading whenever the team changes; the headings of empty teams filled
 * in around them in the daemon's group order, because a heading is what
 * an agent is dragged onto; each team's declared rooms after its agents;
 * whatever nobody is on at the bottom.  A folded team keeps its heading
 * (with a tally of what is behind it) and loses its rows.
 *
 * Returns: (transfer full) (element-type ClawtLrgSidebarEntry): entries
 *   that borrow from @agents and @rooms
 */
GPtrArray *clawt_lrg_sidebar_entries(JsonNode   *agents,
                                     JsonNode   *teams,
                                     JsonNode   *rooms,
                                     GHashTable *collapsed);

/* "2/3", or "1 working · 2/3" when anybody is mid-turn. */
gchar *clawt_lrg_team_tally_text(guint running, guint total, guint busy);

/* A room's name, or its members when nobody named it. */
gchar *clawt_lrg_room_label(JsonObject *room);

/* "3 members · answers when named" or "… · everyone answers". */
gchar *clawt_lrg_room_subtitle(JsonObject *room);

/* "a:<id>" for an agent row, "r:<id>" for a room, NULL for anything else. */
gchar *clawt_lrg_entry_token(const ClawtLrgSidebarEntry *entry);

/**
 * clawt_lrg_entry_team_of:
 * @entries: clawt_lrg_sidebar_entries()
 * @token: "a:<id>" or "r:<id>"
 *
 * Returns: (transfer none) (nullable): the group that row is under, or
 *   %NULL when it is not drawn (a folded team) -- "cannot tell", which a
 *   caller must not read as "teamless"
 */
const gchar *clawt_lrg_entry_team_of(GPtrArray *entries, const gchar *token);

/**
 * clawt_lrg_reorder_entries:
 * @entries: clawt_lrg_sidebar_entries()
 * @dragged: the dragged row's token
 * @onto: the token of the row it was dropped on
 * @after: dropped on the lower half, so it goes below
 *
 * The whole order for `fleet.reorder`, so one frame describes the
 * arrangement completely: every agent and room row in sidebar order,
 * @dragged taken out and put back beside @onto.  Headings are not part
 * of the order.
 *
 * Returns: (transfer full) (nullable): comma-separated tokens, or %NULL
 *   when the drop means nothing (onto itself, or onto no row)
 */
gchar *clawt_lrg_reorder_entries(GPtrArray   *entries,
                                 const gchar *dragged,
                                 const gchar *onto,
                                 gboolean     after);

/**
 * clawt_lrg_team_choices:
 * @teams: (nullable): `team.list`
 * @current: (nullable): the team the agent is on now
 * @ids: (element-type utf8): filled with team ids, "" first
 * @labels: (element-type utf8): filled with what to show for each
 *
 * "No team" first, then every declared team, then -- when the agent
 * names a team nobody declared -- that team under its own id, so the
 * menu can show where the agent actually is.
 */
void clawt_lrg_team_choices(JsonNode    *teams,
                            const gchar *current,
                            GPtrArray   *ids,
                            GPtrArray   *labels);

/* The team's display name, or its id when it is not declared. */
const gchar *clawt_lrg_team_name(JsonNode *teams, const gchar *team_id);

/* "<who> moved to <team>." or "<who> taken off its team." */
gchar *clawt_lrg_moved_text(const gchar *who,
                            const gchar *team,
                            const gchar *team_label);

/* What a computer.start/stop/restart reply says. */
gchar *clawt_lrg_power_text(const gchar *agent_id, JsonNode *reply);

/* The confirmation before stopping a machine that does not survive it. */
gchar *clawt_lrg_power_confirm_title(const gchar *verb);
gchar *clawt_lrg_power_confirm_body(const gchar *agent_id);

/* What an `agent.remove` reply says, naming its computer's fate. */
gchar *clawt_lrg_removed_text(const gchar *agent_id, JsonNode *reply);

/* ── The chat page ────────────────────────────────────────────────── */

/**
 * ClawtLrgSlashCommand:
 * @name: "/help"
 * @argument: (nullable): what is typed after it
 * @summary: one line of what it does
 */
typedef struct {
    const gchar *name;
    const gchar *argument;
    const gchar *summary;
} ClawtLrgSlashCommand;

/* Every built-in, in the GTK client's order. */
const ClawtLrgSlashCommand *clawt_lrg_slash_commands(guint *n_commands);

/* The built-in called @name, or NULL. */
const ClawtLrgSlashCommand *clawt_lrg_slash_find(const gchar *name);

/**
 * clawt_lrg_slash_split:
 * @text: what was typed
 * @name: (out) (transfer full): the command word, with its slash
 * @rest: (out) (transfer full) (nullable): the stripped argument
 *
 * Returns: %FALSE when @text is not a command at all
 */
gboolean clawt_lrg_slash_split(const gchar  *text,
                               gchar       **name,
                               gchar       **rest);

/* The /help text: every command, then the composer's keys. */
gchar *clawt_lrg_slash_help(void);

/* "/memory <query>": the name, and the argument when there is one. */
gchar *clawt_lrg_slash_label(const gchar *name, const gchar *argument);

/**
 * clawt_lrg_slash_completing:
 * @text: (nullable): the composer's text
 *
 * Returns: %TRUE while the line is a command still being named -- it
 *   starts with "/" and has no space yet -- which is when "/" offers the
 *   list
 */
gboolean clawt_lrg_slash_completing(const gchar *text);

/**
 * clawt_lrg_slash_needs_agent:
 * @name: a built-in's name
 *
 * Returns: %TRUE for the commands that are about one agent -- which a
 *   room selection cannot answer
 */
gboolean clawt_lrg_slash_needs_agent(const gchar *name);

/* What a command about one agent says when no agent is selected. */
const gchar *clawt_lrg_slash_needs_agent_text(gboolean room_selected);

/* "There is no /x. Type /help for the list." */
gchar *clawt_lrg_slash_unknown_text(const gchar *name);

/* /agents: id, state and description per agent. */
gchar *clawt_lrg_chat_agents_text(JsonNode *reply);

/* /files: every workspace file, and how to open one. */
gchar *clawt_lrg_chat_files_text(JsonNode *reply);

/* The daemon's path for workspace file @name, or NULL. */
const gchar *clawt_lrg_chat_file_path(JsonNode *reply, const gchar *name);

/* /memory: what was remembered, or that nothing matches. */
gchar *clawt_lrg_chat_memory_text(JsonNode *reply);

/* /reset: how many sessions were cleared. */
gchar *clawt_lrg_chat_reset_text(JsonNode *reply);

/**
 * clawt_lrg_chat_interrupt_text:
 * @agent_id: the agent
 * @reply: an `agent.interrupt` reply
 *
 * The count is the answer: "Stopped" over an agent that was between
 * turns claims something happened that did not.
 *
 * Returns: (transfer full): the sentence
 */
gchar *clawt_lrg_chat_interrupt_text(const gchar *agent_id, JsonNode *reply);

/* /retry: the operator's last message in a `room.history` reply, or NULL. */
const gchar *clawt_lrg_chat_last_user_message(JsonNode *history);

/**
 * clawt_lrg_chat_send_note:
 * @target: who it went to
 * @reply: the `msg.send` reply
 * @waiting: (out): whether to say "delivered -- waiting for a reply"
 *
 * A steer is absent from the transcript until the turn ends and a
 * stopped agent holds the message in its mailbox; both have to be said,
 * or a message that left the composer reads as lost.
 *
 * Returns: (transfer full) (nullable): a toast, or %NULL for none
 */
gchar *clawt_lrg_chat_send_note(const gchar *target,
                                JsonNode    *reply,
                                gboolean    *waiting);

/* "copy-org" -> ORG, "save-text" -> PLAIN, anything else MARKDOWN. */
ClawtExportFormat clawt_lrg_export_format(const gchar *action);

/**
 * clawt_lrg_chat_export_messages:
 * @history: a `room.history` reply
 * @room_id: (nullable): the room, for the messages
 *
 * Returns: (transfer full) (element-type ClawtMessage): the messages,
 *   timestamped, for clawt_export_transcript()
 */
GPtrArray *clawt_lrg_chat_export_messages(JsonNode    *history,
                                          const gchar *room_id);

/* "task 1a2b3c4d" for a message carrying a task id, else NULL. */
gchar *clawt_lrg_task_chip_text(const gchar *task);

/* "hop 3" from the second hop on, else NULL. */
gchar *clawt_lrg_hop_text(gint64 depth);

/* "thinking: …" for reasoning, the summary otherwise. */
gchar *clawt_lrg_step_line(ClawtTurnStep *step);

/**
 * clawt_lrg_steps_before:
 * @steps: (element-type ClawtTurnStep): a room's steps, oldest first
 * @from: where the last stretch ended
 * @message_ts: the next message's timestamp
 *
 * Where the steps that happened before a message end, so a history
 * rebuilt from two fetches puts each stretch of tool calls between the
 * message that prompted it and the answer it produced.
 *
 * Returns: the index of the first step that does not precede it
 */
guint clawt_lrg_steps_before(GPtrArray *steps, guint from, gint64 message_ts);

/**
 * ClawtLrgRun:
 * @sender: (nullable): the last message's sender
 * @day: (nullable): its day, `%Y-%m-%d`
 *
 * Where the transcript's last run stands.
 */
typedef struct {
    gchar *sender;
    gchar *day;
} ClawtLrgRun;

/**
 * clawt_lrg_run_advance:
 * @run: the run so far
 * @sender: who sent the next message
 * @when: its time
 * @new_day: (out) (optional): whether a day divider goes above it
 *
 * Returns: %TRUE when the message begins a new run (a header, an avatar)
 */
gboolean clawt_lrg_run_advance(ClawtLrgRun *run,
                               const gchar *sender,
                               GDateTime   *when,
                               gboolean    *new_day);
void     clawt_lrg_run_reset(ClawtLrgRun *run);

/* Image extensions the transcript previews rather than lists. */
gboolean clawt_lrg_looks_like_image(const gchar *path);

/**
 * ClawtLrgAttachmentRef:
 * @daemon_copy: %TRUE for `clawt:<id>` (an agent's file, fetched with
 *   `attachment.get`), %FALSE for a host path
 * @value: the id or the path
 */
typedef struct {
    gboolean  daemon_copy;
    gchar    *value;
} ClawtLrgAttachmentRef;

void clawt_lrg_attachment_ref_free(ClawtLrgAttachmentRef *ref);

/**
 * clawt_lrg_attachment_refs:
 * @body: (nullable): a message body
 *
 * The files a message's attachment block names, in order: the block is
 * the list under CLAWT_ATTACHMENT_MARKER, ended by anything at the left
 * margin, and the container path in brackets is skipped.
 *
 * Returns: (transfer full) (element-type ClawtLrgAttachmentRef): refs
 */
GPtrArray *clawt_lrg_attachment_refs(const gchar *body);

/**
 * ClawtLrgSentFile:
 * @name: the file's name
 * @host_path: (nullable): where the daemon put it on the host
 * @path: (nullable): where the agent sees it
 * @saved: whether `attachment.put` succeeded
 */
typedef struct {
    gchar    *name;
    gchar    *host_path;
    gchar    *path;
    gboolean  saved;
} ClawtLrgSentFile;

void clawt_lrg_sent_file_free(ClawtLrgSentFile *file);

/**
 * clawt_lrg_body_with_attachments:
 * @body: what was typed
 * @files: (element-type ClawtLrgSentFile) (nullable): what was put
 *
 * The body plus the block that names each file, host path first: an
 * agent's own read tool runs on the host even when its shell does not.
 *
 * Returns: (transfer full): the body to send
 */
gchar *clawt_lrg_body_with_attachments(const gchar *body, GPtrArray *files);

/* Over 32 MB: base64 over the socket would stall the daemon. */
gboolean clawt_lrg_attachment_too_big(gsize size);

/**
 * clawt_lrg_room_roster:
 * @rooms: (nullable): `room.list`
 * @room_id: (nullable): a room
 *
 * Returns: (transfer full) (element-type utf8): its members, possibly
 *   empty
 */
GPtrArray *clawt_lrg_room_roster(JsonNode *rooms, const gchar *room_id);

/**
 * clawt_lrg_mention_rows:
 * @prefix: what follows the `@` being typed ("" right after it)
 * @roster: (element-type utf8): the room's members
 * @offer_all: (out): whether `@all` is offered after them
 *
 * The members to offer, from clawt_mention_candidates(); `@all` last,
 * only on a prefix it starts with and never when a member is called that.
 *
 * Returns: (transfer full) (element-type utf8): the members
 */
GPtrArray *clawt_lrg_mention_rows(const gchar *prefix,
                                  GPtrArray   *roster,
                                  gboolean    *offer_all);

/**
 * clawt_lrg_conversation_peers:
 * @rooms: (nullable): `room.list`
 * @agent_id: the selected agent
 *
 * The other party of each direct conversation @agent_id has with another
 * agent -- the operator's own is not one, it is always first.
 *
 * Returns: (transfer full) (element-type utf8): peer ids
 */
GPtrArray *clawt_lrg_conversation_peers(JsonNode    *rooms,
                                        const gchar *agent_id);

/* "Chat" for the operator's own, "with <peer>" for another. */
gchar *clawt_lrg_conversation_label(const gchar *peer);

/**
 * clawt_lrg_history_payload:
 * @room_entry: (nullable): a selected room
 * @conversation: (nullable): a peer conversation of @agent_id
 * @agent_id: (nullable): the selected agent
 * @limit: (nullable): "5000" for an export, %NULL for the default
 *
 * A group is named directly; an agent's own chat and its peer chats are
 * resolved by the daemon from a member and a viewer, so no client has to
 * know how a direct room is spelled.
 *
 * Returns: (transfer full) (nullable): the payload, or %NULL with
 *   nothing selected
 */
JsonNode *clawt_lrg_history_payload(const gchar *room_entry,
                                    const gchar *conversation,
                                    const gchar *agent_id,
                                    const gchar *limit);

G_END_DECLS
