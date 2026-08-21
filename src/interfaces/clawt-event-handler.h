/*
 * clawt-event-handler.h - Reacting to what happens in the fleet
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

#define CLAWT_TYPE_EVENT_HANDLER (clawt_event_handler_get_type())

G_DECLARE_INTERFACE(ClawtEventHandler, clawt_event_handler, CLAWT,
                    EVENT_HANDLER, GObject)

/**
 * ClawtEventHandlerInterface:
 * @handles: whether this handler wants a given event kind
 * @handle: act on an event
 *
 * Implemented by a plugin that wants to see what the fleet is doing.
 */
struct _ClawtEventHandlerInterface {
    GTypeInterface parent_iface;

    gboolean (*handles) (ClawtEventHandler *self, const gchar *kind);
    void     (*handle)  (ClawtEventHandler *self, ClawtEvent *event);
};

/**
 * clawt_event_handler_handles:
 * @self: a #ClawtEventHandler
 * @kind: an event kind
 *
 * Asked before every dispatch, so a handler interested in one kind is not
 * woken for the other forty.
 *
 * Returns: %TRUE if @self wants events of this kind
 */
gboolean clawt_event_handler_handles(ClawtEventHandler *self,
                                     const gchar       *kind);

/**
 * clawt_event_handler_handle:
 * @self: a #ClawtEventHandler
 * @event: what happened
 *
 * Acts on one event.
 */
void clawt_event_handler_handle(ClawtEventHandler *self, ClawtEvent *event);

G_END_DECLS
