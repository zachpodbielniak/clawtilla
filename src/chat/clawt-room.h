/*
 * clawt-room.h - A conversation with members
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Agents are managed as chats.  A room has members and a transcript;
 * posting into one enqueues to every member's mailbox.  A direct chat is a
 * room with two members, so there is one mechanism rather than two.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "chat/clawt-message.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_ROOM (clawt_room_get_type())

G_DECLARE_FINAL_TYPE(ClawtRoom, clawt_room, CLAWT, ROOM, GObject)

/**
 * clawt_room_new:
 * @room_id: the identifier used to address it
 * @transcript_path: (nullable): where to persist the transcript
 *
 * Returns: (transfer full): a new #ClawtRoom
 */
ClawtRoom *clawt_room_new(const gchar *room_id,
                          const gchar *transcript_path);

const gchar *clawt_room_get_id(ClawtRoom *self);
const gchar *clawt_room_get_name(ClawtRoom *self);
void         clawt_room_set_name(ClawtRoom *self, const gchar *name);

void     clawt_room_add_member(ClawtRoom *self, const gchar *agent_id);
gboolean clawt_room_remove_member(ClawtRoom *self, const gchar *agent_id);
gboolean clawt_room_has_member(ClawtRoom *self, const gchar *agent_id);

/**
 * clawt_room_get_members:
 * @self: a #ClawtRoom
 *
 * Returns: (transfer none) (element-type utf8): the agent ids in the room
 */
GPtrArray *clawt_room_get_members(ClawtRoom *self);

/**
 * clawt_room_set_require_mention:
 * @self: a #ClawtRoom
 * @require: whether a message must name an agent to reach it
 *
 * Worth turning on for a busy room: without it every agent takes a turn on
 * every message, which is expensive and rarely wanted.
 */
void clawt_room_set_require_mention(ClawtRoom *self, gboolean require);

/**
 * clawt_room_get_require_mention:
 * @self: a #ClawtRoom
 *
 * Whether a message must name a member to reach it.
 *
 * When nothing has set it, this follows the room's shape: two members
 * are a conversation and everything said in one is for the other, while
 * three or more is a group where delivering every remark to everybody
 * costs a model turn each.  One resolver, so a creation site cannot
 * arrive at a different answer -- the schema states the same rule.
 *
 * Returns: %TRUE if only named members receive
 */
gboolean clawt_room_get_require_mention(ClawtRoom *self);

/**
 * clawt_room_is_group:
 * @self: a #ClawtRoom
 *
 * Whether this room holds more than two members.
 *
 * The one spelling of the question, because it decides three separate
 * things -- the mention default above, which delivery preamble a member
 * is handed, and whether an agent's session may be partitioned by
 * sender.  There is deliberately no room-kind enum: the daemon's own
 * rooms are already told apart by their id prefix, and a fourth way of
 * asking what sort of room this is would be a fourth thing to drift.
 *
 * Returns: %TRUE if it has more than two members
 */
gboolean clawt_room_is_group(ClawtRoom *self);

/**
 * clawt_room_get_order:
 * @self: a #ClawtRoom
 *
 * Where it sits in the sidebar, on the same scale as an agent's, so a
 * client draws one list rather than two.
 *
 * Returns: the position, or 0 when it has none
 */
gint clawt_room_get_order(ClawtRoom *self);

/**
 * clawt_room_set_order:
 * @self: a #ClawtRoom
 * @order: where it sits, on the same scale as an agent's
 *
 * Set by the config loader and by `fleet.reorder`.  Both, because
 * `room.list` sorts on this and writing only the config left a reorder
 * invisible until the next daemon start.
 */
void clawt_room_set_order(ClawtRoom *self, gint order);

/**
 * clawt_room_get_team:
 * @self: a #ClawtRoom
 *
 * Which team's group it appears under.  Presentation and nothing else:
 * it changes neither who is in the room nor who a message reaches.
 *
 * Returns: (nullable) (transfer none): the team id, or %NULL
 */
const gchar *clawt_room_get_team(ClawtRoom *self);

/**
 * clawt_room_set_team:
 * @self: a #ClawtRoom
 * @team: (nullable): the team whose group it appears under, or %NULL
 *
 * Presentation only: it changes neither who is in the room nor who a
 * message reaches.
 */
void clawt_room_set_team(ClawtRoom *self, const gchar *team);

/**
 * clawt_room_get_catchup_messages:
 * @self: a #ClawtRoom
 *
 * How much of the room a member is caught up on when it is named.
 *
 * In a room that requires mentions an agent receives only what named
 * it, so without this it cannot follow the conversation at all -- the
 * transcript holds it and the model does not.
 *
 * Returns: the cap, or 0 for none
 */
guint clawt_room_get_catchup_messages(ClawtRoom *self);

/**
 * clawt_room_set_catchup_messages:
 * @self: a #ClawtRoom
 * @messages: how many to carry, or 0 for none
 */
void clawt_room_set_catchup_messages(ClawtRoom *self, guint messages);

/**
 * clawt_room_names_any_member:
 * @self: a #ClawtRoom
 * @body: (nullable): the message text
 * @agents: (transfer none) (nullable): the fleet, for display names
 *
 * Whether @body addresses anybody who is in this room.
 *
 * Asked of the members rather than of the text alone: naming somebody
 * who is not here has addressed nobody.  It resolves each member's
 * display name the same way delivery does, which is why it takes the
 * fleet -- two private copies of this walk passed %NULL for the name,
 * so a post naming an agent by the name its own roster advertises was
 * delivered and reported as having named nobody.
 *
 * Returns: %TRUE if any member is named
 */
gboolean clawt_room_names_any_member(ClawtRoom         *self,
                                     const gchar       *body,
                                     ClawtAgentManager *agents);

/**
 * clawt_room_is_declared:
 * @room_id: a room id
 *
 * Whether this room is one somebody wrote down, as against one the
 * daemon derives from who exists.
 *
 * A direct room, a routine's room and a trigger's room have no config
 * entry: their members follow from the pair or the owner, so editing
 * one is meaningless and removing one deletes nothing and comes back
 * the moment the two speak again.  Told apart by the prefixes that
 * already name them -- a declared id cannot contain a colon, so the two
 * sets cannot overlap.
 *
 * Takes an id rather than a room because every caller is checking
 * whether it may act on a name it was handed.
 *
 * Returns: %TRUE if it is a room somebody declared
 */
gboolean clawt_room_is_declared(const gchar *room_id);

/**
 * clawt_room_member_list:
 * @self: a #ClawtRoom
 *
 * Its members as one comma-separated string, which is how they cross
 * IPC and how they are written back to the config.
 *
 * Returns: (transfer full): the list, possibly empty
 */
gchar *clawt_room_member_list(ClawtRoom *self);

/**
 * clawt_room_message_is_for:
 * @self: a #ClawtRoom
 * @message: (transfer none): a message posted to the room
 * @agent_id: a member
 * @display_name: (nullable): what that member is called, when it differs
 *   from its id
 *
 * Whether @agent_id should receive @message.
 *
 * An agent never receives its own message, whatever the mention rules say
 * -- that alone would be an infinite loop.
 *
 * @display_name comes from the caller because a room holds ids and
 * nothing else: it has no view of the fleet, and giving it one so that
 * it could look a name up would be a second answer to who an agent is.
 *
 * Returns: %TRUE if it should be delivered
 */
gboolean clawt_room_message_is_for(ClawtRoom    *self,
                                   ClawtMessage *message,
                                   const gchar  *agent_id,
                                   const gchar  *display_name);

/**
 * clawt_room_append:
 * @self: a #ClawtRoom
 * @message: (transfer none): the message
 * @error: (out) (optional): return location for a #GError
 *
 * Adds a message to the transcript.
 *
 * Returns: %TRUE if it was recorded
 */
void clawt_room_set_transcript_path(ClawtRoom *self, const gchar *path);

void clawt_room_restore(ClawtRoom *self, ClawtMessage *message);

gboolean clawt_room_append(ClawtRoom     *self,
                           ClawtMessage  *message,
                           GError       **error);

/**
 * clawt_room_get_history:
 * @self: a #ClawtRoom
 * @limit: how many of the most recent to return, or 0 for all
 *
 * Returns: (transfer full) (element-type ClawtMessage): the transcript,
 *   oldest first
 */
GPtrArray *clawt_room_get_history(ClawtRoom *self, guint limit);

/**
 * clawt_room_get_message_count:
 * @self: a #ClawtRoom
 *
 * How many messages the room holds.
 *
 * Separate from clawt_room_get_history() because a listing wants the
 * number beside every room and copying every message to count them is
 * the whole transcript per row.
 *
 * Returns: the count
 */
guint clawt_room_get_message_count(ClawtRoom *self);

/**
 * clawt_room_get_max_hops:
 * @self: a #ClawtRoom
 *
 * Returns: this room's hop limit, or 0 to use the global one
 */
guint clawt_room_get_max_hops(ClawtRoom *self);

void clawt_room_set_max_hops(ClawtRoom *self, guint max_hops);

/**
 * clawt_room_get_turn_timeout:
 * @self: a #ClawtRoom
 *
 * How long one member may hold this room's turn, counted in work rather
 * than in wall time -- the clock holds while the turn is parked on an
 * open decision.
 *
 * Returns: the budget in seconds, or 0 for no bound
 */
guint clawt_room_get_turn_timeout(ClawtRoom *self);

/**
 * clawt_room_set_turn_timeout:
 * @self: a #ClawtRoom
 * @seconds: the budget, or 0 for no bound
 */
void clawt_room_set_turn_timeout(ClawtRoom *self, guint seconds);

G_END_DECLS
