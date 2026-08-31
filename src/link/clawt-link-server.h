/*
 * clawt-link-server.h - Accepts agents dialling in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Listens on the unix socket agents connect to, and hands each accepted
 * connection to a #ClawtLink.  Authentication happens here: a link is
 * anonymous until a hello frame names an agent and presents its token, and
 * the daemon decides whether to accept it.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "clawt-types.h"
#include "link/clawt-link.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_LINK_SERVER (clawt_link_server_get_type())

G_DECLARE_FINAL_TYPE(ClawtLinkServer, clawt_link_server,
                     CLAWT, LINK_SERVER, GObject)

/**
 * ClawtLinkAuthFunc:
 * @agent_id: the agent claiming the link
 * @token: (nullable): the token it presented
 * @user_data: data passed when the callback was installed
 *
 * Decides whether a connection may claim @agent_id.
 *
 * Returns: %TRUE to accept
 */
typedef gboolean (*ClawtLinkAuthFunc)(const gchar *agent_id,
                                      const gchar *token,
                                      gpointer     user_data);

/**
 * clawt_link_server_new:
 * @socket_path: where to listen
 *
 * Returns: (transfer full): a new #ClawtLinkServer
 */
ClawtLinkServer *clawt_link_server_new(const gchar *socket_path);

/**
 * clawt_link_server_set_auth_func:
 * @self: a #ClawtLinkServer
 * @func: (nullable) (scope notified): called to authorise each handshake
 * @user_data: data for @func
 * @destroy: (nullable): called when @func is replaced
 *
 * Installs the authorisation callback.
 *
 * Without one every handshake is accepted, which is only safe because the
 * socket is already restricted by file permissions -- but it would let any
 * agent on the machine claim another's identity and read its mail.
 */
void clawt_link_server_set_auth_func(ClawtLinkServer   *self,
                                     ClawtLinkAuthFunc  func,
                                     gpointer           user_data,
                                     GDestroyNotify     destroy);

/**
 * clawt_link_server_start:
 * @self: a #ClawtLinkServer
 * @error: (out) (optional): return location for a #GError
 *
 * Starts listening.
 *
 * A stale socket left by a daemon that did not shut down cleanly is
 * removed first, but only after checking that nothing is listening on it:
 * unlinking a live socket would silently steal a running daemon's agents.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_link_server_start(ClawtLinkServer  *self,
                                 GError          **error);

/**
 * clawt_link_server_stop:
 * @self: a #ClawtLinkServer
 *
 * Stops listening and closes every link.
 */
void clawt_link_server_stop(ClawtLinkServer *self);

/**
 * clawt_link_server_get_link:
 * @self: a #ClawtLinkServer
 * @agent_id: an agent id
 *
 * Returns: (transfer none) (nullable): that agent's link, or %NULL
 */
ClawtLink *clawt_link_server_get_link(ClawtLinkServer *self,
                                      const gchar     *agent_id);

/**
 * clawt_link_server_get_socket_path:
 * @self: a #ClawtLinkServer
 *
 * Returns: (transfer none): the path being listened on
 */
const gchar *clawt_link_server_get_socket_path(ClawtLinkServer *self);

/**
 * clawt_link_server_count_links:
 * @self: a #ClawtLinkServer
 *
 * Returns: how many agents are currently connected
 */
guint clawt_link_server_count_links(ClawtLinkServer *self);

/**
 * clawt_link_server_count_evictions:
 * @self: a #ClawtLinkServer
 * @agent_id: the agent to ask about
 *
 * How many times a live link for @agent_id has been displaced by a newer
 * connection claiming the same id, within the contest window.
 *
 * One is an ordinary reconnect.  A run of them is two processes serving
 * one agent id and taking the link from each other, which delivers that
 * agent's messages to whichever happens to hold it at the time.  The
 * count is reset when the agent's link closes without being displaced.
 *
 * Returns: the eviction count currently held against @agent_id
 */
guint clawt_link_server_count_evictions(ClawtLinkServer *self,
                                        const gchar     *agent_id);

/**
 * clawt_link_server_is_contested:
 * @self: a #ClawtLinkServer
 * @agent_id: the agent to ask about
 *
 * Whether @agent_id has been displaced often enough, and fast enough, to
 * be treated as contested rather than reconnecting.  While it is, further
 * connections claiming that id are refused instead of taking the link.
 *
 * Returns: %TRUE when the id is fenced
 */
gboolean clawt_link_server_is_contested(ClawtLinkServer *self,
                                        const gchar     *agent_id);

G_END_DECLS
