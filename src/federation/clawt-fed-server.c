/*
 * clawt-fed-server.c - The HTTP door other fleets knock on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-server.h"
#include "clawt-http.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>
#include <libsoup/soup.h>

/* Requests a single unauthenticated address may make in a minute. */
#define ANON_PER_MINUTE (60)

struct _ClawtFedServer {
    GObject             parent_instance;
    SoupServer         *server;
    gsize               max_body_bytes;
    ClawtFedServerFunc  handler;
    ClawtFedSocketFunc  socket_handler;
    gpointer            user_data;
    GPtrArray          *bound;        /* gchar*, as given */
    GPtrArray          *socket_files; /* gchar*, unix paths we made */
    GHashTable         *buckets;      /* remote -> Bucket */
};

typedef struct {
    gint64 window;
    guint  count;
} Bucket;

G_DEFINE_FINAL_TYPE(ClawtFedServer, clawt_fed_server, G_TYPE_OBJECT)

ClawtFedRoute
clawt_fed_route(
    const gchar *method,
    const gchar *path
){
    static const struct {
        const gchar   *suffix;
        const gchar   *method;
        ClawtFedRoute  route;
    } table[] = {
        { "/fed/v1/hello", "GET", CLAWT_FED_ROUTE_HELLO },
        { "/fed/v1/invite/accept", "POST", CLAWT_FED_ROUTE_INVITE },
        { "/fed/v1/envelope", "POST", CLAWT_FED_ROUTE_ENVELOPE },
        { "/fed/v1/events", "GET", CLAWT_FED_ROUTE_EVENTS },
    };
    guint i;

    if (path == NULL || method == NULL)
        return CLAWT_FED_ROUTE_NONE;
    for (i = 0; i < G_N_ELEMENTS(table); i++) {
        /* A base mount (`/base/fed/v1/...`) is found by its suffix. */
        if (!g_str_has_suffix(path, table[i].suffix))
            continue;
        if (strcmp(method, table[i].method) != 0)
            return CLAWT_FED_ROUTE_WRONG_METHOD;
        return table[i].route;
    }
    return CLAWT_FED_ROUTE_NONE;
}

const gchar *
clawt_fed_route_binding(
    ClawtFedRoute route
){
    switch (route) {
    case CLAWT_FED_ROUTE_HELLO:        return "GET /fed/v1/hello";
    case CLAWT_FED_ROUTE_INVITE:       return "POST /fed/v1/invite/accept";
    case CLAWT_FED_ROUTE_ENVELOPE:     return "POST /fed/v1/envelope";
    case CLAWT_FED_ROUTE_EVENTS:       return "GET /fed/v1/events";
    case CLAWT_FED_ROUTE_NONE:
    case CLAWT_FED_ROUTE_WRONG_METHOD: return "";
    }
    return "";
}

static void
clawt_fed_server_finalize(
    GObject *object
){
    ClawtFedServer *self = CLAWT_FED_SERVER(object);

    clawt_fed_server_stop(self);
    g_clear_object(&self->server);
    g_ptr_array_unref(self->bound);
    g_ptr_array_unref(self->socket_files);
    g_hash_table_unref(self->buckets);
    G_OBJECT_CLASS(clawt_fed_server_parent_class)->finalize(object);
}

static void
clawt_fed_server_class_init(
    ClawtFedServerClass *klass
){
    G_OBJECT_CLASS(klass)->finalize = clawt_fed_server_finalize;
}

static void
clawt_fed_server_init(
    ClawtFedServer *self
){
    self->bound = g_ptr_array_new_with_free_func(g_free);
    self->socket_files = g_ptr_array_new_with_free_func(g_free);
    self->buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/* Who asked: a tunnel's CF-Connecting-IP only over a local socket. */
static gchar *
remote_of(
    SoupServerMessage *message
){
    GSocketAddress *address = soup_server_message_get_remote_address(message);
    const gchar *forwarded;

    forwarded = soup_message_headers_get_one(
        soup_server_message_get_request_headers(message), "CF-Connecting-IP");

    if (address == NULL || G_IS_UNIX_SOCKET_ADDRESS(address))
        return g_strdup(forwarded != NULL ? forwarded : "local");
    if (G_IS_INET_SOCKET_ADDRESS(address)) {
        GInetAddress *inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address));
        if (g_inet_address_get_is_loopback(inet) && forwarded != NULL)
            return g_strdup(forwarded);
        return g_inet_address_to_string(inet);
    }
    return g_strdup("unknown");
}

/* A fixed window per minute; TRUE while under the limit. */
static gboolean
bucket_take(
    ClawtFedServer *self,
    const gchar    *remote
){
    gint64 minute = g_get_monotonic_time() / (60 * G_USEC_PER_SEC);
    Bucket *b = g_hash_table_lookup(self->buckets, remote);

    if (b == NULL) {
        /* Keep the table bounded: forget everybody when it grows large. */
        if (g_hash_table_size(self->buckets) > 4096)
            g_hash_table_remove_all(self->buckets);
        b = g_new0(Bucket, 1);
        g_hash_table_insert(self->buckets, g_strdup(remote), b);
    }
    if (b->window != minute) {
        b->window = minute;
        b->count = 0;
    }
    return ++b->count <= ANON_PER_MINUTE;
}

static const gchar *
header_lookup(
    const gchar *name,
    gpointer     user_data
){
    return soup_message_headers_get_one(user_data, name);
}

static void
header_set(
    const gchar *name,
    const gchar *value,
    gpointer     user_data
){
    soup_message_headers_replace(user_data, name, value);
}

static void
answer_plain(
    SoupServerMessage *message,
    guint              status,
    const gchar       *text
){
    soup_server_message_set_status(message, status, NULL);
    soup_server_message_set_response(message, "application/json",
                                     SOUP_MEMORY_COPY, text, strlen(text));
}

static void
on_request(
    SoupServer        *server,
    SoupServerMessage *message,
    const char        *path,
    GHashTable        *query,
    gpointer           user_data
){
    ClawtFedServer *self = user_data;
    ClawtFedRoute route;
    g_autoptr(ClawtFedStamp) stamp = NULL;
    g_autofree gchar *remote = NULL;
    g_autofree gchar *body = NULL;
    SoupMessageBody *bounded;
    ClawtFedAnswer answer;

    (void)server;
    (void)query;

    route = clawt_fed_route(soup_server_message_get_method(message), path);
    switch (route) {
    case CLAWT_FED_ROUTE_NONE:
        answer_plain(message, SOUP_STATUS_NOT_FOUND, "{\"error\":\"no such route\"}");
        return;
    case CLAWT_FED_ROUTE_WRONG_METHOD:
        answer_plain(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                     "{\"error\":\"wrong method\"}");
        return;
    case CLAWT_FED_ROUTE_EVENTS:
        /* Only an upgrade belongs here; the WebSocket handler takes it. */
        answer_plain(message, SOUP_STATUS_BAD_REQUEST,
                     "{\"error\":\"this route is a WebSocket\"}");
        return;
    case CLAWT_FED_ROUTE_HELLO:
    case CLAWT_FED_ROUTE_INVITE:
    case CLAWT_FED_ROUTE_ENVELOPE:
        break;
    }

    remote = remote_of(message);
    stamp = clawt_fed_stamp_from_headers(header_lookup,
                                         soup_server_message_get_request_headers(message),
                                         NULL);
    /* Unsigned requests (a hello, a probe) are rate limited per address. */
    if (stamp == NULL && !bucket_take(self, remote)) {
        answer_plain(message, 429,
                     "{\"error\":\"slow down\"}");
        return;
    }

    /*
     * Flattened before it is read: with accumulation off, a body's `data`
     * is not filled in until something flattens the chunks.
     */
    bounded = clawt_http_server_message_body(message);
    if (bounded != NULL && bounded->length > 0) {
        g_autoptr(GBytes) flat = soup_message_body_flatten(bounded);
        gsize length = 0;
        const gchar *data = g_bytes_get_data(flat, &length);

        if (data == NULL || memchr(data, '\0', length) != NULL
            || !g_utf8_validate(data, length, NULL)) {
            answer_plain(message, SOUP_STATUS_BAD_REQUEST,
                         "{\"error\":\"the body is not text\"}");
            return;
        }
        body = g_strndup(data, length);
    } else {
        body = g_strdup("");
    }

    memset(&answer, 0, sizeof(answer));
    answer.status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
    self->handler(self, route, stamp, body, remote, &answer, self->user_data);

    if (answer.stamp != NULL)
        clawt_fed_stamp_to_headers(answer.stamp,
                                   header_set,
                                   soup_server_message_get_response_headers(message));
    answer_plain(message, answer.status, answer.body != NULL ? answer.body : "{}");
    clawt_fed_stamp_free(answer.stamp);
    g_free(answer.body);
}

static void
on_websocket(
    SoupServer              *server,
    SoupServerMessage       *message,
    const char              *path,
    SoupWebsocketConnection *connection,
    gpointer                 user_data
){
    ClawtFedServer *self = user_data;
    g_autoptr(ClawtFedStamp) stamp = NULL;

    (void)server;
    (void)path;

    stamp = clawt_fed_stamp_from_headers(header_lookup,
                                         soup_server_message_get_request_headers(message),
                                         NULL);
    /*
     * Closed before a frame is read or sent, unless the handler takes it
     * -- and it verifies the stamp before it does.
     */
    if (stamp == NULL || self->socket_handler == NULL
        || !self->socket_handler(self, connection, stamp, self->user_data))
        soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION,
                                        "not authenticated");
}

ClawtFedServer *
clawt_fed_server_new(
    gsize              max_body_bytes,
    ClawtFedServerFunc handler,
    ClawtFedSocketFunc socket_handler,
    gpointer           user_data
){
    ClawtFedServer *self;

    self = g_object_new(CLAWT_TYPE_FED_SERVER, NULL);
    self->max_body_bytes = max_body_bytes > 0 ? max_body_bytes : 1048576;
    self->handler = handler;
    self->socket_handler = socket_handler;
    self->user_data = user_data;
    self->server = soup_server_new("server-header", "clawtilla-federation", NULL);
    clawt_http_server_limit_bodies(self->server, self->max_body_bytes);
    soup_server_add_websocket_handler(self->server, "/fed/v1/events", NULL, NULL,
                                      on_websocket, self, NULL);
    soup_server_add_handler(self->server, NULL, on_request, self, NULL);
    return self;
}

static gboolean
listen_unix(
    ClawtFedServer  *self,
    const gchar     *path,
    GError         **error
){
    g_autoptr(GSocket) socket = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autofree gchar *dir = NULL;

    if (!clawt_check_socket_path(path, error))
        return FALSE;
    dir = g_path_get_dirname(path);
    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;
    /* The daemon's state lock says this path is ours; a leftover goes. */
    if (g_file_test(path, G_FILE_TEST_EXISTS) && g_unlink(path) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "cannot replace %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                          G_SOCKET_PROTOCOL_DEFAULT, error);
    if (socket == NULL)
        return FALSE;
    address = g_unix_socket_address_new(path);
    if (!g_socket_bind(socket, address, TRUE, error) || !g_socket_listen(socket, error))
        return FALSE;
    g_chmod(path, 0600);
    if (!soup_server_listen_socket(self->server, socket, 0, error))
        return FALSE;
    g_ptr_array_add(self->socket_files, g_strdup(path));
    return TRUE;
}

gboolean
clawt_fed_server_listen(
    ClawtFedServer  *self,
    const gchar     *where,
    GError         **error
){
    g_return_val_if_fail(CLAWT_IS_FED_SERVER(self), FALSE);

    if (where == NULL || *where == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "no address to listen on");
        return FALSE;
    }
    if (g_str_has_prefix(where, "unix:")) {
        if (!listen_unix(self, where + 5, error))
            return FALSE;
    } else {
        g_autofree gchar *host = NULL;
        g_autoptr(GInetAddress) inet = NULL;
        g_autoptr(GSocketAddress) address = NULL;
        guint16 port = 0;
        g_autofree gchar *endpoint = g_strdup_printf("clawtilla://%s", where);

        if (!clawt_fed_endpoint_parse(endpoint, NULL, &host, &port, NULL, NULL)
            || strchr(where, ':') == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "\"%s\" is not unix:PATH or HOST:PORT", where);
            return FALSE;
        }
        /* An address, never a name: a name could resolve anywhere. */
        inet = g_inet_address_new_from_string(host);
        if (inet == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "\"%s\" names a host; federation.listen takes an address",
                        where);
            return FALSE;
        }
        address = g_inet_socket_address_new(inet, port);
        if (!soup_server_listen(self->server, address, 0, error))
            return FALSE;
    }
    g_ptr_array_add(self->bound, g_strdup(where));
    return TRUE;
}

gchar **
clawt_fed_server_list_bound(
    ClawtFedServer *self
){
    GPtrArray *out = g_ptr_array_new();
    guint i;

    for (i = 0; i < self->bound->len; i++)
        g_ptr_array_add(out, g_strdup(g_ptr_array_index(self->bound, i)));
    g_ptr_array_add(out, NULL);
    return (gchar **)g_ptr_array_free(out, FALSE);
}

void
clawt_fed_server_stop(
    ClawtFedServer *self
){
    guint i;

    if (self->server != NULL)
        soup_server_disconnect(self->server);
    for (i = 0; i < self->socket_files->len; i++)
        g_unlink(g_ptr_array_index(self->socket_files, i));
    g_ptr_array_set_size(self->socket_files, 0);
    g_ptr_array_set_size(self->bound, 0);
}
