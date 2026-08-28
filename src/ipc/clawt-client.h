/*
 * clawt-client.h - Talking to the daemon
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

#define CLAWT_TYPE_CLIENT (clawt_client_get_type())

G_DECLARE_FINAL_TYPE(ClawtClient, clawt_client, CLAWT, CLIENT, GObject)

/**
 * clawt_client_new:
 * @socket_path: (nullable): the daemon's socket, or %NULL for the default
 *
 * The one object every client -- CLI, GTK, web -- uses to reach the
 * daemon.
 *
 * Returns: (transfer full): a new #ClawtClient
 */
ClawtClient *clawt_client_new(const gchar *socket_path);

/**
 * clawt_client_new_tcp:
 * @host: the daemon's host
 * @port: its port
 * @token: the shared secret
 *
 * Returns: (transfer full): a new #ClawtClient
 */
ClawtClient *clawt_client_new_tcp(const gchar *host,
                                  guint16      port,
                                  const gchar *token);

/**
 * clawt_client_set_tls:
 * @self: a #ClawtClient
 * @enabled: whether to wrap the TCP connection in TLS
 * @accept_unknown_certificate: accept a certificate that does not validate
 *
 * @accept_unknown_certificate exists for a self-signed daemon certificate
 * on a machine you control.  It disables the check that would otherwise
 * notice someone else answering on that address, so it is off by default
 * and worth saying out loud when it is on.
 */
void clawt_client_set_tls(ClawtClient *self,
                          gboolean     enabled,
                          gboolean     accept_unknown_certificate);

/**
 * clawt_client_connect:
 * @self: a #ClawtClient
 * @error: (out) (optional): return location for a #GError
 *
 * Connects and exchanges `control.hello`.
 *
 * Returns: %TRUE if connected
 */
gboolean clawt_client_connect(ClawtClient *self, GError **error);

void     clawt_client_disconnect(ClawtClient *self);
gboolean clawt_client_is_connected(ClawtClient *self);

/**
 * clawt_client_set_auto_reconnect:
 * @self: a #ClawtClient
 * @enabled: whether to reconnect after the daemon goes away
 *
 * A long-lived client -- the GTK app, an editor -- should have this on:
 * the daemon restarts whenever its config changes, and a client that gave
 * up on the first drop would sit there showing stale state.  A CLI run
 * should not: it has one thing to do and should report the failure.
 */
void clawt_client_set_auto_reconnect(ClawtClient *self, gboolean enabled);

/**
 * clawt_client_is_reconnecting:
 * @self: a #ClawtClient
 *
 * Whether this client has lost its daemon and is trying to get it back.
 *
 * Distinct from clawt_client_is_connected() being %FALSE, which is also
 * true of a client nobody has connected yet.  A client that is
 * reconnecting is one somebody was using a moment ago, and that is the
 * state worth drawing: auto-reconnect is right, but silence while it
 * happens is indistinguishable from a fleet that has simply gone quiet.
 *
 * Returns: %TRUE while a retry is scheduled
 */
gboolean clawt_client_is_reconnecting(ClawtClient *self);

/**
 * clawt_client_start_reconnecting:
 * @self: a #ClawtClient
 *
 * Starts the retry loop for a client that has never connected.
 *
 * Auto-reconnect is armed by a connection *going away*, so a client
 * whose very first clawt_client_connect() failed was inert for ever:
 * starting the daemon a moment later changed nothing, and the only way
 * back was to close the application and open it again.  A daemon that is
 * not up yet is the ordinary case for a desktop client launched from a
 * menu, so it is the case that has to work.
 *
 * Does nothing when the client is connected, when a retry is already
 * scheduled, or when clawt_client_set_auto_reconnect() has not been
 * turned on -- a caller with one thing to do wants the failure reported,
 * not retried behind its back.
 */
void clawt_client_start_reconnecting(ClawtClient *self);

/**
 * clawt_client_request:
 * @self: a #ClawtClient
 * @kind: the request kind
 * @payload: (transfer full) (nullable): the request body
 * @error: (out) (optional): return location for a #GError
 *
 * Sends a request and waits for its reply.
 *
 * Events that arrive while waiting are dispatched as usual rather than
 * dropped, so a synchronous call does not lose the caller's subscription.
 *
 * Returns: (transfer full) (nullable): the reply payload, or %NULL on error
 */
/**
 * clawt_client_request_full:
 * @self: a #ClawtClient
 * @kind: the frame kind
 * @payload: (nullable) (transfer full): the request payload
 * @timeout_seconds: how long to wait, or 0 for the default
 * @error: (out) (optional): return location for a #GError
 *
 * clawt_client_request() with the waiting time named.
 *
 * The default suits a request the daemon answers out of what it already
 * knows.  It does not suit one that waits on a person: authorising a
 * connector takes as long as somebody takes to pick up a phone and type
 * a code, and a client that gave up after the default would report a
 * timeout for a flow that was about to succeed -- and then leave the
 * daemon holding a credential nobody was told about.
 *
 * Returns: (transfer full) (nullable): the reply payload, or %NULL
 */
JsonNode *clawt_client_request_full(ClawtClient  *self,
                                    const gchar  *kind,
                                    JsonNode     *payload,
                                    gint          timeout_seconds,
                                    GError      **error);

JsonNode *clawt_client_request(ClawtClient  *self,
                               const gchar  *kind,
                               JsonNode     *payload,
                               GError      **error);

/**
 * clawt_client_request_async:
 * @self: a #ClawtClient
 * @kind: the request kind
 * @payload: (transfer full) (nullable): the request body
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the reply arrives
 * @user_data: data for @callback
 *
 * Sends a request without blocking.
 */
void clawt_client_request_async(ClawtClient         *self,
                                const gchar         *kind,
                                JsonNode            *payload,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data);

/**
 * clawt_client_request_finish:
 * @self: a #ClawtClient
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the reply payload
 */
JsonNode *clawt_client_request_finish(ClawtClient   *self,
                                      GAsyncResult  *result,
                                      GError       **error);

/**
 * clawt_client_subscribe:
 * @self: a #ClawtClient
 * @cursor: the last cursor seen, or 0 for everything the daemon still holds
 * @out_resumed: (out) (optional): %FALSE if the daemon could not replay
 *   from @cursor, so this client must re-fetch rather than assume it is
 *   up to date
 * @error: (out) (optional): return location for a #GError
 *
 * Subscribes to the event stream.
 *
 * Returns: %TRUE if subscribed
 */
gboolean clawt_client_subscribe(ClawtClient  *self,
                                guint64       cursor,
                                gboolean     *out_resumed,
                                GError      **error);

/**
 * clawt_client_get_cursor:
 * @self: a #ClawtClient
 *
 * Returns: the most recent cursor this client has seen
 */
guint64 clawt_client_get_cursor(ClawtClient *self);

/**
 * clawt_client_default_socket_path:
 *
 * Returns: (transfer full): where the daemon listens unless told otherwise
 */
gchar *clawt_client_default_socket_path(void);

G_END_DECLS
