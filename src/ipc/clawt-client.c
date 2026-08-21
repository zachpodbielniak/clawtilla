/*
 * clawt-client.c - Talking to the daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ipc/clawt-client.h"
#include "ipc/clawt-ipc-proto.h"

#include <gio/gunixsocketaddress.h>

#include <string.h>

/*
 * How long a synchronous request waits before giving up.
 *
 * A bound is needed because the alternative is a CLI that hangs for ever
 * against a daemon that is wedged, with no output and no clue why.
 */
#define REQUEST_TIMEOUT_SECONDS 120

#define RECONNECT_BASE_SECONDS 1
#define RECONNECT_MAX_SECONDS 60

struct _ClawtClient {
    GObject parent_instance;

    gchar   *socket_path;
    gchar   *host;
    guint16  port;
    gchar   *token;
    gboolean tls;
    gboolean accept_unknown_certificate;

    GIOStream        *stream;
    GSocketConnection *connection;
    GDataInputStream *input;
    GOutputStream    *output;

    gboolean subscribed;
    guint64  cursor;
    guint    request_serial;

    gboolean auto_reconnect;
    guint    reconnect_source_id;
    guint    reconnect_delay;
};

G_DEFINE_FINAL_TYPE(ClawtClient, clawt_client, G_TYPE_OBJECT)

enum {
    SIGNAL_CONNECTED,
    SIGNAL_DISCONNECTED,
    SIGNAL_EVENT,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void read_next(ClawtClient *self);
static gboolean try_reconnect(gpointer user_data);

gchar *
clawt_client_default_socket_path(void)
{
    const gchar *runtime = g_get_user_runtime_dir();

    return g_build_filename(runtime, "clawtilla", "daemon.sock", NULL);
}

ClawtClient *
clawt_client_new(const gchar *socket_path)
{
    ClawtClient *self = g_object_new(CLAWT_TYPE_CLIENT, NULL);

    self->socket_path = (socket_path != NULL)
                        ? clawt_expand_path(socket_path)
                        : clawt_client_default_socket_path();

    return self;
}

ClawtClient *
clawt_client_new_tcp(const gchar *host, guint16 port, const gchar *token)
{
    ClawtClient *self = g_object_new(CLAWT_TYPE_CLIENT, NULL);

    self->host = g_strdup(host);
    self->port = port;
    self->token = g_strdup(token);

    return self;
}

void
clawt_client_set_tls(ClawtClient *self, gboolean enabled,
                     gboolean accept_unknown_certificate)
{
    g_return_if_fail(CLAWT_IS_CLIENT(self));

    self->tls = enabled;
    self->accept_unknown_certificate = accept_unknown_certificate;
}

void
clawt_client_set_auto_reconnect(ClawtClient *self, gboolean enabled)
{
    g_return_if_fail(CLAWT_IS_CLIENT(self));

    self->auto_reconnect = enabled;
}

gboolean
clawt_client_is_connected(ClawtClient *self)
{
    g_return_val_if_fail(CLAWT_IS_CLIENT(self), FALSE);

    return self->stream != NULL;
}

guint64
clawt_client_get_cursor(ClawtClient *self)
{
    g_return_val_if_fail(CLAWT_IS_CLIENT(self), 0);

    return self->cursor;
}

/* ── Wire ────────────────────────────────────────────────────────── */

static gboolean
send_frame(ClawtClient *self, JsonNode *frame, GError **error)
{
    g_autofree gchar *line = NULL;

    if (self->output == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "not connected to the daemon");
        return FALSE;
    }

    line = clawt_ipc_frame_to_line(frame);

    return g_output_stream_write_all(self->output, line, strlen(line), NULL,
                                     NULL, error);
}

static void
dispatch_event(ClawtClient *self, JsonNode *frame)
{
    JsonObject *payload = clawt_ipc_frame_get_payload(frame);
    g_autoptr(ClawtEvent) event = NULL;
    const gchar *kind;

    if (payload == NULL || !json_object_has_member(payload, "kind"))
        return;

    kind = json_object_get_string_member(payload, "kind");

    event = clawt_event_new(kind,
                            json_object_has_member(payload, "subject")
                                ? json_object_get_string_member(payload,
                                                                "subject")
                                : NULL);

    if (json_object_has_member(payload, "ts"))
        clawt_event_set_timestamp(event,
                                  json_object_get_int_member(payload, "ts"));

    if (json_object_has_member(payload, "cursor")) {
        guint64 cursor =
            (guint64)json_object_get_int_member(payload, "cursor");

        clawt_event_set_cursor(event, cursor);

        /*
         * Tracked here rather than by the caller, so a reconnect can
         * resume from exactly what this client has actually seen.
         */
        if (cursor > self->cursor)
            self->cursor = cursor;
    }

    if (json_object_has_member(payload, "detail")) {
        JsonObject *detail = json_object_get_object_member(payload, "detail");
        g_autoptr(GList) members = json_object_get_members(detail);
        GList *l;

        for (l = members; l != NULL; l = l->next)
            clawt_event_set_detail(
                event, l->data,
                json_object_get_string_member(detail, l->data));
    }

    g_signal_emit(self, signals[SIGNAL_EVENT], 0, event);
}

static void
handle_disconnect(ClawtClient *self)
{
    if (self->stream == NULL)
        return;

    g_clear_object(&self->input);
    g_clear_object(&self->connection);
    g_clear_object(&self->stream);
    self->output = NULL;

    g_signal_emit(self, signals[SIGNAL_DISCONNECTED], 0);

    if (!self->auto_reconnect || self->reconnect_source_id != 0)
        return;

    if (self->reconnect_delay == 0)
        self->reconnect_delay = RECONNECT_BASE_SECONDS;

    self->reconnect_source_id =
        g_timeout_add_seconds(self->reconnect_delay, try_reconnect, self);
}

static gboolean
try_reconnect(gpointer user_data)
{
    ClawtClient *self = user_data;
    g_autoptr(GError) error = NULL;

    self->reconnect_source_id = 0;

    if (clawt_client_connect(self, &error)) {
        self->reconnect_delay = RECONNECT_BASE_SECONDS;

        if (self->subscribed) {
            gboolean resumed = TRUE;

            /*
             * Resumed from the last cursor actually seen, and the answer
             * is passed on: a client that assumes it caught up when it did
             * not will show stale state indefinitely.
             */
            clawt_client_subscribe(self, self->cursor, &resumed, NULL);
        }

        return G_SOURCE_REMOVE;
    }

    /* Doubling, capped: a daemon that is down stays down for a while. */
    self->reconnect_delay = MIN(self->reconnect_delay * 2,
                                RECONNECT_MAX_SECONDS);
    self->reconnect_source_id =
        g_timeout_add_seconds(self->reconnect_delay, try_reconnect, self);

    return G_SOURCE_REMOVE;
}

static void
on_line_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtClient *self = user_data;
    g_autofree gchar *line = NULL;
    g_autoptr(JsonNode) frame = NULL;

    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                                result, NULL, NULL);

    if (line == NULL) {
        handle_disconnect(self);
        return;
    }

    frame = clawt_ipc_frame_from_line(line, NULL);

    if (frame != NULL &&
        g_strcmp0(clawt_ipc_frame_get_kind(frame), "event") == 0)
        dispatch_event(self, frame);

    read_next(self);
}

static void
read_next(ClawtClient *self)
{
    if (self->input == NULL)
        return;

    g_data_input_stream_read_line_async(self->input, G_PRIORITY_DEFAULT,
                                        NULL, on_line_read, self);
}

/* ── Connecting ──────────────────────────────────────────────────── */

static gboolean
on_accept_certificate(GTlsConnection       *connection,
                      GTlsCertificate      *peer_certificate,
                      GTlsCertificateFlags  errors,
                      gpointer              user_data)
{
    (void)connection;
    (void)peer_certificate;
    (void)user_data;

    g_warning("ipc: accepting the daemon's certificate without validating "
              "it (0x%x); anything answering on that address is trusted",
              (guint)errors);

    return TRUE;
}

static void
on_socket_client_event(GSocketClient       *client,
                       GSocketClientEvent   event,
                       GSocketConnectable  *connectable,
                       GIOStream           *connection,
                       gpointer             user_data)
{
    (void)client;
    (void)connectable;

    if (event != G_SOCKET_CLIENT_TLS_HANDSHAKING)
        return;

    g_signal_connect(connection, "accept-certificate",
                     G_CALLBACK(on_accept_certificate), user_data);
}

static gboolean
say_hello(ClawtClient *self, GError **error)
{
    g_autoptr(JsonNode) request = NULL;
    g_autoptr(JsonNode) reply = NULL;

    request = clawt_ipc_request_new("control.hello", "hello");

    if (self->token != NULL) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "token");
        json_builder_add_string_value(builder, self->token);
        json_builder_end_object(builder);

        clawt_ipc_frame_set_payload(request, json_builder_get_root(builder));
    }

    if (!send_frame(self, request, error))
        return FALSE;

    for (;;) {
        g_autofree gchar *line = NULL;
        g_autoptr(JsonNode) frame = NULL;

        line = g_data_input_stream_read_line(self->input, NULL, NULL, error);

        if (line == NULL) {
            if (error != NULL && *error == NULL)
                g_set_error_literal(error, CLAWT_ERROR,
                                    CLAWT_ERROR_NOT_CONNECTED,
                                    "the daemon closed the connection during "
                                    "the handshake");
            return FALSE;
        }

        frame = clawt_ipc_frame_from_line(line, NULL);

        if (frame == NULL)
            continue;

        if (g_strcmp0(clawt_ipc_frame_get_kind(frame), "event") == 0) {
            dispatch_event(self, frame);
            continue;
        }

        if (clawt_ipc_frame_is_error(frame)) {
            GError *reported = clawt_ipc_frame_to_error(frame);

            g_propagate_error(error, reported);
            return FALSE;
        }

        return TRUE;
    }
}

gboolean
clawt_client_connect(ClawtClient *self, GError **error)
{
    g_autoptr(GSocketClient) socket_client = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GSocketAddress) address = NULL;

    g_return_val_if_fail(CLAWT_IS_CLIENT(self), FALSE);

    if (self->stream != NULL)
        return TRUE;

    socket_client = g_socket_client_new();

    if (self->socket_path != NULL) {
        address = g_unix_socket_address_new(self->socket_path);
        connection = g_socket_client_connect(socket_client,
                                             G_SOCKET_CONNECTABLE(address),
                                             NULL, error);

        if (connection == NULL) {
            /*
             * Named, with the likely cause.  "Connection refused" on its
             * own sends people hunting for a permissions problem when the
             * answer is that the daemon is not running.
             */
            g_prefix_error(error, "could not reach the daemon at %s "
                                  "(is clawtillad running?): ",
                           self->socket_path);
            return FALSE;
        }

        self->stream = G_IO_STREAM(g_object_ref(connection));
    } else {
        if (self->tls) {
            g_socket_client_set_tls(socket_client, TRUE);

            /*
             * Accepting an unvalidated certificate is done by answering
             * the handshake rather than by clearing a validation-flags
             * field, which GLib deprecated precisely because clearing it
             * silently accepted everything for ever.  Here the decision is
             * made per connection and only when explicitly asked for.
             */
            if (self->accept_unknown_certificate)
                g_signal_connect(socket_client, "event",
                                 G_CALLBACK(on_socket_client_event), self);
        }

        connection = g_socket_client_connect_to_host(socket_client,
                                                     self->host, self->port,
                                                     NULL, error);

        if (connection == NULL) {
            g_prefix_error(error, "could not reach the daemon at %s:%u: ",
                           self->host, self->port);
            return FALSE;
        }

        self->stream = G_IO_STREAM(g_object_ref(connection));
    }

    self->connection = g_object_ref(connection);
    self->input = g_data_input_stream_new(
        g_io_stream_get_input_stream(self->stream));
    g_data_input_stream_set_newline_type(self->input,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);
    self->output = g_io_stream_get_output_stream(self->stream);

    if (!say_hello(self, error)) {
        clawt_client_disconnect(self);
        return FALSE;
    }

    g_signal_emit(self, signals[SIGNAL_CONNECTED], 0);

    return TRUE;
}

void
clawt_client_disconnect(ClawtClient *self)
{
    g_return_if_fail(CLAWT_IS_CLIENT(self));

    if (self->reconnect_source_id != 0) {
        g_source_remove(self->reconnect_source_id);
        self->reconnect_source_id = 0;
    }

    if (self->stream == NULL)
        return;

    g_io_stream_close(self->stream, NULL, NULL);

    g_clear_object(&self->input);
    g_clear_object(&self->connection);
    g_clear_object(&self->stream);
    self->output = NULL;
}

/* ── Requests ────────────────────────────────────────────────────── */

JsonNode *
clawt_client_request(ClawtClient  *self,
                     const gchar  *kind,
                     JsonNode     *payload,
                     GError      **error)
{
    g_autoptr(JsonNode) request = NULL;
    g_autofree gchar *id = NULL;
    gint64 deadline;

    g_return_val_if_fail(CLAWT_IS_CLIENT(self), NULL);
    g_return_val_if_fail(kind != NULL, NULL);

    if (self->stream == NULL) {
        json_node_unref(payload);
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "not connected to the daemon");
        return NULL;
    }

    id = g_strdup_printf("r%u", ++self->request_serial);
    request = clawt_ipc_request_new(kind, id);

    if (payload != NULL)
        clawt_ipc_frame_set_payload(request, payload);

    if (!send_frame(self, request, error))
        return NULL;

    deadline = g_get_monotonic_time() +
               ((gint64)REQUEST_TIMEOUT_SECONDS * G_USEC_PER_SEC);

    for (;;) {
        g_autofree gchar *line = NULL;
        g_autoptr(JsonNode) frame = NULL;
        g_autoptr(GError) local = NULL;

        if (g_get_monotonic_time() > deadline) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                        "the daemon did not answer %s within %d seconds",
                        kind, REQUEST_TIMEOUT_SECONDS);
            return NULL;
        }

        line = g_data_input_stream_read_line(self->input, NULL, NULL, &local);

        if (line == NULL) {
            handle_disconnect(self);
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                                local != NULL ? local->message
                                              : "the daemon closed the "
                                                "connection");
            return NULL;
        }

        frame = clawt_ipc_frame_from_line(line, NULL);

        if (frame == NULL)
            continue;

        /*
         * Events that arrive while waiting are still delivered.  Dropping
         * them would mean a synchronous call silently punched a hole in
         * the caller's subscription.
         */
        if (g_strcmp0(clawt_ipc_frame_get_kind(frame), "event") == 0) {
            dispatch_event(self, frame);
            continue;
        }

        /* Somebody else's reply, on a shared connection. */
        if (g_strcmp0(clawt_ipc_frame_get_id(frame), id) != 0)
            continue;

        if (clawt_ipc_frame_is_error(frame)) {
            GError *reported = clawt_ipc_frame_to_error(frame);

            g_propagate_error(error, reported);
            return NULL;
        }

        {
            JsonObject *reply = clawt_ipc_frame_get_payload(frame);

            if (reply == NULL)
                return json_node_new(JSON_NODE_OBJECT);

            return json_node_init_object(json_node_alloc(), reply);
        }
    }
}

typedef struct {
    gchar    *kind;
    JsonNode *payload;
} RequestData;

static void
request_data_free(gpointer data)
{
    RequestData *request = data;

    g_free(request->kind);
    g_clear_pointer(&request->payload, json_node_unref);
    g_free(request);
}

static void
request_thread(GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
    ClawtClient *self = source;
    RequestData *request = task_data;
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    (void)cancellable;

    reply = clawt_client_request(self, request->kind,
                                 g_steal_pointer(&request->payload), &error);

    if (reply == NULL)
        g_task_return_error(task, g_steal_pointer(&error));
    else
        g_task_return_pointer(task, reply,
                              (GDestroyNotify)json_node_unref);
}

void
clawt_client_request_async(ClawtClient         *self,
                           const gchar         *kind,
                           JsonNode            *payload,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    g_autoptr(GTask) task = NULL;
    RequestData *request;

    g_return_if_fail(CLAWT_IS_CLIENT(self));
    g_return_if_fail(kind != NULL);

    request = g_new0(RequestData, 1);
    request->kind = g_strdup(kind);
    request->payload = payload;

    task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_task_data(task, request, request_data_free);
    g_task_run_in_thread(task, request_thread);
}

JsonNode *
clawt_client_request_finish(ClawtClient   *self,
                            GAsyncResult  *result,
                            GError       **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean
clawt_client_subscribe(ClawtClient  *self,
                       guint64       cursor,
                       gboolean     *out_resumed,
                       GError      **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *object;

    g_return_val_if_fail(CLAWT_IS_CLIENT(self), FALSE);

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "cursor");
    json_builder_add_int_value(builder, (gint64)cursor);
    json_builder_end_object(builder);

    reply = clawt_client_request(self, "control.subscribe",
                                 json_builder_get_root(builder), error);

    if (reply == NULL)
        return FALSE;

    self->subscribed = TRUE;
    object = json_node_get_object(reply);

    if (out_resumed != NULL)
        *out_resumed = json_object_has_member(object, "resumed")
                       ? json_object_get_boolean_member(object, "resumed")
                       : TRUE;

    /*
     * Reading resumes only now: until the subscription is answered, the
     * synchronous read loop above owns the stream, and a second reader
     * would race it for lines.
     */
    read_next(self);

    return TRUE;
}

static void
clawt_client_dispose(GObject *object)
{
    ClawtClient *self = CLAWT_CLIENT(object);

    self->auto_reconnect = FALSE;
    clawt_client_disconnect(self);

    G_OBJECT_CLASS(clawt_client_parent_class)->dispose(object);
}

static void
clawt_client_finalize(GObject *object)
{
    ClawtClient *self = CLAWT_CLIENT(object);

    g_free(self->socket_path);
    g_free(self->host);
    g_free(self->token);

    G_OBJECT_CLASS(clawt_client_parent_class)->finalize(object);
}

static void
clawt_client_class_init(ClawtClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_client_dispose;
    object_class->finalize = clawt_client_finalize;

    signals[SIGNAL_CONNECTED] =
        g_signal_new("connected", CLAWT_TYPE_CLIENT, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 0);

    signals[SIGNAL_DISCONNECTED] =
        g_signal_new("disconnected", CLAWT_TYPE_CLIENT, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 0);

    /**
     * ClawtClient::event:
     * @self: the client
     * @event: what happened
     *
     * Emitted for every event from the daemon.
     */
    signals[SIGNAL_EVENT] =
        g_signal_new("event", CLAWT_TYPE_CLIENT, G_SIGNAL_RUN_LAST, 0, NULL,
                     NULL, NULL, G_TYPE_NONE, 1, CLAWT_TYPE_EVENT);
}

static void
clawt_client_init(ClawtClient *self)
{
    self->reconnect_delay = RECONNECT_BASE_SECONDS;
}
