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
 *
 * What the feed tells its host.  Called on the host's main context.
 */
typedef struct {
    void (*world)(const gchar *pack_dir, JsonObject *world, gpointer user_data);
    void (*layout)(JsonObject *layout, gpointer user_data);
    void (*color)(const gchar *id, const gchar *color, gpointer user_data);
    void (*status)(const gchar *text, gpointer user_data);
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
 * office.changed.  The host forwards its events here.
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
