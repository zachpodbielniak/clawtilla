/*
 * clawt-room-manager.h - The fleet's rooms and their transcripts
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "chat/clawt-room.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_ROOM_MANAGER (clawt_room_manager_get_type())

G_DECLARE_FINAL_TYPE(ClawtRoomManager, clawt_room_manager, CLAWT,
                     ROOM_MANAGER, GObject)

/**
 * clawt_room_manager_new:
 * @transcript_dir: (nullable): where to persist transcripts, or %NULL to
 *   keep them in memory only
 *
 * Returns: (transfer full): a new #ClawtRoomManager
 */
ClawtRoomManager *clawt_room_manager_new(const gchar *transcript_dir);

/**
 * clawt_room_manager_load:
 * @self: a #ClawtRoomManager
 * @config: the fleet configuration
 *
 * Creates the rooms declared in `rooms:` and reloads their transcripts.
 *
 * Returns: how many rooms were created
 */
guint clawt_room_manager_load(ClawtRoomManager *self, ClawtConfig *config);

/**
 * clawt_room_manager_set_agents:
 * @self: a #ClawtRoomManager
 * @agents: (transfer none) (nullable): the fleet
 *
 * Lets room creation refuse a name an agent already has.
 *
 * Every resolver in the tree tries a room first and falls back to
 * treating the id as an agent, which is what lets a client ask for a
 * conversation by naming the agent -- so a room called `oryx` would
 * hide the direct conversation with `oryx`, and the symptom is a chat
 * opening on the wrong transcript rather than anything that looks like
 * a collision.  Checked in the manager rather than at the two creation
 * sites, because a third one would not know to ask.
 *
 * Borrowed rather than referenced: the daemon owns both and outlives
 * this, and a reference would close a cycle through the router.
 */
void clawt_room_manager_set_agents(ClawtRoomManager  *self,
                                   ClawtAgentManager *agents);

/**
 * clawt_room_manager_create:
 * @self: a #ClawtRoomManager
 * @room_id: the id
 * @name: (nullable): display name
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer none) (nullable): the new room, or %NULL if the id
 *   is taken or invalid
 */
ClawtRoom *clawt_room_manager_create(ClawtRoomManager  *self,
                                     const gchar       *room_id,
                                     const gchar       *name,
                                     GError           **error);

/**
 * clawt_room_manager_get:
 * @self: a #ClawtRoomManager
 * @room_id: the id
 *
 * Returns: (transfer none) (nullable): the room, or %NULL
 */
ClawtRoom *clawt_room_manager_get(ClawtRoomManager *self,
                                  const gchar      *room_id);

/**
 * clawt_room_manager_direct_id:
 * @a: one member
 * @b: the other
 *
 * The id a direct room between @a and @b has, without making one.
 *
 * A client cannot work this out for itself -- how a direct room is named
 * is the daemon's business, and a client that takes "dm:a:b" apart is a
 * client that breaks when that changes.  So the daemon reports it, and
 * both the reporting and the creation ask this rather than each
 * spelling the format out.
 *
 * Returns: (transfer full): the id
 */
gchar *clawt_room_manager_direct_id(const gchar *a, const gchar *b);

/**
 * clawt_room_manager_get_direct:
 * @self: a #ClawtRoomManager
 * @a: one agent id
 * @b: the other
 *
 * The two-member room for a direct exchange, created if it does not
 * exist.  The id does not depend on which way round the pair is given,
 * so a reply lands in the same room as the message it answers rather than
 * starting a parallel one.
 *
 * Returns: (transfer none): the room
 */
ClawtRoom *clawt_room_manager_get_direct(ClawtRoomManager *self,
                                         const gchar      *a,
                                         const gchar      *b);

/**
 * clawt_room_manager_get_routine:
 * @self: a #ClawtRoomManager
 * @routine_id: which routine
 * @agent_id: the agent it runs against
 *
 * The room an isolated routine's runs happen in, making it if it is not
 * there.
 *
 * libreclaw keys a session on channel, room and sender, so a room of its
 * own with a sender of its own is a *session* of its own: a run no
 * longer inherits the last conversation's context, nor waits behind it.
 * Without one a routine is sent from `user` to the agent and lands in
 * the operator's room, from the operator's sender.
 *
 * The id is `routine:<id>`, with a colon for the same reason a direct
 * room's is: an agent or room id cannot contain one, so a room the
 * daemon owns can never collide with one somebody created.  That is also
 * why this exists rather than callers building the id and calling
 * clawt_room_manager_create(), which refuses a colon.
 *
 * One room per routine rather than per run.  A room per run would be
 * perfect isolation and no continuity at all, and continuity between a
 * routine's own runs is the thing worth having.
 *
 * Returns: (transfer none): the room
 */
ClawtRoom *clawt_room_manager_get_routine(ClawtRoomManager *self,
                                          const gchar      *routine_id,
                                          const gchar      *agent_id);

/**
 * clawt_room_manager_get_trigger:
 * @self: a #ClawtRoomManager
 * @trigger_id: which trigger
 * @agent_id: who runs it
 *
 * The room an isolated trigger's runs happen in.
 *
 * The same shape as clawt_room_manager_get_routine(), under a namespace
 * of its own: `trigger:<id>`. A shared prefix would put a trigger and a
 * routine of the same name in one room, and the symptom would be one
 * of them apparently answering the other's work.
 *
 * Returns: (transfer none): the room
 */
ClawtRoom *clawt_room_manager_get_trigger(ClawtRoomManager *self,
                                          const gchar      *trigger_id,
                                          const gchar      *agent_id);

/**
 * clawt_room_manager_list:
 * @self: a #ClawtRoomManager
 *
 * Returns: (transfer container) (element-type ClawtRoom): every room
 */
GPtrArray *clawt_room_manager_list(ClawtRoomManager *self);

/**
 * clawt_room_manager_remove:
 * @self: a #ClawtRoomManager
 * @room_id: the id
 *
 * Returns: %TRUE if a room was removed
 */
gboolean clawt_room_manager_remove(ClawtRoomManager *self,
                                   const gchar      *room_id);

/**
 * clawt_room_manager_rooms_for:
 * @self: a #ClawtRoomManager
 * @agent_id: an agent
 *
 * Returns: (transfer container) (element-type ClawtRoom): the rooms this
 *   agent is a member of
 */
GPtrArray *clawt_room_manager_rooms_for(ClawtRoomManager *self,
                                        const gchar      *agent_id);

/**
 * clawt_room_manager_load_direct:
 * @self: a #ClawtRoomManager
 *
 * Re-creates the direct rooms that have a transcript on disk.
 *
 * A direct room is made on demand by clawt_room_manager_get_direct(),
 * so after a restart one exists only once somebody sends a message
 * through it again -- which meant every conversation two agents had
 * ever had was missing from a listing until they spoke once more. The
 * transcripts were there the whole time; nothing had asked for them.
 *
 * Returns: how many were restored
 */
guint clawt_room_manager_load_direct(ClawtRoomManager *self);

/**
 * clawt_room_manager_flush:
 * @self: a #ClawtRoomManager
 *
 * Writes every transcript to disk.
 *
 * Returns: how many rooms were written
 */
guint clawt_room_manager_flush(ClawtRoomManager *self);

G_END_DECLS
