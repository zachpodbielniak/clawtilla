/*
 * clawt-event-log.h - The durable record of what happened
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
#include "core/clawt-event-bus.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_EVENT_LOG (clawt_event_log_get_type())

G_DECLARE_FINAL_TYPE(ClawtEventLog, clawt_event_log, CLAWT, EVENT_LOG,
                     GObject)

/**
 * clawt_event_log_new:
 * @dir: directory to write NDJSON files into
 * @retention_days: how long to keep them; 0 keeps everything
 *
 * Returns: (transfer full): a new #ClawtEventLog
 */
ClawtEventLog *clawt_event_log_new(const gchar *dir, gint retention_days);

/**
 * clawt_event_log_attach:
 * @self: a #ClawtEventLog
 * @bus: (transfer none): the bus to record
 *
 * Records everything published on @bus from now on.
 */
void clawt_event_log_attach(ClawtEventLog *self, ClawtEventBus *bus);

/**
 * clawt_event_log_append:
 * @self: a #ClawtEventLog
 * @event: the event to record
 * @error: (out) (optional): return location for a #GError
 *
 * Appends one event.
 *
 * Returns: %TRUE if it was written
 */
gboolean clawt_event_log_append(ClawtEventLog  *self,
                                ClawtEvent     *event,
                                GError        **error);

/**
 * clawt_event_log_read:
 * @self: a #ClawtEventLog
 * @subject: (nullable): only events about this agent, room or task
 * @limit: how many of the most recent to return, or 0 for all
 *
 * Reads events back off disk.
 *
 * Returns: (transfer full) (element-type ClawtEvent): the events, oldest first
 */
GPtrArray *clawt_event_log_read(ClawtEventLog *self,
                                const gchar   *subject,
                                guint          limit);

/**
 * clawt_event_log_sweep:
 * @self: a #ClawtEventLog
 *
 * Deletes log files older than the retention period.
 *
 * Returns: how many files were removed
 */
guint clawt_event_log_sweep(ClawtEventLog *self);

G_END_DECLS
