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

gboolean clawt_room_get_require_mention(ClawtRoom *self);

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
