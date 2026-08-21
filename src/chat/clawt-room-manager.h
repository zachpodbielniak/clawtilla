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
 * clawt_room_manager_flush:
 * @self: a #ClawtRoomManager
 *
 * Writes every transcript to disk.
 *
 * Returns: how many rooms were written
 */
guint clawt_room_manager_flush(ClawtRoomManager *self);

G_END_DECLS
