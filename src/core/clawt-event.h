/*
 * clawt-event.h - Something that happened in the fleet
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
#include <json-glib/json-glib.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_EVENT (clawt_event_get_type())

GType clawt_event_get_type(void) G_GNUC_CONST;

/**
 * clawt_event_new:
 * @kind: a dotted kind, e.g. `agent.state-changed`
 * @subject: (nullable): what it happened to -- an agent id, room id or task id
 *
 * Creates an event.
 *
 * Returns: (transfer full): a new #ClawtEvent
 */
ClawtEvent *clawt_event_new(const gchar *kind, const gchar *subject);

ClawtEvent *clawt_event_copy(ClawtEvent *self);
void        clawt_event_free(ClawtEvent *self);

const gchar *clawt_event_get_kind(ClawtEvent *self);
const gchar *clawt_event_get_subject(ClawtEvent *self);
gint64       clawt_event_get_timestamp(ClawtEvent *self);

/**
 * clawt_event_set_timestamp:
 * @self: a #ClawtEvent
 * @timestamp: microseconds since the epoch
 *
 * Overrides when the event happened.  Used when reading an event back off
 * disk, so a replayed event keeps its original time rather than claiming
 * to have happened when it was read.
 */
void clawt_event_set_timestamp(ClawtEvent *self, gint64 timestamp);

/**
 * clawt_event_get_cursor:
 * @self: a #ClawtEvent
 *
 * The event's position in the stream.
 *
 * A client that reconnects asks to resume from the last cursor it saw.
 * Sequence numbers rather than timestamps, because two events in the same
 * microsecond are ordinary and a client must not have to guess which of
 * them it already has.
 *
 * Returns: the sequence number, or 0 if it has not been published
 */
guint64 clawt_event_get_cursor(ClawtEvent *self);

void clawt_event_set_cursor(ClawtEvent *self, guint64 cursor);

/**
 * clawt_event_set_detail:
 * @self: a #ClawtEvent
 * @key: field name
 * @value: (nullable): field value
 *
 * Attaches a string field.
 */
void clawt_event_set_detail(ClawtEvent  *self,
                            const gchar *key,
                            const gchar *value);

void clawt_event_set_detail_int(ClawtEvent  *self,
                                const gchar *key,
                                gint64       value);

const gchar *clawt_event_get_detail(ClawtEvent *self, const gchar *key);

/**
 * clawt_event_get_detail_int:
 * @self: a #ClawtEvent
 * @key: a detail set with clawt_event_set_detail_int()
 *
 * Details travel as strings, so this is the counterpart that turns one
 * back -- without it every caller writes the same g_ascii_strtoll().
 *
 * Returns: the value, or 0 when there is no such detail or it is not a
 *   number
 */
gint64 clawt_event_get_detail_int(ClawtEvent *self, const gchar *key);

/**
 * clawt_event_to_json:
 * @self: a #ClawtEvent
 *
 * Renders the event for the wire and for the event log.
 *
 * Returns: (transfer full): a JSON object node
 */
JsonNode *clawt_event_to_json(ClawtEvent *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtEvent, clawt_event_free)

G_END_DECLS
