/*
 * office-feed.h - The daemon's side of the drawn office
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Reads the world (office.world), copies the pack's files into a local
 * cache over the wire (office.file) so a renderer on another machine
 * draws the same pack, then reads who sits where (office.layout) once a
 * second and whenever an office.presence event says it changed.  Every
 * request is asynchronous on the host's main context, which the host
 * drains once a frame: nothing here waits.
 */

#ifndef CLAWT_OFFICE_FEED_H
#define CLAWT_OFFICE_FEED_H

#include <clawtilla.h>

G_BEGIN_DECLS

typedef struct _ClawtOfficeFeed ClawtOfficeFeed;

/**
 * ClawtOfficeFeedFuncs:
 * @world: the pack is in @pack_dir and @world describes it
 * @layout: who sits where now
 * @color: an agent's colour, from agent.list (@color may be %NULL)
 * @status: a line for the host to show, or %NULL to clear it
 * @character: an agent's character files (its sheet, its mesh) are cached
 *   under @dir at the paths @character names (`office.characters`); may be %NULL
 * @usage: what each provider login has left (`usage.providers`, as it
 *   stands -- the board never asks for a fresh read); may be %NULL
 * @look: which face @id wears, from agent.list.  @face is the resolved
 *   shape name or "initials", or %NULL.  @has_avatar is whether a picture
 *   is stored.  May be %NULL
 * @portrait: the decoded picture for @id, or a clear.  @rgba is square
 *   straight RGBA, @size on a side, valid only for this call (the host
 *   copies it); %NULL @rgba drops a picture the host is holding.  @etag
 *   is the bytes' tag, or %NULL when clearing.  May be %NULL
 *
 * What the feed tells its host.  Called on the host's main context.
 * @look and @portrait were added at the end so a host that still
 * initialises the struct by position keeps them %NULL.
 */
typedef struct {
    void (*world)(const gchar *pack_dir, JsonObject *world, gpointer user_data);
    void (*layout)(JsonObject *layout, gpointer user_data);
    void (*color)(const gchar *id, const gchar *color, gpointer user_data);
    void (*status)(const gchar *text, gpointer user_data);
    void (*character)(const gchar *agent, const gchar *dir, JsonObject *character,
                      gpointer user_data);
    void (*usage)(JsonObject *usage, gpointer user_data);
    void (*look)(const gchar *id, const gchar *name, const gchar *face,
                 gboolean has_avatar, gpointer user_data);
    void (*portrait)(const gchar *id, const guint8 *rgba, gint size,
                     const gchar *etag, gpointer user_data);
} ClawtOfficeFeedFuncs;

/**
 * clawt_office_feed_new:
 * @client: a connected (or connecting) client
 * @context: the main context the host drains
 * @world_id: (nullable): a world to draw, or %NULL for the configured one
 * @funcs: the host's callbacks, copied
 * @user_data: passed to each
 *
 * Returns: (transfer full): a feed; it starts at once
 */
ClawtOfficeFeed *clawt_office_feed_new(ClawtClient                *client,
                                       GMainContext               *context,
                                       const gchar                *world_id,
                                       const ClawtOfficeFeedFuncs *funcs,
                                       gpointer                    user_data);

/**
 * clawt_office_feed_event:
 * @self: a feed
 * @event: an event from the daemon
 *
 * Re-reads the layout on office.presence, and the world on
 * office.changed.  agent.changed (and any agent.avatar* event) re-reads
 * agent.list and refreshes that agent's picture.  The host forwards its
 * events here.
 */
void clawt_office_feed_event(ClawtOfficeFeed *self, ClawtEvent *event);

/**
 * clawt_office_feed_refresh:
 * @self: a feed
 *
 * Reads the world again from the start.
 */
void clawt_office_feed_refresh(ClawtOfficeFeed *self);

/**
 * clawt_office_feed_free:
 * @self: (nullable): a feed
 *
 * Stops the timer; replies still in flight are dropped.
 */
void clawt_office_feed_free(ClawtOfficeFeed *self);

G_END_DECLS

#endif /* CLAWT_OFFICE_FEED_H */
