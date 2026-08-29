/*
 * clawt-webhook-ingress.h - The one door a forge may knock on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Its own #SoupServer on its own port, serving exactly two paths:
 *
 *   GET  /health          so a tunnel or a monitor can see it is up
 *   POST /hooks/<endpoint> a delivery
 *
 * Nothing else.  Not the IPC surface, not a static route, not a
 * directory listing -- so putting this behind a tunnel exposes the
 * ability to deliver an event to a trigger somebody registered, and
 * nothing else about the machine.
 *
 * It binds where `clawtilla-web` binds: the loopback, plus the tailnet
 * address when there is one.  A machine with no tailnet gets the
 * loopback alone rather than every interface -- widening the audience
 * because an address was missing is the opposite of what somebody would
 * want, and this is the listener where it would matter most.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "trigger/clawt-trigger-provider.h"

G_BEGIN_DECLS

/**
 * ClawtWebhookRoute:
 * @CLAWT_WEBHOOK_ROUTE_HEALTH: `GET /health`
 * @CLAWT_WEBHOOK_ROUTE_DELIVERY: `POST /hooks/<endpoint>`
 * @CLAWT_WEBHOOK_ROUTE_WRONG_METHOD: the right path, the wrong verb
 * @CLAWT_WEBHOOK_ROUTE_NONE: anything else
 *
 * What a request is, before anything is read from it.
 */
typedef enum {
    CLAWT_WEBHOOK_ROUTE_NONE = 0,
    CLAWT_WEBHOOK_ROUTE_HEALTH,
    CLAWT_WEBHOOK_ROUTE_DELIVERY,
    CLAWT_WEBHOOK_ROUTE_WRONG_METHOD
} ClawtWebhookRoute;

/**
 * clawt_webhook_route:
 * @method: the HTTP method
 * @path: the request path
 * @out_endpoint: (out) (optional) (transfer none) (nullable): where the
 *   endpoint id begins inside @path, for a delivery
 *
 * Decides what a request is, from the path and method alone.
 *
 * Separated from the server so that "what does this listener serve" is
 * answerable without opening a port -- which is the one question about
 * this file worth being sure of, and the one a test that needs a socket
 * would have to be skipped for.
 *
 * Exactly two things are served. Anything else is
 * %CLAWT_WEBHOOK_ROUTE_NONE, including a path *under* an endpoint: a
 * `/hooks/<id>/anything` that reached the lookup would hand it a shape
 * nothing downstream expects.
 *
 * Returns: what the request is
 */
ClawtWebhookRoute clawt_webhook_route(const gchar  *method,
                                      const gchar  *path,
                                      const gchar **out_endpoint);

/**
 * ClawtWebhookDeliverFunc:
 * @endpoint: the endpoint id from the path
 * @headers: (element-type utf8 utf8): the request's headers, names lowercased
 * @presented: (nullable): a secret carried in the URL's `token` parameter
 * @body: (array length=body_length): the raw body, exactly as it arrived
 * @body_length: how many bytes of @body
 * @user_data: data passed to clawt_webhook_ingress_set_deliver_func()
 * @out_status: (out): the HTTP status to answer with
 *
 * Handles one delivery.
 *
 * The ingress knows about sockets and nothing about triggers, which is
 * what makes "does this endpoint authenticate" answerable without
 * opening a port. Everything the callback needs is here: the raw bytes,
 * because a re-serialised body has a different digest, and the headers,
 * because that is where every forge puts its signature.
 *
 * @presented is the other half of that -- a sender that cannot set a
 * header at all. podomation's webhook module is the worked example: its
 * `post()` takes a URL and nothing else, so a recipe that assumed a
 * bearer token or an HMAC would authenticate nothing. Whether it is
 * *accepted* is not the ingress's decision; it hands it on and
 * clawt_trigger_verify_url_secret() answers, because only the trigger
 * knows which provider it named and a forge must go on proving it can
 * sign.
 *
 * Returns: (transfer full) (nullable): a short body for the response
 */
typedef gchar *(*ClawtWebhookDeliverFunc)(const gchar  *endpoint,
                                          GHashTable   *headers,
                                          const gchar  *presented,
                                          const guchar *body,
                                          gsize         body_length,
                                          gpointer      user_data,
                                          guint        *out_status);

#define CLAWT_TYPE_WEBHOOK_INGRESS (clawt_webhook_ingress_get_type())

G_DECLARE_FINAL_TYPE(ClawtWebhookIngress, clawt_webhook_ingress,
                     CLAWT, WEBHOOK_INGRESS, GObject)

/**
 * clawt_webhook_ingress_new:
 * @port: the port to listen on
 * @max_body_bytes: the largest delivery that will be read
 *
 * Returns: (transfer full): a new #ClawtWebhookIngress, not yet listening
 */
ClawtWebhookIngress *clawt_webhook_ingress_new(guint16 port,
                                               gsize   max_body_bytes);

/**
 * clawt_webhook_ingress_set_deliver_func:
 * @self: a #ClawtWebhookIngress
 * @func: (scope forever): what to do with a delivery
 * @user_data: data for @func
 *
 * Set once, by the daemon, and never replaced -- hence `forever`.
 */
void clawt_webhook_ingress_set_deliver_func(ClawtWebhookIngress     *self,
                                            ClawtWebhookDeliverFunc  func,
                                            gpointer                 user_data);

/**
 * clawt_webhook_ingress_start:
 * @self: a #ClawtWebhookIngress
 * @tailnet: whether to also bind this machine's tailnet address
 * @error: (out) (optional): return location for a #GError
 *
 * Binds and begins listening.
 *
 * The loopback is an address somebody asked for by turning the receiver
 * on, so failing to bind it is an error. The tailnet address is one
 * clawtilla chose, so failing to bind that is a warning: somebody else
 * holding the port on the tailnet is a reason not to be reachable from a
 * laptop, and not a reason to refuse to run at all.
 *
 * Returns: %TRUE if anything is listening
 */
gboolean clawt_webhook_ingress_start(ClawtWebhookIngress  *self,
                                     gboolean              tailnet,
                                     GError              **error);

void clawt_webhook_ingress_stop(ClawtWebhookIngress *self);

/**
 * clawt_webhook_ingress_get_addresses:
 * @self: a #ClawtWebhookIngress
 *
 * What it is actually listening on, rather than what it was asked for.
 *
 * A convenience address whose bind failed is exactly the interesting
 * case, and reporting the request would say it was reachable there.
 *
 * Returns: (transfer none) (element-type utf8): the addresses
 */
GPtrArray *clawt_webhook_ingress_get_addresses(ClawtWebhookIngress *self);

/**
 * clawt_webhook_ingress_get_port:
 * @self: a #ClawtWebhookIngress
 *
 * Returns: the port it listens on
 */
guint16 clawt_webhook_ingress_get_port(ClawtWebhookIngress *self);

G_END_DECLS
