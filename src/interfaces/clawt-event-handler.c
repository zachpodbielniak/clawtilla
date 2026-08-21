/*
 * clawt-event-handler.c - Reacting to what happens in the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "interfaces/clawt-event-handler.h"

G_DEFINE_INTERFACE(ClawtEventHandler, clawt_event_handler, G_TYPE_OBJECT)

static void
clawt_event_handler_default_init(ClawtEventHandlerInterface *iface)
{
    (void)iface;
}

gboolean
clawt_event_handler_handles(ClawtEventHandler *self, const gchar *kind)
{
    ClawtEventHandlerInterface *iface;

    g_return_val_if_fail(CLAWT_IS_EVENT_HANDLER(self), FALSE);

    iface = CLAWT_EVENT_HANDLER_GET_IFACE(self);

    /*
     * A handler that does not say defaults to wanting everything.  The
     * opposite default would make a plugin that forgot to implement this
     * silently receive nothing, which is a very quiet way to fail.
     */
    if (iface->handles == NULL)
        return TRUE;

    return iface->handles(self, kind);
}

void
clawt_event_handler_handle(ClawtEventHandler *self, ClawtEvent *event)
{
    ClawtEventHandlerInterface *iface;

    g_return_if_fail(CLAWT_IS_EVENT_HANDLER(self));
    g_return_if_fail(event != NULL);

    iface = CLAWT_EVENT_HANDLER_GET_IFACE(self);

    if (iface->handle != NULL)
        iface->handle(self, event);
}
