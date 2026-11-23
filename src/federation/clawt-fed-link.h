/*
 * clawt-fed-link.h - Talking to one other fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A link sends signed envelopes to one fleet and checks every answer is
 * signed by that fleet, bound to the request it answers.  Which pipe it
 * uses comes from the endpoint:
 *
 *   https:// http://   POST /fed/v1/envelope on the federation listener
 *   clawtilla://        the fleet's own IPC port: `fed.hello`, then
 *                       `fed.envelope` frames on one kept connection
 *   unix:/path          either, on this machine (the listener's socket
 *                       by default; `unix:/path#ipc` for an IPC socket)
 *
 * Every request is asynchronous and runs on the context the link was
 * made with.  Nothing here blocks the daemon's loop.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include "federation/clawt-fed-envelope.h"
#include "federation/clawt-fed-registry.h"
#include "federation/clawt-fed-transport.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_FED_LINK (clawt_fed_link_get_type())
G_DECLARE_FINAL_TYPE(ClawtFedLink, clawt_fed_link, CLAWT, FED_LINK, GObject)

/**
 * clawt_fed_link_new:
 * @key: our fleet key
 * @key_index: the index peers know @key by
 * @our_fleet: our fleet id
 * @endpoint: where the other fleet is
 * @context: (nullable): the context requests and their callbacks run on
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): a link, or %NULL when @endpoint is
 *   not one
 */
ClawtFedLink *clawt_fed_link_new(ClawtFleetKey  *key,
                                 guint           key_index,
                                 const gchar    *our_fleet,
                                 const gchar    *endpoint,
                                 GMainContext   *context,
                                 GError        **error);

/**
 * clawt_fed_link_set_peer:
 * @self: a #ClawtFedLink
 * @peer: (nullable): the peer's record, copied: its fleet id, its keys and
 *   its seal key
 *
 * Who answers must be.  Without one only clawt_fed_link_exchange_async()
 * works, for the invite exchange that learns it.
 */
void clawt_fed_link_set_peer(ClawtFedLink       *self,
                             const ClawtFedPeer *peer);

/**
 * clawt_fed_link_set_seal:
 * @self: a #ClawtFedLink
 * @seal: whether request bodies are sealed
 *
 * Needs a peer with a seal key.  Answers are opened either way.
 */
void clawt_fed_link_set_seal(ClawtFedLink *self,
                             gboolean      seal);

/**
 * clawt_fed_link_set_access:
 * @self: a #ClawtFedLink
 * @id: (nullable): a Cloudflare Access service token id
 * @secret: (nullable): its secret
 *
 * Sent as `CF-Access-Client-Id`/`-Secret` on every HTTPS request, for a
 * peer whose tunnel sits behind Access.
 */
void clawt_fed_link_set_access(ClawtFedLink *self,
                               const gchar  *id,
                               const gchar  *secret);

/**
 * clawt_fed_link_set_tls_pin:
 * @self: a #ClawtFedLink
 * @pin: (nullable): SHA-256 of the peer's IPC certificate, hex
 *
 * For a `clawtilla://` link: speak TLS and accept that certificate only.
 */
void clawt_fed_link_set_tls_pin(ClawtFedLink *self,
                                const gchar  *pin);

/**
 * clawt_fed_link_get_endpoint:
 * @self: a #ClawtFedLink
 *
 * Returns: (transfer none): the endpoint
 */
const gchar *clawt_fed_link_get_endpoint(ClawtFedLink *self);

/**
 * clawt_fed_link_get_transport:
 * @self: a #ClawtFedLink
 *
 * Returns: HTTP or IPC
 */
ClawtFedTransport clawt_fed_link_get_transport(ClawtFedLink *self);

/**
 * clawt_fed_link_is_connected:
 * @self: a #ClawtFedLink
 *
 * Returns: for an IPC link, whether its connection is up; for HTTP,
 *   whether the last request reached the peer
 */
gboolean clawt_fed_link_is_connected(ClawtFedLink *self);

/**
 * ClawtFedExchange:
 * @status: the HTTP status, or 200/4xx mapped from an IPC reply
 * @stamp: (nullable): the answer's stamp, unchecked
 * @body: the answer's body, as received (sealed or not)
 * @request_nonce: the nonce the request was signed with
 *
 * A raw answer, before it is checked.
 */
typedef struct {
    guint          status;
    ClawtFedStamp *stamp;
    gchar         *body;
    gchar         *request_nonce;
} ClawtFedExchange;

GType clawt_fed_exchange_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_FED_EXCHANGE (clawt_fed_exchange_get_type())

/**
 * clawt_fed_exchange_copy:
 * @exchange: a #ClawtFedExchange
 *
 * Returns: (transfer full): a deep copy
 */
ClawtFedExchange *clawt_fed_exchange_copy(const ClawtFedExchange *exchange);

/**
 * clawt_fed_exchange_free:
 * @exchange: (transfer full) (nullable): a #ClawtFedExchange
 */
void clawt_fed_exchange_free(ClawtFedExchange *exchange);

/**
 * clawt_fed_link_exchange_async:
 * @self: a #ClawtFedLink
 * @route_binding: `POST /fed/v1/invite/accept` or `POST /fed/v1/envelope`
 * @kind: the envelope's kind
 * @to: (nullable): the receiving fleet id
 * @body: the body, JSON text, sent as given (never sealed)
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the answer
 * @user_data: closure
 *
 * Sends and returns the answer unchecked, for a caller that learns the
 * key to check it with from the answer itself -- the invite exchange.
 * Everything else uses clawt_fed_link_request_async().
 */
void clawt_fed_link_exchange_async(ClawtFedLink        *self,
                                   const gchar         *route_binding,
                                   const gchar         *kind,
                                   const gchar         *to,
                                   const gchar         *body,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);

/**
 * clawt_fed_link_exchange_finish:
 * @self: a #ClawtFedLink
 * @result: the result
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL when nothing
 *   came back (%CLAWT_ERROR_NOT_CONNECTED for an unreachable fleet)
 */
ClawtFedExchange *clawt_fed_link_exchange_finish(ClawtFedLink  *self,
                                                 GAsyncResult  *result,
                                                 GError       **error);

/**
 * clawt_fed_link_request_async:
 * @self: a #ClawtFedLink with a peer
 * @kind: the envelope's kind
 * @body: (nullable): the body, JSON text
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the answer
 * @user_data: closure
 *
 * Sends a signed (and, when set, sealed) envelope and checks the answer:
 * signed by the peer, addressed to us, bound to this request, within the
 * clock window.  An answer that fails any of those is an error, never a
 * reply.
 */
void clawt_fed_link_request_async(ClawtFedLink        *self,
                                  const gchar         *kind,
                                  const gchar         *body,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);

/**
 * clawt_fed_link_request_finish:
 * @self: a #ClawtFedLink
 * @result: the result
 * @error: (out) (optional): return location for a #GError --
 *   %CLAWT_ERROR_NOT_CONNECTED when the fleet could not be reached,
 *   %CLAWT_ERROR_AUTH when the answer did not check out, and the peer's
 *   own code and words when it refused
 *
 * Returns: (transfer full) (nullable): the answer's body, opened
 */
gchar *clawt_fed_link_request_finish(ClawtFedLink  *self,
                                     GAsyncResult  *result,
                                     GError       **error);

/**
 * clawt_fed_link_close:
 * @self: a #ClawtFedLink
 *
 * Drops any kept connection.  Requests in flight fail.
 */
void clawt_fed_link_close(ClawtFedLink *self);

/**
 * clawt_fed_answer_error:
 * @status: an HTTP status
 * @body: (nullable): an answer body
 *
 * The error a refusal answer carries: `{"error": "...", "code": "..."}`.
 *
 * Returns: (transfer full): a #GError in %CLAWT_ERROR
 */
GError *clawt_fed_answer_error(guint        status,
                               const gchar *body);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFedExchange, clawt_fed_exchange_free)

G_END_DECLS
