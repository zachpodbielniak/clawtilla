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

#include <gio/gio.h>
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
 * CLAWT_IPC_KEEPALIVE_IDLE_SECONDS:
 *
 * How long a network connection may be silent before the kernel starts
 * probing the peer.
 *
 * A conversation is idle most of the time -- somebody reading a reply is
 * sending nothing -- so this is what keeps a NAT or a tailnet from
 * forgetting the connection exists in the first place.
 */
#define CLAWT_IPC_KEEPALIVE_IDLE_SECONDS 30

/**
 * CLAWT_IPC_KEEPALIVE_INTERVAL_SECONDS:
 *
 * How long between probes once the peer has stopped answering them.
 */
#define CLAWT_IPC_KEEPALIVE_INTERVAL_SECONDS 10

/**
 * CLAWT_IPC_KEEPALIVE_COUNT:
 *
 * How many unanswered probes end the connection.
 *
 * With the two above, a peer that has gone is reported within about a
 * minute -- close to the link server's 120-second deadline for an agent,
 * and far short of the kernel's own two-hour default, which is long
 * enough that a person gives up on the fleet before the socket does.
 */
#define CLAWT_IPC_KEEPALIVE_COUNT 3

/**
 * clawt_ipc_socket_keepalive:
 * @socket: the socket a client connection is carried on
 * @error: (nullable): where a failure to configure the socket is put
 *
 * Arms TCP keepalive, so that a connection whose route has gone away is
 * reported as broken rather than believed in for ever.
 *
 * A unix socket cannot die quietly -- the peer's exit closes it and the
 * reader is told at once -- so this is a no-op for one, and a request for
 * a socket family we do not recognise is a no-op too rather than an
 * error.
 *
 * Returns: %TRUE if the socket needed nothing or was configured
 */
gboolean clawt_ipc_socket_keepalive(GSocket *socket, GError **error);

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
/**
 * clawt_ipc_payload_strv:
 * @payload: (nullable): a request payload
 * @key: the member to read
 *
 * Reads an array of strings.
 *
 * A command belongs in one of these rather than in a string: joining an
 * argv for transport and splitting it again at the far end loses exactly
 * the quoting the caller went to the trouble of writing.
 *
 * Returns: (transfer full) (nullable): the strings, or %NULL if there is
 *   no such member or it is not an array of them
 */
GStrv        clawt_ipc_payload_strv(JsonObject *payload, const gchar *key);

gboolean     clawt_ipc_payload_boolean(JsonObject  *payload,
                                       const gchar *key,
                                       gboolean     fallback);

/**
 * clawt_ipc_reply_refusal_text:
 * @payload: (nullable): the payload of a reply that may carry `refused`
 * @n_refused: (out) (optional): how many agents were named
 *
 * Every handler that rewrites the fleet's agent files answers with a
 * `refused` array -- one `{agent, message}` per agent clawtilla would not
 * render -- alongside whatever else it reports.  A refusal is a normal
 * outcome of a normal edit (an operator-typed `libreclaw:` block that
 * redeclares a section clawtilla renders itself), and the agent it names
 * is still running against the config.yaml it already had.
 *
 * One reader for every client, because the sentence a person is shown
 * about it should not depend on which client they opened.
 *
 * Returns: (transfer full) (nullable): the refusals as text, one agent a
 *   line and a closing sentence, or %NULL when nothing was refused
 */
gchar *clawt_ipc_reply_refusal_text(JsonNode *payload, guint *n_refused);

/**
 * clawt_ipc_reply_object:
 * @reply: (nullable): a reply payload as a client received it
 *
 * The reply's object, or %NULL.
 *
 * A node can hold the object *type* and no object --
 * json_node_new(JSON_NODE_OBJECT) builds exactly that, and every
 * payload-less reply once arrived as one -- so JSON_NODE_HOLDS_OBJECT()
 * is not a pointer check.  One reader, total over every #JsonNode, so a
 * client cannot get it right in one page and wrong in the next.
 *
 * Returns: (transfer none) (nullable): the object
 */
JsonObject *clawt_ipc_reply_object(JsonNode *reply);

/**
 * clawt_ipc_payload_new:
 * @first_key: (nullable): the first member name, or %NULL for `{}`
 * @...: its string value, then further name/value pairs, %NULL-terminated
 *
 * Builds a request payload from string pairs.
 *
 * A %NULL value drops the member rather than sending null: the daemon
 * treats absent and null differently in places, and a caller passing
 * %NULL almost always means "I do not have one".  Every value is a
 * string; the daemon's readers accept "true"/"false" for booleans for
 * exactly this reason (see clawt_ipc_payload_boolean()).
 *
 * Returns: (transfer full): a payload object
 */
JsonNode *clawt_ipc_payload_new(const gchar *first_key, ...)
    G_GNUC_NULL_TERMINATED;

/**
 * clawt_ipc_reply_toast_text:
 * @error: (nullable): the request's error, or %NULL if it succeeded
 * @reply: (nullable): the reply, when it succeeded
 *
 * What a client should say about a request somebody just made, or %NULL
 * for nothing.
 *
 * A failure is said -- except %CLAWT_ERROR_NOT_CONNECTED, which is not
 * an answer to anything somebody did but a condition the window is in,
 * held open by its connection banner: a window opened against a stopped
 * daemon makes a dozen requests before it draws, and a dozen copies of
 * one sentence stacked over the control that leads out of it helps
 * nobody.  A success is said only when it left an agent behind
 * (clawt_ipc_reply_refusal_text()).  Every graphical client applies this
 * to every reply, so it lives here rather than in three copies.
 *
 * Returns: (transfer full) (nullable): the text to show
 */
gchar *clawt_ipc_reply_toast_text(const GError *error, JsonNode *reply);

G_END_DECLS
