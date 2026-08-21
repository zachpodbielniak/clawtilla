/*
 * clawt-event-bus.h - Where events go, and how a client catches up
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
#include "core/clawt-event.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_EVENT_BUS (clawt_event_bus_get_type())

G_DECLARE_FINAL_TYPE(ClawtEventBus, clawt_event_bus, CLAWT, EVENT_BUS,
                     GObject)

/**
 * clawt_event_bus_new:
 * @history: how many past events to keep for reconnecting clients
 *
 * Returns: (transfer full): a new #ClawtEventBus
 */
ClawtEventBus *clawt_event_bus_new(guint history);

/**
 * clawt_event_bus_publish:
 * @self: a #ClawtEventBus
 * @event: (transfer none): the event
 *
 * Stamps @event with the next cursor, records it and emits ::event.
 *
 * Returns: the cursor assigned
 */
guint64 clawt_event_bus_publish(ClawtEventBus *self, ClawtEvent *event);

/**
 * clawt_event_bus_emit:
 * @self: a #ClawtEventBus
 * @kind: the event kind
 * @subject: (nullable): what it happened to
 *
 * Convenience for publishing an event with no details.
 *
 * Returns: the cursor assigned
 */
guint64 clawt_event_bus_emit(ClawtEventBus *self,
                             const gchar   *kind,
                             const gchar   *subject);

/**
 * clawt_event_bus_replay:
 * @self: a #ClawtEventBus
 * @cursor: the last cursor the client saw, or 0 for "everything held"
 * @out_complete: (out) (optional): %FALSE if @cursor had already fallen out
 *   of the buffer, so the client is missing events nobody can supply
 *
 * Returns everything after @cursor.
 *
 * @out_complete is the point of this call.  A client that silently
 * receives a partial replay carries a hole for the rest of its life and
 * shows stale state it has no way to notice; one that is told the replay
 * was incomplete re-fetches and is correct again.
 *
 * Returns: (transfer container) (element-type ClawtEvent): the events
 */
GPtrArray *clawt_event_bus_replay(ClawtEventBus *self,
                                  guint64        cursor,
                                  gboolean      *out_complete);

/**
 * clawt_event_bus_get_cursor:
 * @self: a #ClawtEventBus
 *
 * Returns: the most recently assigned cursor
 */
guint64 clawt_event_bus_get_cursor(ClawtEventBus *self);

G_END_DECLS
