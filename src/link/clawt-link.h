/*
 * clawt-link.h - One live connection to one agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon end of the connection an agent's clawtilla channel dials in
 * on.  Carries three kinds of traffic over one socket:
 *
 *   control.*  handshake and keepalives
 *   chat.*     messages to and from the agent
 *   mcp.*      the agent calling the orchestration tools
 *
 * Framing is one JSON envelope per line, using libreclaw's bridge protocol
 * codec.  Sharing the codec rather than inventing a second one means a
 * change to the envelope is made once.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_LINK (clawt_link_get_type())

G_DECLARE_FINAL_TYPE(ClawtLink, clawt_link, CLAWT, LINK, GObject)

/**
 * ClawtLinkMcpHandler:
 * @link: the #ClawtLink the request arrived on
 * @request: (transfer none): the MCP JSON-RPC request
 * @user_data: data passed when the handler was installed
 *
 * Called when an agent invokes an orchestration tool.
 *
 * Returns: (transfer full) (nullable): the JSON-RPC response, or %NULL to
 *   send no reply
 */
typedef JsonNode *(*ClawtLinkMcpHandler)(ClawtLink *link,
                                         JsonNode  *request,
                                         gpointer   user_data);

/**
 * clawt_link_new:
 * @connection: (transfer none): the accepted connection
 *
 * Wraps an accepted connection.  The link starts unidentified: it belongs
 * to no agent until a hello frame arrives and is accepted.
 *
 * Returns: (transfer full): a new #ClawtLink
 */
ClawtLink *clawt_link_new(GSocketConnection *connection);

/**
 * clawt_link_start:
 * @self: a #ClawtLink
 *
 * Begins reading.  Call once, after connecting to the signals -- frames
 * that arrived before a handler was attached would otherwise be dropped.
 */
void clawt_link_start(ClawtLink *self);

/**
 * clawt_link_close:
 * @self: a #ClawtLink
 * @reason: (nullable): why, sent to the agent before closing
 *
 * Closes the link, telling the agent first so it can distinguish a
 * deliberate close from the daemon crashing.
 */
void clawt_link_close(ClawtLink   *self,
                      const gchar *reason);

/**
 * clawt_link_get_agent_id:
 * @self: a #ClawtLink
 *
 * Returns: (transfer none) (nullable): the agent this link belongs to, or
 *   %NULL before the handshake
 */
const gchar *clawt_link_get_agent_id(ClawtLink *self);

/**
 * clawt_link_get_agent_name:
 * @self: a #ClawtLink
 *
 * Returns: (transfer none) (nullable): the agent's display name
 */
const gchar *clawt_link_get_agent_name(ClawtLink *self);

/**
 * clawt_link_is_open:
 * @self: a #ClawtLink
 *
 * Returns: %TRUE while the connection is usable
 */
gboolean clawt_link_is_open(ClawtLink *self);

/**
 * clawt_link_deliver:
 * @self: a #ClawtLink
 * @room_id: (nullable): the room the message is in
 * @sender_id: who sent it
 * @sender_name: (nullable): their display name
 * @body: the message
 * @thread_id: (nullable): thread to reply into
 * @error: (out) (optional): return location for a #GError
 *
 * Delivers a message to the agent.
 *
 * Returns: %TRUE if it reached the socket
 */
gboolean clawt_link_deliver(ClawtLink    *self,
                            const gchar  *room_id,
                            const gchar  *sender_id,
                            const gchar  *sender_name,
                            const gchar  *body,
                            const gchar  *thread_id,
                            GError      **error);

/**
 * clawt_link_send_welcome:
 * @self: a #ClawtLink
 * @error: (out) (optional): return location for a #GError
 *
 * Accepts the handshake.  Until this is sent the agent does not know
 * whether it was recognised.
 *
 * Returns: %TRUE if it reached the socket
 */
gboolean clawt_link_send_welcome(ClawtLink  *self,
                                 GError    **error);

/**
 * clawt_link_send_error:
 * @self: a #ClawtLink
 * @code: a numeric code
 * @message: a human-readable explanation
 *
 * Tells the agent something went wrong, without closing.
 */
void clawt_link_send_error(ClawtLink   *self,
                           gint         code,
                           const gchar *message);

/**
 * clawt_link_ping:
 * @self: a #ClawtLink
 *
 * Sends a keepalive.  An agent that stops answering is treated as gone.
 */
void clawt_link_ping(ClawtLink *self);

/**
 * clawt_link_seconds_since_seen:
 * @self: a #ClawtLink
 *
 * How long since anything arrived on this link.
 *
 * Returns: seconds since the last frame
 */
gint64 clawt_link_seconds_since_seen(ClawtLink *self);

/**
 * clawt_link_set_mcp_handler:
 * @self: a #ClawtLink
 * @handler: (nullable) (scope notified): called for mcp.request frames
 * @user_data: data for @handler
 * @destroy: (nullable): called when the handler is replaced or the link dies
 *
 * Installs the handler that serves the orchestration tools.
 */
void clawt_link_set_mcp_handler(ClawtLink           *self,
                                ClawtLinkMcpHandler  handler,
                                gpointer             user_data,
                                GDestroyNotify       destroy);

G_END_DECLS
