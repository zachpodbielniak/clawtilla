/*
 * clawt-read-store.h - How far the operator has read, per conversation
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The unread counts every client draws used to live in that client's
 * memory.  Reading a conversation in the GTK client left it lit in the
 * web client, a restart lit nothing at all, and a badge that one window
 * had cleared stayed on a second window for ever.  The mark is held here,
 * by the daemon, so there is one answer to "what have I read" and every
 * client is a view of it.
 *
 * One row per room: the id and timestamp of the last message the
 * operator has read in it.  The count itself is never stored -- it is
 * derived from the room's history every time it is asked for (see
 * clawt_room_count_unread()), so a count and the messages it counts can
 * never disagree.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_READ_STORE (clawt_read_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtReadStore, clawt_read_store, CLAWT, READ_STORE,
                     GObject)

/**
 * clawt_read_store_new:
 * @path: the database file, usually `<state_dir>/reads.db`
 * @error: (out) (optional): return location for a #GError
 *
 * Opens the store, creating the file when there is none.
 *
 * Returns: (transfer full) (nullable): the store, or %NULL with @error
 */
ClawtReadStore *clawt_read_store_new(const gchar *path, GError **error);

/**
 * clawt_read_store_was_created:
 * @self: a #ClawtReadStore
 *
 * Whether this open made the file rather than finding it.
 *
 * The daemon seeds a fresh store with a mark at every room's newest
 * message, so the build that introduces the store does not light up
 * every conversation the fleet ever had.  Asked of the store rather than
 * worked out from an empty table, because an empty table is also what a
 * fleet with no conversations looks like a week later -- and seeding
 * then would mark read messages nobody has seen.
 *
 * Returns: %TRUE if the file did not exist before this store opened it
 */
gboolean clawt_read_store_was_created(ClawtReadStore *self);

/**
 * clawt_read_store_get:
 * @self: a #ClawtReadStore
 * @room_id: the room
 * @out_last_id: (out) (optional) (transfer full) (nullable): the id of
 *   the last message read, or %NULL
 * @out_last_ts: (out) (optional): its timestamp, in seconds
 *
 * Returns: %TRUE if @room_id has a mark; on %FALSE the outputs are %NULL
 *   and 0, which is what "nothing read yet" means to
 *   clawt_room_count_unread()
 */
gboolean clawt_read_store_get(ClawtReadStore *self,
                              const gchar    *room_id,
                              gchar         **out_last_id,
                              gint64         *out_last_ts);

/**
 * clawt_read_store_set:
 * @self: a #ClawtReadStore
 * @room_id: the room
 * @last_id: (nullable): the id of the last message read
 * @last_ts: its timestamp, in seconds
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the room's mark, replacing any held.
 *
 * Unconditional: whether a mark is *ahead* of the one held is a question
 * about the room's history, which the store does not have.  The daemon
 * asks it (see `room.read` in daemon-room.c, which never moves a mark
 * backwards) and only then writes.
 *
 * Returns: %TRUE unless the write failed
 */
gboolean clawt_read_store_set(ClawtReadStore *self,
                              const gchar    *room_id,
                              const gchar    *last_id,
                              gint64          last_ts,
                              GError        **error);

/**
 * clawt_read_store_count:
 * @self: a #ClawtReadStore
 *
 * Returns: how many rooms have a mark
 */
guint clawt_read_store_count(ClawtReadStore *self);

G_END_DECLS
