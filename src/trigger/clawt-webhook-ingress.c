/*
 * clawt-webhook-ingress.c - The one door a forge may knock on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "trigger/clawt-webhook-ingress.h"

#include <libsoup/soup.h>

#include <string.h>

struct _ClawtWebhookIngress {
    GObject parent_instance;

    SoupServer *server;
    guint16     port;
    gsize       max_body_bytes;

    GPtrArray  *addresses;    /* gchar*, what actually bound */

    ClawtWebhookDeliverFunc deliver;
    gpointer                deliver_data;
};

G_DEFINE_FINAL_TYPE(ClawtWebhookIngress, clawt_webhook_ingress, G_TYPE_OBJECT)

/*
 * The one answer a caller gets when it is not entitled to a delivery.
 *
 * 404 for every one of "no such endpoint", "the trigger is switched off"
 * and "that path is not served" -- so scanning for endpoints learns
 * nothing, and turning a trigger off does not advertise that it exists.
 * A wrong *secret* is a 401, because at that point the caller has
 * already proved it knows the endpoint and telling it the secret is
 * wrong is what lets somebody fix a rotation.
 */
static void
answer(SoupServerMessage *message, guint status, const gchar *body)
{
    const gchar *text = body != NULL ? body : "";

    soup_server_message_set_status(message, status, NULL);
    soup_server_message_set_response(message, "text/plain",
                                     SOUP_MEMORY_COPY, text, strlen(text));
}

ClawtWebhookRoute
clawt_webhook_route(const gchar  *method,
                    const gchar  *path,
                    const gchar **out_endpoint)
{
    const gchar *endpoint;

    if (out_endpoint != NULL)
        *out_endpoint = NULL;

    if (path == NULL)
        return CLAWT_WEBHOOK_ROUTE_NONE;

    if (g_strcmp0(path, "/health") == 0)
        return (g_strcmp0(method, "GET") == 0)
            ? CLAWT_WEBHOOK_ROUTE_HEALTH
            : CLAWT_WEBHOOK_ROUTE_WRONG_METHOD;

    if (!g_str_has_prefix(path, "/hooks/"))
        return CLAWT_WEBHOOK_ROUTE_NONE;

    endpoint = path + strlen("/hooks/");

    /*
     * A path with anything further down it is not an endpoint.
     *
     * Checked before the method, so `/hooks/<id>/../../etc` is a 404
     * rather than a 405 -- a caller learning that a path exists but
     * wants a different verb is a caller learning that the path exists.
     * And without it a slashed string would reach the endpoint lookup,
     * which is a shape nothing downstream is written to expect.
     */
    if (*endpoint == '\0' || strchr(endpoint, '/') != NULL)
        return CLAWT_WEBHOOK_ROUTE_NONE;

    if (g_strcmp0(method, "POST") != 0)
        return CLAWT_WEBHOOK_ROUTE_WRONG_METHOD;

    if (out_endpoint != NULL)
        *out_endpoint = endpoint;

    return CLAWT_WEBHOOK_ROUTE_DELIVERY;
}

static void
on_request(SoupServer        *server,
           SoupServerMessage *message,
           const gchar       *path,
           GHashTable        *query,
           gpointer           user_data)
{
    ClawtWebhookIngress *self = user_data;
    SoupMessageBody *body;
    g_autoptr(GHashTable) headers = NULL;
    SoupMessageHeadersIter iter;
    const gchar *name;
    const gchar *value;
    const gchar *endpoint = NULL;
    const gchar *presented = NULL;
    g_autofree gchar *reply = NULL;
    guint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

    (void)server;

    switch (clawt_webhook_route(soup_server_message_get_method(message),
                                path, &endpoint)) {
    case CLAWT_WEBHOOK_ROUTE_HEALTH:
        /*
         * Deliberately says nothing but that something answered. A
         * health endpoint that named the fleet, the version or the
         * number of triggers would be the one unauthenticated read on
         * this port, and it would be worth making.
         */
        answer(message, SOUP_STATUS_OK, "ok\n");
        return;

    case CLAWT_WEBHOOK_ROUTE_WRONG_METHOD:
        answer(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
               "a delivery is a POST\n");
        return;

    case CLAWT_WEBHOOK_ROUTE_NONE:
        answer(message, SOUP_STATUS_NOT_FOUND, "no such endpoint\n");
        return;

    case CLAWT_WEBHOOK_ROUTE_DELIVERY:
        break;
    }

    body = soup_server_message_get_request_body(message);

    /*
     * The cap, checked on what arrived.
     *
     * libsoup has already read the body by the time a handler runs, so
     * the streaming limit is set on the server itself -- see
     * clawt_webhook_ingress_start(), where soup_server_add_handler() is
     * paired with a `got-headers` refusal for an oversized
     * Content-Length. This is the second half of the same rule and
     * catches a chunked body that declared no length: refused here,
     * before the delivery function is called and therefore before any
     * HMAC is computed over it.
     */
    if (body != NULL && (gsize)body->length > self->max_body_bytes) {
        answer(message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE,
               "that delivery is too large\n");
        return;
    }

    if (self->deliver == NULL) {
        answer(message, SOUP_STATUS_SERVICE_UNAVAILABLE,
               "nothing is wired up to receive deliveries\n");
        return;
    }

    headers = clawt_trigger_headers_new();
    soup_message_headers_iter_init(&iter,
                                   soup_server_message_get_request_headers(
                                       message));

    while (soup_message_headers_iter_next(&iter, &name, &value))
        clawt_trigger_headers_add(headers, name, value);

    /*
     * The capability form, for a sender that can name a URL and nothing
     * else. Read here because this is the only place that sees a query
     * string at all, and passed on rather than acted on: whether a URL
     * may authenticate depends on which provider the trigger named,
     * which the ingress deliberately does not know.
     */
    if (query != NULL)
        presented = g_hash_table_lookup(query, "token");

    reply = self->deliver(endpoint, headers, presented,
                          (body != NULL) ? (const guchar *)body->data : NULL,
                          (body != NULL) ? (gsize)body->length : 0,
                          self->deliver_data, &status);

    answer(message, status, reply);
}

/*
 * Refuses an oversized delivery before its body is read.
 *
 * This is what makes the cap cost nothing: a caller announcing a
 * gigabyte is turned away at the headers, so neither the memory nor the
 * HMAC over it is ever spent. Without it the limit would still refuse,
 * but only after buffering the whole thing -- which is the attack the
 * limit exists to stop.
 */
static void
on_got_headers(SoupServerMessage *message, gpointer user_data)
{
    ClawtWebhookIngress *self = user_data;
    SoupMessageHeaders *headers =
        soup_server_message_get_request_headers(message);
    goffset declared = soup_message_headers_get_content_length(headers);

    if (declared > 0 && (gsize)declared > self->max_body_bytes) {
        soup_server_message_set_status(
            message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
        answer(message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE,
               "that delivery is too large\n");
    }
}

static void
on_request_started(SoupServer        *server,
                   SoupServerMessage *message,
                   gpointer           user_data)
{
    (void)server;

    g_signal_connect(message, "got-headers", G_CALLBACK(on_got_headers),
                     user_data);
}

static void
clawt_webhook_ingress_dispose(GObject *object)
{
    ClawtWebhookIngress *self = CLAWT_WEBHOOK_INGRESS(object);

    clawt_webhook_ingress_stop(self);

    g_clear_object(&self->server);
    g_clear_pointer(&self->addresses, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_webhook_ingress_parent_class)->dispose(object);
}

static void
clawt_webhook_ingress_class_init(ClawtWebhookIngressClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_webhook_ingress_dispose;
}

static void
clawt_webhook_ingress_init(ClawtWebhookIngress *self)
{
    self->addresses = g_ptr_array_new_with_free_func(g_free);
}

ClawtWebhookIngress *
clawt_webhook_ingress_new(guint16 port, gsize max_body_bytes)
{
    ClawtWebhookIngress *self =
        g_object_new(CLAWT_TYPE_WEBHOOK_INGRESS, NULL);

    self->port = port;
    self->max_body_bytes = max_body_bytes > 0 ? max_body_bytes
                                              : (gsize)(1024 * 1024);

    return self;
}

void
clawt_webhook_ingress_set_deliver_func(ClawtWebhookIngress     *self,
                                       ClawtWebhookDeliverFunc  func,
                                       gpointer                 user_data)
{
    g_return_if_fail(CLAWT_IS_WEBHOOK_INGRESS(self));

    self->deliver = func;
    self->deliver_data = user_data;
}

static gboolean
listen_on(ClawtWebhookIngress  *self,
          const gchar          *address,
          GError              **error)
{
    g_autoptr(GInetAddress) inet = g_inet_address_new_from_string(address);
    g_autoptr(GSocketAddress) socket_address = NULL;

    /*
     * A name is refused rather than resolved.  This listener decides who
     * can start an agent, and a name that resolves differently tomorrow
     * is a listener that moved without anybody saying so.
     */
    if (inet == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not an address", address);
        return FALSE;
    }

    socket_address = g_inet_socket_address_new(inet, self->port);

    if (!soup_server_listen(self->server, socket_address, 0, error))
        return FALSE;

    g_ptr_array_add(self->addresses, g_strdup(address));

    return TRUE;
}

gboolean
clawt_webhook_ingress_start(ClawtWebhookIngress  *self,
                            gboolean              tailnet,
                            GError              **error)
{
    g_return_val_if_fail(CLAWT_IS_WEBHOOK_INGRESS(self), FALSE);

    if (self->server != NULL)
        return TRUE;

    self->server = soup_server_new(NULL, NULL);

    soup_server_add_handler(self->server, "/", on_request, self, NULL);
    g_signal_connect(self->server, "request-started",
                     G_CALLBACK(on_request_started), self);

    /*
     * The loopback is not optional.  Somebody turned the receiver on, so
     * a daemon that could not bind it and carried on regardless would be
     * accepting deliveries nowhere while reporting that it was up.
     */
    if (!listen_on(self, "127.0.0.1", error)) {
        g_clear_object(&self->server);
        return FALSE;
    }

    if (!tailnet)
        return TRUE;

    {
        g_autofree gchar *address = clawt_tailscale_find_address();
        g_autoptr(GError) local = NULL;

        /*
         * No tailnet is the loopback alone, never a fallback to every
         * interface. This is the listener where that mistake would put a
         * fleet on the open internet.
         */
        if (address == NULL)
            return TRUE;

        if (!listen_on(self, address, &local)) {
            g_warning("webhooks: cannot listen on the tailnet address "
                      "%s: %s -- deliveries still reach the loopback",
                      address, local->message);
            return TRUE;
        }
    }

    return TRUE;
}

void
clawt_webhook_ingress_stop(ClawtWebhookIngress *self)
{
    g_return_if_fail(CLAWT_IS_WEBHOOK_INGRESS(self));

    if (self->server == NULL)
        return;

    soup_server_disconnect(self->server);
    g_ptr_array_set_size(self->addresses, 0);
}

GPtrArray *
clawt_webhook_ingress_get_addresses(ClawtWebhookIngress *self)
{
    g_return_val_if_fail(CLAWT_IS_WEBHOOK_INGRESS(self), NULL);

    return self->addresses;
}

guint16
clawt_webhook_ingress_get_port(ClawtWebhookIngress *self)
{
    g_return_val_if_fail(CLAWT_IS_WEBHOOK_INGRESS(self), 0);

    return self->port;
}
