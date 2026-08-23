/*
 * clawt-ipc-server.h - Where clients connect
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
#include "core/clawt-event-bus.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_IPC_SERVER (clawt_ipc_server_get_type())

G_DECLARE_FINAL_TYPE(ClawtIpcServer, clawt_ipc_server, CLAWT, IPC_SERVER,
                     GObject)

/**
 * ClawtIpcHandler:
 * @request: the frame a client sent
 * @user_data: data passed to clawt_ipc_server_set_handler()
 *
 * Answers one request.
 *
 * Returns: (transfer full): the frame to send back
 */
typedef JsonNode *(*ClawtIpcHandler)(JsonNode *request, gpointer user_data);

/**
 * clawt_ipc_server_new:
 * @socket_path: the unix socket to listen on
 *
 * Returns: (transfer full): a new #ClawtIpcServer
 */
ClawtIpcServer *clawt_ipc_server_new(const gchar *socket_path);

/**
 * clawt_ipc_server_set_handler:
 * @self: a #ClawtIpcServer
 * @handler: (scope notified): called for every request
 * @user_data: data for @handler
 * @destroy: (nullable): called when the handler is replaced
 */
void clawt_ipc_server_set_handler(ClawtIpcServer  *self,
                                  ClawtIpcHandler  handler,
                                  gpointer         user_data,
                                  GDestroyNotify   destroy);

/**
 * clawt_ipc_server_set_tcp:
 * @self: a #ClawtIpcServer
 * @address: (nullable): address to bind, or %NULL for none
 * @port: the port
 * @token: (nullable): shared secret a TCP client must present
 *
 * Adds an optional TCP listener beside the unix socket.
 *
 * Off by default, and refused without a token: a unix socket is protected
 * by file permissions, and a TCP port is protected by nothing at all.
 */
void clawt_ipc_server_set_tcp(ClawtIpcServer *self,
                              const gchar    *address,
                              guint16         port,
                              const gchar    *token);

/**
 * clawt_ipc_server_add_tcp_address:
 * @self: a #ClawtIpcServer
 * @address: another address to bind, at the same port
 *
 * Adds an address beside the one clawt_ipc_server_set_tcp() named.
 *
 * This is how the tailnet address is bound without displacing whatever
 * `daemon.tcp_address` says: the two are separate decisions, and a
 * machine that is on a tailnet usually wants that address and no other.
 * Adding an address already listed does nothing, because binding it
 * twice fails with EADDRINUSE against ourselves and reads as another
 * daemon already running.
 *
 * The token set by clawt_ipc_server_set_tcp() applies here too -- an
 * address added without one is refused at start, exactly as the first
 * one is.
 */
void clawt_ipc_server_add_tcp_address(ClawtIpcServer *self,
                                      const gchar    *address);

/**
 * clawt_ipc_server_set_optional_tcp_address:
 * @self: a #ClawtIpcServer
 * @address: another address to bind, at the same port
 *
 * Adds an address whose bind failing is a warning rather than a refusal
 * to start.
 *
 * The tailnet address is bound this way.  It is a convenience the daemon
 * offers rather than an interface anyone configured by name, and the
 * unix socket -- the daemon's real interface -- is already listening by
 * the time this is reached.  A fleet that would not start because a
 * stale process still held that port, or because tailscaled restarted
 * between the address lookup and the bind, is a much worse failure than
 * not being reachable from a laptop.
 *
 * An address named in the configuration is never optional: someone asked
 * for it, so failing to provide it is an error rather than a footnote.
 */
void clawt_ipc_server_set_optional_tcp_address(ClawtIpcServer *self,
                                               const gchar    *address);

/**
 * clawt_ipc_server_is_listening_on:
 * @self: a #ClawtIpcServer
 * @address: an address that was added
 *
 * Whether @address actually bound.
 *
 * An optional address may not have, so anything telling a person where
 * the daemon can be reached has to ask this rather than repeat back what
 * it requested -- otherwise the daemon announces an address in one line
 * and warns that it could not bind it in the next.
 *
 * Returns: %TRUE if the server is listening on @address
 */
gboolean clawt_ipc_server_is_listening_on(ClawtIpcServer *self,
                                          const gchar    *address);

/**
 * clawt_ipc_server_set_tls:
 * @self: a #ClawtIpcServer
 * @certificate_path: (nullable): PEM certificate
 * @key_path: (nullable): PEM private key
 *
 * Wraps the TCP listener in TLS.
 */
void clawt_ipc_server_set_tls(ClawtIpcServer *self,
                              const gchar    *certificate_path,
                              const gchar    *key_path);

/**
 * clawt_ipc_server_attach_bus:
 * @self: a #ClawtIpcServer
 * @bus: (transfer none): the event bus to broadcast
 *
 * Sends every published event to every subscribed client.
 */
void clawt_ipc_server_attach_bus(ClawtIpcServer *self, ClawtEventBus *bus);

/**
 * clawt_ipc_server_start:
 * @self: a #ClawtIpcServer
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the server is listening
 */
gboolean clawt_ipc_server_start(ClawtIpcServer *self, GError **error);

void clawt_ipc_server_stop(ClawtIpcServer *self);

/**
 * clawt_ipc_server_count_clients:
 * @self: a #ClawtIpcServer
 *
 * Returns: how many clients are connected
 */
guint clawt_ipc_server_count_clients(ClawtIpcServer *self);

const gchar *clawt_ipc_server_get_socket_path(ClawtIpcServer *self);

G_END_DECLS
