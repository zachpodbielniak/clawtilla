/*
 * clawt-ipc-proto.h - The frames clients and the daemon exchange
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

/**
 * CLAWT_IPC_VERSION:
 *
 * The protocol version carried in every frame.
 *
 * A client and a daemon from different installs meet over this socket
 * more often than you would think -- an upgraded package with the old
 * daemon still running, for one -- so the version is checked rather than
 * assumed.
 */
#define CLAWT_IPC_VERSION 1

/**
 * CLAWT_IPC_MAX_FRAME_BYTES:
 *
 * The largest frame that will be read.
 *
 * A bound is required, not optional: without one a peer that never sends
 * a newline makes the daemon buy memory until it is killed.
 */
#define CLAWT_IPC_MAX_FRAME_BYTES (8 * 1024 * 1024)

/**
 * clawt_ipc_request_new:
 * @kind: the request kind, e.g. `agent.list`
 * @id: a correlation id the reply repeats back
 *
 * Returns: (transfer full): a request frame with an empty payload
 */
JsonNode *clawt_ipc_request_new(const gchar *kind, const gchar *id);

/**
 * clawt_ipc_response_new:
 * @request: (nullable): the frame being answered
 * @payload: (transfer full) (nullable): the reply body
 *
 * Returns: (transfer full): a response frame
 */
JsonNode *clawt_ipc_response_new(JsonNode *request, JsonNode *payload);

/**
 * clawt_ipc_error_new:
 * @request: (nullable): the frame being answered
 * @code: a #ClawtErrorCode
 * @message: what went wrong, in words a person can act on
 *
 * Returns: (transfer full): an error frame
 */
JsonNode *clawt_ipc_error_new(JsonNode    *request,
                              gint         code,
                              const gchar *message);

/**
 * clawt_ipc_event_new:
 * @event: the event to broadcast
 *
 * Returns: (transfer full): an event frame
 */
JsonNode *clawt_ipc_event_new(ClawtEvent *event);

/**
 * clawt_ipc_frame_validate:
 * @frame: (nullable): a parsed frame
 * @error: (out) (optional): return location for why it was rejected
 *
 * Checks a frame is one this build understands.
 *
 * Returns: %TRUE if the frame may be acted on
 */
gboolean clawt_ipc_frame_validate(JsonNode *frame, GError **error);

const gchar *clawt_ipc_frame_get_kind(JsonNode *frame);
const gchar *clawt_ipc_frame_get_id(JsonNode *frame);

/**
 * clawt_ipc_frame_get_payload:
 * @frame: a frame
 *
 * Returns: (transfer none) (nullable): the payload object, or %NULL
 */
JsonObject *clawt_ipc_frame_get_payload(JsonNode *frame);

/**
 * clawt_ipc_frame_set_payload:
 * @frame: a frame
 * @payload: (transfer full): the payload
 */
void clawt_ipc_frame_set_payload(JsonNode *frame, JsonNode *payload);

/**
 * clawt_ipc_frame_is_error:
 * @frame: a frame
 *
 * Returns: %TRUE if this frame reports a failure
 */
gboolean clawt_ipc_frame_is_error(JsonNode *frame);

/**
 * clawt_ipc_frame_to_error:
 * @frame: an error frame
 *
 * Returns: (transfer full) (nullable): the failure as a #GError
 */
GError *clawt_ipc_frame_to_error(JsonNode *frame);

/**
 * clawt_ipc_frame_to_line:
 * @frame: a frame
 *
 * Serialises a frame for the wire, newline included.
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_ipc_frame_to_line(JsonNode *frame);

/**
 * clawt_ipc_frame_from_line:
 * @line: one line off the wire
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the frame, or %NULL if unusable
 */
JsonNode *clawt_ipc_frame_from_line(const gchar *line, GError **error);

/* Payload helpers, so callers do not repeat the null checks. */
const gchar *clawt_ipc_payload_string(JsonObject *payload, const gchar *key);
gint64       clawt_ipc_payload_int(JsonObject  *payload,
                                   const gchar *key,
                                   gint64       fallback);
gboolean     clawt_ipc_payload_boolean(JsonObject  *payload,
                                       const gchar *key,
                                       gboolean     fallback);

G_END_DECLS
