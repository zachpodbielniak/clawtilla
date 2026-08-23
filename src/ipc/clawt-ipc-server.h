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
 * ClawtIpcPending:
 *
 * A reply a handler could not give straight away.
 *
 * A #ClawtIpcHandler runs on the daemon's main context while the client
 * blocks, so a handler that waits on the network stalls every other
 * client for as long as the far end takes -- the rule that moved the
 * model cache out of daemon start, met again from the other direction.
 * A handler with real work to do calls clawt_ipc_server_defer(), returns
 * %NULL, and answers when the work finishes.
 *
 * It holds a reference to the client, so a connection that closes while
 * the answer is in flight is still there to be answered into and simply
 * drops it.
 */
typedef struct _ClawtIpcPending ClawtIpcPending;

/**
 * clawt_ipc_server_defer: (skip)
 * @self: a #ClawtIpcServer
 * @request: the frame being answered
 *
 * Claims the right to answer @request later.
 *
 * Only valid from inside a #ClawtIpcHandler, which must then return
 * %NULL: the request's answer is whatever is passed to
 * clawt_ipc_pending_respond(), and a handler that both defers and returns
 * a frame would send two.
 *
 * Returns: (transfer full) (nullable): the token to answer with
 */
ClawtIpcPending *clawt_ipc_server_defer(ClawtIpcServer *self,
                                        JsonNode       *request);

/**
 * clawt_ipc_pending_respond: (skip)
 * @self: (transfer full): a #ClawtIpcPending
 * @response: (transfer full) (nullable): the frame to send
 *
 * Sends the answer and releases the token.
 *
 * Consumes both, so it cannot be called twice and there is nothing left
 * to leak on the paths that answer early.
 */
void clawt_ipc_pending_respond(ClawtIpcPending *self, JsonNode *response);

/**
 * clawt_ipc_pending_get_request: (skip)
 * @self: a #ClawtIpcPending
 *
 * The frame being answered, kept so the reply carries its id.
 *
 * Returns: (transfer none): the request
 */
JsonNode *clawt_ipc_pending_get_request(ClawtIpcPending *self);

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
 * clawt_ipc_server_set_token:
 * @self: a #ClawtIpcServer
 * @token: (nullable): shared secret a TCP client must present
 *
 * A listener is refused at start without one.  A unix socket is protected
 * by file permissions; a TCP port is protected by nothing at all, so
 * defaulting to open would turn one careless config line into a remote
 * shell.
 *
 * Unix clients never present it -- the kernel already vouches for them
 * through SO_PEERCRED.
 */
void clawt_ipc_server_set_token(ClawtIpcServer *self, const gchar *token);

/**
 * clawt_ipc_server_add_listener:
 * @self: a #ClawtIpcServer
 * @host: the address to bind
 * @port: the port to bind it on
 * @optional: whether failing to bind it is a warning rather than an error
 *
 * Adds a network address beside the unix socket.
 *
 * Each listener carries its own port, because the addresses a person
 * names have no reason to agree about which port they are on -- and a
 * single shared port is a limitation `clawtillad --bind` would have
 * inherited for no reason.
 *
 * @optional is for an address clawtilla chose rather than one anybody
 * asked for -- the tailnet address is the only such case today.  The unix
 * socket is the daemon's real interface and is already listening by the
 * time these are bound, so a stale process holding that port, or a
 * tailnet that came up twice, must not take the whole fleet down with it.
 * An address named in the configuration or on the command line is never
 * optional: somebody asked for it, so failing to provide it is an error.
 *
 * Adding the same address and port twice does nothing, because binding it
 * twice fails with EADDRINUSE against ourselves and reads as another
 * daemon already running.  Adding it again as non-optional does promote
 * it, so naming the tailnet address explicitly makes it mandatory.
 */
void clawt_ipc_server_add_listener(ClawtIpcServer *self,
                                   const gchar    *host,
                                   guint16         port,
                                   gboolean        optional);

/**
 * clawt_ipc_server_clear_listeners:
 * @self: a #ClawtIpcServer
 *
 * Drops every network address, leaving the unix socket alone.
 */
void clawt_ipc_server_clear_listeners(ClawtIpcServer *self);

/**
 * clawt_ipc_parse_listen_address:
 * @text: an address, optionally with `:port`
 * @default_port: the port to assume when @text does not name one
 * @out_host: (out) (transfer full): the address
 * @out_port: (out): the port
 * @error: (out) (optional): return location for a #GError
 *
 * Parses the `<ip>:<port>` form `clawtillad --bind` takes.
 *
 * IPv6 is why this is not a `strrchr(':')`: an IPv6 address is full of
 * colons, so one must be written `[fd7a::1]:8792` when it carries a port.
 * A bare `fd7a::1` is accepted as an address with @default_port, because
 * that is unambiguous -- it parses as an address whole.
 *
 * Only numeric addresses are accepted.  A name would have to be resolved,
 * which is a network round trip on the path that starts the daemon, and
 * it can resolve to an address on a different network than the one
 * intended.
 *
 * Returns: %TRUE if @text was understood
 */
gboolean clawt_ipc_parse_listen_address(const gchar  *text,
                                        guint16       default_port,
                                        gchar       **out_host,
                                        guint16      *out_port,
                                        GError      **error);

/**
 * clawt_ipc_server_is_listening_on:
 * @self: a #ClawtIpcServer
 * @host: an address that was added
 * @port: the port it was added on
 *
 * Whether that listener actually bound.
 *
 * An optional listener may not have, so anything telling a person where
 * the daemon can be reached has to ask this rather than repeat back what
 * it requested -- otherwise the daemon announces an address in one line
 * and warns that it could not bind it in the next.
 *
 * Returns: %TRUE if the server is listening there
 */
gboolean clawt_ipc_server_is_listening_on(ClawtIpcServer *self,
                                          const gchar    *host,
                                          guint16         port);

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
