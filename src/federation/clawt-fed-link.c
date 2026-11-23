/*
 * clawt-fed-link.c - Talking to one other fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-link.h"
#include "clawt-http.h"
#include "federation/clawt-fed-seal.h"
#include "federation/clawt-fed-server.h"
#include "ipc/clawt-client.h"
#include "ipc/clawt-ipc-proto.h"

#include <string.h>
#include <gio/gunixsocketaddress.h>
#include <libsoup/soup.h>

/* How long one request may take before the fleet counts as unreachable. */
#define REQUEST_TIMEOUT_SECONDS (30)

struct _ClawtFedLink {
    GObject            parent_instance;
    ClawtFleetKey     *key;
    guint              key_index;
    gchar             *our_fleet;
    gchar             *endpoint;
    ClawtFedTransport  transport;
    GMainContext      *context;

    ClawtFedPeer      *peer;          /* nullable, a copy */
    gboolean           seal;
    gchar             *access_id;
    gchar             *access_secret;
    gchar             *tls_pin;

    /* HTTP */
    SoupSession       *session;
    gchar             *base_url;
    gboolean           reached;

    /* IPC */
    gchar             *ipc_socket;
    gchar             *ipc_host;
    guint16            ipc_port;
    ClawtClient       *client;
    gboolean           connecting;
    GQueue            *waiting;       /* GTask*, sent once connected */

    /* the HTTP link's live stream */
    SoupWebsocketConnection *socket;
    gboolean           socket_opening;
};

enum {
    SIGNAL_PUSH,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtFedLink, clawt_fed_link, G_TYPE_OBJECT)

typedef struct {
    gchar *frame_kind;   /* fed.envelope / fed.invite, for IPC */
    gchar *path;         /* /fed/v1/..., for HTTP */
    gchar *binding;
    gchar *kind;
    gchar *to;
    gchar *body;
    gchar *nonce;
    gboolean verify;
} Outgoing;

static void
outgoing_free(
    Outgoing *o
){
    g_free(o->frame_kind);
    g_free(o->path);
    g_free(o->binding);
    g_free(o->kind);
    g_free(o->to);
    g_free(o->body);
    g_free(o->nonce);
    g_free(o);
}

ClawtFedExchange *
clawt_fed_exchange_copy(
    const ClawtFedExchange *exchange
){
    ClawtFedExchange *copy = g_new0(ClawtFedExchange, 1);

    copy->status = exchange->status;
    copy->stamp = exchange->stamp != NULL ? clawt_fed_stamp_copy(exchange->stamp) : NULL;
    copy->body = g_strdup(exchange->body);
    copy->request_nonce = g_strdup(exchange->request_nonce);
    return copy;
}

G_DEFINE_BOXED_TYPE(ClawtFedExchange, clawt_fed_exchange,
                    clawt_fed_exchange_copy, clawt_fed_exchange_free)

void
clawt_fed_exchange_free(
    ClawtFedExchange *exchange
){
    if (exchange == NULL)
        return;
    clawt_fed_stamp_free(exchange->stamp);
    g_free(exchange->body);
    g_free(exchange->request_nonce);
    g_free(exchange);
}

GError *
clawt_fed_answer_error(
    guint        status,
    const gchar *body
){
    g_autoptr(JsonParser) parser = json_parser_new();
    const gchar *message = NULL;
    gint code;

    if (body != NULL && json_parser_load_from_data(parser, body, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);
            if (o != NULL && json_object_has_member(o, "error")
                && JSON_NODE_HOLDS_VALUE(json_object_get_member(o, "error")))
                message = json_object_get_string_member(o, "error");
        }
    }
    switch (status) {
    case 401: case 403: code = CLAWT_ERROR_AUTH; break;
    case 404: code = CLAWT_ERROR_NOT_FOUND; break;
    case 409: code = CLAWT_ERROR_ALREADY_EXISTS; break;
    case 413: case 400: code = CLAWT_ERROR_INVALID_ARGUMENT; break;
    case 429: code = CLAWT_ERROR_LOOP_LIMIT; break;
    case 501: code = CLAWT_ERROR_NOT_SUPPORTED; break;
    default:  code = CLAWT_ERROR_FAILED; break;
    }
    return g_error_new(CLAWT_ERROR, code, "the other fleet answered %u: %s",
                       status, message != NULL ? message : "no reason given");
}

/* ---- construction ------------------------------------------------------ */

static void
clawt_fed_link_dispose(
    GObject *object
){
    ClawtFedLink *self = CLAWT_FED_LINK(object);

    clawt_fed_link_close(self);
    g_clear_object(&self->session);
    G_OBJECT_CLASS(clawt_fed_link_parent_class)->dispose(object);
}

static void
clawt_fed_link_finalize(
    GObject *object
){
    ClawtFedLink *self = CLAWT_FED_LINK(object);

    clawt_fleet_key_unref(self->key);
    g_free(self->our_fleet);
    g_free(self->endpoint);
    g_clear_pointer(&self->context, g_main_context_unref);
    clawt_fed_peer_free(self->peer);
    g_free(self->access_id);
    if (self->access_secret != NULL)
        memset(self->access_secret, 0, strlen(self->access_secret));
    g_free(self->access_secret);
    g_free(self->base_url);
    g_free(self->tls_pin);
    g_free(self->ipc_socket);
    g_free(self->ipc_host);
    g_queue_free(self->waiting);
    G_OBJECT_CLASS(clawt_fed_link_parent_class)->finalize(object);
}

static void
clawt_fed_link_class_init(
    ClawtFedLinkClass *klass
){
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_fed_link_dispose;
    object_class->finalize = clawt_fed_link_finalize;

    /**
     * ClawtFedLink::push:
     * @self: the link
     * @kind: the envelope's kind
     * @body: its body, checked and opened
     *
     * The other fleet sent something unasked over the connection this
     * link keeps (IPC).  Only emitted for an envelope that checked out.
     */
    signals[SIGNAL_PUSH] =
        g_signal_new("push", CLAWT_TYPE_FED_LINK, G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void
clawt_fed_link_init(
    ClawtFedLink *self
){
    self->waiting = g_queue_new();
}

ClawtFedLink *
clawt_fed_link_new(
    ClawtFleetKey  *key,
    guint           key_index,
    const gchar    *our_fleet,
    const gchar    *endpoint,
    GMainContext   *context,
    GError        **error
){
    g_autoptr(ClawtFedLink) self = NULL;
    g_autofree gchar *host = NULL;
    g_autofree gchar *socket = NULL;
    ClawtFedTransport transport;
    guint16 port = 0;
    gboolean tls = FALSE;

    if (!clawt_fed_endpoint_check(endpoint, error))
        return NULL;
    if (!clawt_fed_endpoint_parse(endpoint, &transport, &host, &port, &socket, &tls)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "cannot read the endpoint %s", endpoint);
        return NULL;
    }

    self = g_object_new(CLAWT_TYPE_FED_LINK, NULL);
    self->key = clawt_fleet_key_ref(key);
    self->key_index = key_index;
    self->our_fleet = g_strdup(our_fleet);
    self->endpoint = g_strdup(endpoint);
    self->transport = transport;
    self->context = context != NULL ? g_main_context_ref(context) : NULL;

    if (transport == CLAWT_FED_TRANSPORT_IPC) {
        self->ipc_socket = g_steal_pointer(&socket);
        self->ipc_host = g_steal_pointer(&host);
        self->ipc_port = port;
        return g_steal_pointer(&self);
    }

    /*
     * libsoup binds a session to the context that is thread-default when
     * it is *made*, and answers into it: made under the wrong one, a
     * request is sent and its answer never read (AGENTS.md, libsoup).
     */
    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);
    if (socket != NULL) {
        g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket);
        self->session = g_object_new(SOUP_TYPE_SESSION,
                                     "remote-connectable", address,
                                     "timeout", REQUEST_TIMEOUT_SECONDS,
                                     "user-agent", "clawtilla-federation", NULL);
        self->base_url = g_strdup("http://localhost");
    } else {
        gsize len;
        self->session = g_object_new(SOUP_TYPE_SESSION,
                                     "timeout", REQUEST_TIMEOUT_SECONDS,
                                     "user-agent", "clawtilla-federation", NULL);
        self->base_url = g_strdup(endpoint);
        len = strlen(self->base_url);
        while (len > 0 && self->base_url[len - 1] == '/')
            self->base_url[--len] = '\0';
    }
    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);
    return g_steal_pointer(&self);
}

void
clawt_fed_link_set_peer(
    ClawtFedLink       *self,
    const ClawtFedPeer *peer
){
    clawt_fed_peer_free(self->peer);
    self->peer = peer != NULL ? clawt_fed_peer_copy(peer) : NULL;
}

void
clawt_fed_link_set_seal(
    ClawtFedLink *self,
    gboolean      seal
){
    self->seal = seal;
}

void
clawt_fed_link_set_access(
    ClawtFedLink *self,
    const gchar  *id,
    const gchar  *secret
){
    g_free(self->access_id);
    g_free(self->access_secret);
    self->access_id = g_strdup(id);
    self->access_secret = g_strdup(secret);
}

void
clawt_fed_link_set_tls_pin(
    ClawtFedLink *self,
    const gchar  *pin
){
    g_free(self->tls_pin);
    self->tls_pin = g_strdup(pin);
}

const gchar *
clawt_fed_link_get_endpoint(
    ClawtFedLink *self
){
    return self->endpoint;
}

ClawtFedTransport
clawt_fed_link_get_transport(
    ClawtFedLink *self
){
    return self->transport;
}

gboolean
clawt_fed_link_is_connected(
    ClawtFedLink *self
){
    if (self->transport == CLAWT_FED_TRANSPORT_IPC)
        return self->client != NULL && clawt_client_is_connected(self->client);
    return self->reached;
}

/* ---- HTTP -------------------------------------------------------------- */

static void
http_set_header(
    const gchar *name,
    const gchar *value,
    gpointer     user_data
){
    soup_message_headers_replace(user_data, name, value);
}

static const gchar *
http_get_header(
    const gchar *name,
    gpointer     user_data
){
    return soup_message_headers_get_one(user_data, name);
}

static void
on_http_answer(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    g_autoptr(GTask) task = user_data;
    ClawtFedLink *self = g_task_get_source_object(task);
    Outgoing *o = g_task_get_task_data(task);
    SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = NULL;
    ClawtFedExchange *exchange;
    gsize length = 0;
    const gchar *data;

    bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
    if (bytes == NULL) {
        self->reached = FALSE;
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                                "%s did not answer: %s", self->endpoint, error->message);
        return;
    }
    self->reached = TRUE;

    exchange = g_new0(ClawtFedExchange, 1);
    exchange->status = soup_message_get_status(message);
    exchange->stamp = clawt_fed_stamp_from_headers(http_get_header,
                                                   soup_message_get_response_headers(message),
                                                   NULL);
    data = g_bytes_get_data(bytes, &length);
    exchange->body = g_strndup(data != NULL ? data : "", length);
    exchange->request_nonce = g_strdup(o->nonce);
    g_task_return_pointer(task, exchange, (GDestroyNotify)clawt_fed_exchange_free);
}

static void
send_http(
    ClawtFedLink *self,
    GTask        *task,
    ClawtFedStamp *stamp
){
    Outgoing *o = g_task_get_task_data(task);
    g_autofree gchar *url = g_strconcat(self->base_url, o->path, NULL);
    /* Signed for here, and may carry an Access token: never redirected. */
    g_autoptr(SoupMessage) message = clawt_http_message_new("POST", url,
                                                            CLAWT_HTTP_CARRIES_CREDENTIALS);
    g_autoptr(GBytes) bytes = NULL;
    SoupMessageHeaders *headers;

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "cannot make a request to %s", url);
        g_object_unref(task);
        return;
    }
    headers = soup_message_get_request_headers(message);
    clawt_fed_stamp_to_headers(stamp, http_set_header, headers);
    if (self->access_id != NULL && self->access_secret != NULL) {
        soup_message_headers_replace(headers, "CF-Access-Client-Id", self->access_id);
        soup_message_headers_replace(headers, "CF-Access-Client-Secret", self->access_secret);
    }
    bytes = g_bytes_new(o->body, strlen(o->body));
    soup_message_set_request_body_from_bytes(message, "application/json", bytes);

    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);
    soup_session_send_and_read_async(self->session, message, G_PRIORITY_DEFAULT,
                                     g_task_get_cancellable(task), on_http_answer, task);
    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);
}

/* ---- IPC --------------------------------------------------------------- */

static const gchar *
ipc_hello(
    ClawtClient  *client,
    JsonNode    **out_payload,
    gpointer      user_data
){
    ClawtFedLink *self = user_data;
    g_autoptr(ClawtFedStamp) stamp = NULL;
    JsonNode *node;

    (void)client;
    /* Signed at the moment of each (re)connect, so it is never stale. */
    stamp = clawt_fed_stamp_sign(self->key, self->key_index, "hello", self->our_fleet,
                                 self->peer != NULL ? self->peer->fleet_id : "",
                                 "{}", "IPC fed.hello", NULL);
    node = json_node_new(JSON_NODE_OBJECT);
    if (stamp != NULL)
        json_node_take_object(node, clawt_fed_envelope_build(stamp, "{}"));
    else
        json_node_take_object(node, json_object_new());
    *out_payload = node;
    return "fed.hello";
}

static void verify_push(ClawtFedLink *self, JsonObject *payload, const gchar *binding);

static void
on_ipc_push(
    ClawtClient *client,
    JsonObject  *payload,
    gpointer     user_data
){
    (void)client;
    verify_push(user_data, payload, "IPC fed.push");
}

/* A pushed envelope: the peer's, to us, fresh -- or dropped unheard. */
static void
verify_push(
    ClawtFedLink *self,
    JsonObject   *payload,
    const gchar  *binding
){
    g_autoptr(ClawtFedStamp) stamp = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *opened = NULL;
    guint8 key[CLAWT_FLEET_KEY_SIZE];
    guint8 seal[CLAWT_FLEET_KEY_SIZE];
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    if (self->peer == NULL || payload == NULL)
        return;
    stamp = clawt_fed_envelope_split(payload, &body, NULL);
    if (stamp == NULL || g_strcmp0(stamp->from, self->peer->fleet_id) != 0
        || !clawt_fed_peer_public_key(self->peer, stamp->key_index, now, key)
        || !clawt_fed_stamp_check(stamp, key, self->our_fleet, body, binding, now,
                                  CLAWT_FED_DEFAULT_SKEW_SECONDS, NULL))
        return;
    if (clawt_fed_body_is_sealed(body)) {
        if (self->peer->seal_key == NULL || !clawt_fed_decode_key(self->peer->seal_key, seal))
            return;
        opened = clawt_fed_unseal(self->key, seal, body, NULL);
        if (opened == NULL)
            return;
    } else {
        opened = g_steal_pointer(&body);
    }
    g_signal_emit(self, signals[SIGNAL_PUSH], 0, stamp->kind, opened);
}

static void
on_ipc_answer(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    g_autoptr(GTask) task = user_data;
    Outgoing *o = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = NULL;
    ClawtFedExchange *exchange;

    reply = clawt_client_request_finish(CLAWT_CLIENT(source), result, &error);
    exchange = g_new0(ClawtFedExchange, 1);
    exchange->request_nonce = g_strdup(o->nonce);
    if (reply == NULL) {
        /* A refusal frame: map it to a status, keep its words. */
        if (error != NULL && error->domain == CLAWT_ERROR
            && error->code == CLAWT_ERROR_NOT_CONNECTED) {
            clawt_fed_exchange_free(exchange);
            g_task_return_error(task, g_steal_pointer(&error));
            return;
        }
        exchange->status = (error != NULL && error->code == CLAWT_ERROR_AUTH) ? 401
                         : (error != NULL && error->code == CLAWT_ERROR_NOT_FOUND) ? 404
                         : (error != NULL && error->code == CLAWT_ERROR_NOT_SUPPORTED) ? 501
                         : (error != NULL && error->code == CLAWT_ERROR_LOOP_LIMIT) ? 429
                         : (error != NULL && error->code == CLAWT_ERROR_ALREADY_EXISTS) ? 409
                         : 400;
        {
            g_autoptr(JsonBuilder) b = json_builder_new();
            g_autoptr(JsonGenerator) g = json_generator_new();
            g_autoptr(JsonNode) root = NULL;
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "error");
            json_builder_add_string_value(b, error != NULL ? error->message : "refused");
            json_builder_end_object(b);
            root = json_builder_get_root(b);
            json_generator_set_root(g, root);
            exchange->body = json_generator_to_data(g, NULL);
        }
        g_task_return_pointer(task, exchange, (GDestroyNotify)clawt_fed_exchange_free);
        return;
    }
    exchange->status = 200;
    if (JSON_NODE_HOLDS_OBJECT(reply)) {
        g_autofree gchar *body = NULL;
        exchange->stamp = clawt_fed_envelope_split(json_node_get_object(reply), &body, NULL);
        exchange->body = g_steal_pointer(&body);
    }
    if (exchange->body == NULL)
        exchange->body = g_strdup("{}");
    g_task_return_pointer(task, exchange, (GDestroyNotify)clawt_fed_exchange_free);
}

static void
send_ipc_now(
    ClawtFedLink  *self,
    GTask         *task
){
    ClawtFedStamp *stamp = g_object_get_data(G_OBJECT(task), "clawt-fed-stamp");
    Outgoing *o = g_task_get_task_data(task);
    JsonNode *payload = json_node_new(JSON_NODE_OBJECT);

    json_node_take_object(payload, clawt_fed_envelope_build(stamp, o->body));
    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);
    clawt_client_request_async(self->client, o->frame_kind, payload,
                               g_task_get_cancellable(task), on_ipc_answer, task);
    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);
}

static void
on_ipc_connected(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    g_autoptr(ClawtFedLink) self = user_data;
    g_autoptr(GError) error = NULL;
    GTask *task;

    self->connecting = FALSE;
    if (!clawt_client_connect_finish(CLAWT_CLIENT(source), result, &error)) {
        /* Everything waiting fails the same way; the next request redials. */
        while ((task = g_queue_pop_head(self->waiting)) != NULL) {
            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                                    "%s did not answer: %s", self->endpoint, error->message);
            g_object_unref(task);
        }
        if (self->client != NULL)
            clawt_client_disconnect(self->client);
        g_clear_object(&self->client);
        return;
    }
    while ((task = g_queue_pop_head(self->waiting)) != NULL)
        send_ipc_now(self, task);
}

static void
send_ipc(
    ClawtFedLink  *self,
    GTask         *task
){
    if (self->client != NULL && clawt_client_is_connected(self->client)) {
        send_ipc_now(self, task);
        return;
    }
    g_queue_push_tail(self->waiting, task);
    if (self->connecting)
        return;
    if (self->client != NULL) {
        clawt_client_disconnect(self->client);
        g_clear_object(&self->client);
    }
    self->client = self->ipc_socket != NULL
        ? clawt_client_new(self->ipc_socket)
        : clawt_client_new_tcp(self->ipc_host, self->ipc_port, NULL);
    clawt_client_set_hello_func(self->client, ipc_hello, self, NULL);
    if (self->tls_pin != NULL && self->ipc_socket == NULL)
        clawt_client_set_tls_pin(self->client, self->tls_pin);
    g_signal_connect(self->client, "push", G_CALLBACK(on_ipc_push), self);
    self->connecting = TRUE;
    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);
    clawt_client_connect_async(self->client, NULL, on_ipc_connected, g_object_ref(self));
    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);
}

/* ---- the live stream ---------------------------------------------------- */

static void
on_socket_message(
    SoupWebsocketConnection *socket,
    gint                     type,
    GBytes                  *message,
    gpointer                 user_data
){
    ClawtFedLink *self = user_data;
    g_autoptr(JsonParser) parser = json_parser_new();
    gsize length = 0;
    const gchar *data = g_bytes_get_data(message, &length);
    JsonNode *root;

    (void)socket;
    /* Text frames only, bounded: a push is a roster, not a download. */
    if (type != SOUP_WEBSOCKET_DATA_TEXT || data == NULL || length > 4 * 1024 * 1024)
        return;
    if (!json_parser_load_from_data(parser, data, (gssize)length, NULL))
        return;
    root = json_parser_get_root(parser);
    if (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
        verify_push(self, json_node_get_object(root), "WS fed.push");
}

static void
on_socket_closed(
    SoupWebsocketConnection *socket,
    gpointer                 user_data
){
    ClawtFedLink *self = user_data;

    if (self->socket == socket) {
        g_signal_handlers_disconnect_by_data(socket, self);
        g_clear_object(&self->socket);
    }
}

static void
on_socket_open(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    g_autoptr(ClawtFedLink) self = user_data;
    g_autoptr(GError) error = NULL;
    SoupWebsocketConnection *socket;

    self->socket_opening = FALSE;
    socket = soup_session_websocket_connect_finish(SOUP_SESSION(source), result, &error);
    if (socket == NULL) {
        g_debug("federation: no live stream from %s: %s", self->endpoint, error->message);
        return;
    }
    self->socket = socket;
    /* Keep-alive pings, so a dead route is noticed and the stream reopened. */
    soup_websocket_connection_set_keepalive_interval(socket, 30);
    soup_websocket_connection_set_max_incoming_payload_size(socket, 4 * 1024 * 1024);
    g_signal_connect(socket, "message", G_CALLBACK(on_socket_message), self);
    g_signal_connect(socket, "closed", G_CALLBACK(on_socket_closed), self);
}

void
clawt_fed_link_open_stream(
    ClawtFedLink *self
){
    g_return_if_fail(CLAWT_IS_FED_LINK(self));

    if (self->peer == NULL)
        return;
    if (self->transport == CLAWT_FED_TRANSPORT_IPC) {
        /* The kept connection is the stream; a ping through it connects it. */
        return;
    }
    if (self->socket != NULL || self->socket_opening)
        return;
    {
        g_autofree gchar *url = g_strconcat(self->base_url, "/fed/v1/events", NULL);
        g_autoptr(SoupMessage) message = clawt_http_message_new("GET", url,
                                                                CLAWT_HTTP_CARRIES_CREDENTIALS);
        g_autoptr(ClawtFedStamp) stamp = NULL;
        SoupMessageHeaders *headers;

        if (message == NULL)
            return;
        stamp = clawt_fed_stamp_sign(self->key, self->key_index, "events", self->our_fleet,
                                     self->peer->fleet_id, "", "GET /fed/v1/events", NULL);
        if (stamp == NULL)
            return;
        headers = soup_message_get_request_headers(message);
        clawt_fed_stamp_to_headers(stamp, http_set_header, headers);
        if (self->access_id != NULL && self->access_secret != NULL) {
            soup_message_headers_replace(headers, "CF-Access-Client-Id", self->access_id);
            soup_message_headers_replace(headers, "CF-Access-Client-Secret", self->access_secret);
        }
        self->socket_opening = TRUE;
        if (self->context != NULL)
            g_main_context_push_thread_default(self->context);
        soup_session_websocket_connect_async(self->session, message, NULL, NULL,
                                             G_PRIORITY_DEFAULT, NULL, on_socket_open,
                                             g_object_ref(self));
        if (self->context != NULL)
            g_main_context_pop_thread_default(self->context);
    }
}

gboolean
clawt_fed_link_is_streaming(
    ClawtFedLink *self
){
    if (self->transport == CLAWT_FED_TRANSPORT_IPC)
        return self->client != NULL && clawt_client_is_connected(self->client);
    return self->socket != NULL
        && soup_websocket_connection_get_state(self->socket) == SOUP_WEBSOCKET_STATE_OPEN;
}

/* ---- the two entry points ---------------------------------------------- */

static void
start(
    ClawtFedLink        *self,
    const gchar         *route_binding,
    const gchar         *kind,
    const gchar         *to,
    const gchar         *body,
    gboolean             verify,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    Outgoing *o;
    g_autoptr(GError) error = NULL;
    ClawtFedStamp *stamp;
    g_autofree gchar *sealed = NULL;

    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);
    task = g_task_new(self, cancellable, callback, user_data);
    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);

    o = g_new0(Outgoing, 1);
    o->kind = g_strdup(kind);
    o->to = g_strdup(to);
    o->verify = verify;
    if (g_strcmp0(route_binding, clawt_fed_route_binding(CLAWT_FED_ROUTE_INVITE)) == 0) {
        o->frame_kind = g_strdup("fed.invite");
        o->path = g_strdup("/fed/v1/invite/accept");
    } else {
        o->frame_kind = g_strdup("fed.envelope");
        o->path = g_strdup("/fed/v1/envelope");
    }
    o->binding = self->transport == CLAWT_FED_TRANSPORT_IPC
        ? g_strdup_printf("IPC %s", o->frame_kind)
        : g_strdup(route_binding);

    /* Sealed first, then signed: the stamp covers the ciphertext. */
    if (verify && self->seal && self->peer != NULL && self->peer->seal_key != NULL) {
        guint8 peer_seal[CLAWT_FLEET_KEY_SIZE];
        if (!clawt_fed_decode_key(self->peer->seal_key, peer_seal)
            || (sealed = clawt_fed_seal(self->key, peer_seal,
                                        body != NULL ? body : "{}", &error)) == NULL) {
            outgoing_free(o);
            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                    "cannot seal for %s: %s", self->endpoint,
                                    error != NULL ? error->message : "no seal key");
            g_object_unref(task);
            return;
        }
        o->body = g_steal_pointer(&sealed);
    } else {
        o->body = g_strdup(body != NULL ? body : "{}");
    }

    stamp = clawt_fed_stamp_sign(self->key, self->key_index, kind, self->our_fleet,
                                 to, o->body, o->binding, &error);
    if (stamp == NULL) {
        outgoing_free(o);
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }
    o->nonce = g_strdup(stamp->nonce);
    g_task_set_task_data(task, o, (GDestroyNotify)outgoing_free);
    g_object_set_data_full(G_OBJECT(task), "clawt-fed-stamp", stamp,
                           (GDestroyNotify)clawt_fed_stamp_free);

    if (self->transport == CLAWT_FED_TRANSPORT_IPC)
        send_ipc(self, task);
    else
        send_http(self, task, stamp);
}

void
clawt_fed_link_exchange_async(
    ClawtFedLink        *self,
    const gchar         *route_binding,
    const gchar         *kind,
    const gchar         *to,
    const gchar         *body,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    g_return_if_fail(CLAWT_IS_FED_LINK(self));
    start(self, route_binding, kind, to, body, FALSE, cancellable, callback, user_data);
}

ClawtFedExchange *
clawt_fed_link_exchange_finish(
    ClawtFedLink  *self,
    GAsyncResult  *result,
    GError       **error
){
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
on_verified_exchange(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    g_autoptr(GTask) outer = user_data;
    ClawtFedLink *self = CLAWT_FED_LINK(source);
    g_autoptr(ClawtFedExchange) exchange = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *binding = NULL;
    g_autofree gchar *opened = NULL;
    guint8 key[CLAWT_FLEET_KEY_SIZE];
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    exchange = g_task_propagate_pointer(G_TASK(result), &error);
    if (exchange == NULL) {
        g_task_return_error(outer, g_steal_pointer(&error));
        return;
    }
    if (exchange->status != 200) {
        g_task_return_error(outer, clawt_fed_answer_error(exchange->status, exchange->body));
        return;
    }

    /*
     * The answer must be the peer's, addressed to us, and bound to this
     * request's nonce -- an answer replayed from another request, or
     * one from whoever sits between us and the peer, fails here.
     */
    binding = g_strdup_printf("re:%s", exchange->request_nonce);
    if (exchange->stamp == NULL
        || g_strcmp0(exchange->stamp->kind, "reply") != 0
        || g_strcmp0(exchange->stamp->from, self->peer->fleet_id) != 0
        || !clawt_fed_peer_public_key(self->peer, exchange->stamp->key_index, now, key)
        || !clawt_fed_stamp_check(exchange->stamp, key, self->our_fleet, exchange->body,
                                  binding, now, CLAWT_FED_DEFAULT_SKEW_SECONDS, &error)) {
        g_task_return_new_error(outer, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                "the answer from %s is not signed by the fleet we know "
                                "as %s (%s)", self->endpoint, self->peer->alias,
                                error != NULL ? error->message : "no stamp");
        return;
    }
    if (clawt_fed_body_is_sealed(exchange->body)) {
        guint8 seal[CLAWT_FLEET_KEY_SIZE];
        if (self->peer->seal_key == NULL || !clawt_fed_decode_key(self->peer->seal_key, seal)) {
            g_task_return_new_error(outer, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                    "a sealed answer from a fleet with no seal key");
            return;
        }
        opened = clawt_fed_unseal(self->key, seal, exchange->body, &error);
        if (opened == NULL) {
            g_task_return_error(outer, g_steal_pointer(&error));
            return;
        }
    } else {
        opened = g_steal_pointer(&exchange->body);
    }
    g_task_return_pointer(outer, g_steal_pointer(&opened), g_free);
}

void
clawt_fed_link_request_async(
    ClawtFedLink        *self,
    const gchar         *kind,
    const gchar         *body,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *outer;

    g_return_if_fail(CLAWT_IS_FED_LINK(self));

    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);
    outer = g_task_new(self, cancellable, callback, user_data);
    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);

    if (self->peer == NULL) {
        g_task_return_new_error(outer, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                "this link has no peer to check answers against");
        g_object_unref(outer);
        return;
    }
    start(self, clawt_fed_route_binding(CLAWT_FED_ROUTE_ENVELOPE), kind,
          self->peer->fleet_id, body, TRUE, cancellable, on_verified_exchange, outer);
}

gchar *
clawt_fed_link_request_finish(
    ClawtFedLink  *self,
    GAsyncResult  *result,
    GError       **error
){
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

void
clawt_fed_link_close(
    ClawtFedLink *self
){
    GTask *task;

    while (self->waiting != NULL && (task = g_queue_pop_head(self->waiting)) != NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                                "the link to %s was closed", self->endpoint);
        g_object_unref(task);
    }
    if (self->client != NULL) {
        g_signal_handlers_disconnect_by_data(self->client, self);
        clawt_client_disconnect(self->client);
        g_clear_object(&self->client);
    }
    self->connecting = FALSE;
    if (self->socket != NULL) {
        g_signal_handlers_disconnect_by_data(self->socket, self);
        if (soup_websocket_connection_get_state(self->socket) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(self->socket, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
        g_clear_object(&self->socket);
    }
    if (self->session != NULL)
        soup_session_abort(self->session);
}
