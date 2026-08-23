/*
 * clawt-ipc-server.c - Where clients connect
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ipc/clawt-ipc-server.h"
#include "ipc/clawt-ipc-proto.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include <errno.h>

/*
 * How much a slow client may have queued before it is disconnected.
 *
 * A client that stops reading -- a paused GUI, a terminal scrolled and
 * suspended -- would otherwise grow the daemon's memory without bound
 * while looking healthy. Dropping it is recoverable; running the daemon
 * out of memory is not.
 */
/*
 * Comfortably above CLAWT_IPC_MAX_FRAME_BYTES, because a single legal
 * reply may be that large.  A cap below the largest permitted frame
 * disconnects a client that is reading perfectly well, and blames it for
 * not reading.
 */
#define MAX_PENDING_BYTES ((CLAWT_IPC_MAX_FRAME_BYTES * 2) + (1024 * 1024))

/*
 * Reference counted, because a pending async read outlives the client it
 * belongs to.
 *
 * A client that disconnects mid-read has its connection dropped from the
 * server's list immediately; the read then completes and hands the
 * callback a pointer to memory that has already gone.  The callback holds
 * a reference for exactly as long as the read is outstanding, so the
 * struct survives to be told the read failed.
 */
typedef struct {
    gint              ref_count;
    ClawtIpcServer   *server;      /* unowned */
    GSocketConnection *connection;
    GDataInputStream *input;
    GOutputStream    *output;
    gboolean          subscribed;
    gboolean          authenticated;
    gboolean          closing;
    GString          *pending;     /* queued outbound bytes, not yet sent */
    GBytes           *writing_bytes; /* the immutable buffer being written */
    gboolean          writing;
} Client;

struct _ClawtIpcServer {
    GObject parent_instance;

    gchar          *socket_path;
    GSocketService *service;
    GPtrArray      *clients;      /* Client*, owned */

    /*
     * More than one, because a tailnet address is bound beside whatever
     * daemon.tcp_address says rather than instead of it: the two are
     * different decisions, and a machine on a tailnet usually wants the
     * tailnet address and nothing else.
     */
    GPtrArray *tcp_addresses;  /* gchar*, owned */
    /*
     * Addresses whose bind failing is a warning rather than a refusal to
     * start.  The tailnet address is one: it is a convenience the daemon
     * offers, and a fleet that would not start because a second daemon
     * already held that port -- or because tailscaled restarted between
     * the lookup and the bind -- is a far worse failure than not being
     * reachable from a laptop.
     */
    GPtrArray *optional_addresses;  /* gchar*, owned; subset by value */
    /*
     * What actually bound, filled in by start().  Kept because an
     * optional address may not have, and anything reporting where the
     * daemon can be reached has to say what happened rather than what
     * was asked for.
     */
    GPtrArray *bound_addresses;     /* gchar*, owned */
    guint16    tcp_port;
    gchar     *tcp_token;
    gchar     *tls_certificate;
    gchar     *tls_key;

    ClawtEventBus *bus;
    gulong         bus_handler;

    ClawtIpcHandler handler;
    gpointer        handler_data;
    GDestroyNotify  handler_destroy;
};

G_DEFINE_FINAL_TYPE(ClawtIpcServer, clawt_ipc_server, G_TYPE_OBJECT)

static void read_next(Client *client);
static void flush_pending(Client *client);
static void client_unref(gpointer data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Client, client_unref)

ClawtIpcServer *
clawt_ipc_server_new(const gchar *socket_path)
{
    ClawtIpcServer *self;

    g_return_val_if_fail(socket_path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_IPC_SERVER, NULL);
    self->socket_path = clawt_expand_path(socket_path);
    self->tcp_addresses = g_ptr_array_new_with_free_func(g_free);
    self->optional_addresses = g_ptr_array_new_with_free_func(g_free);
    self->bound_addresses = g_ptr_array_new_with_free_func(g_free);

    return self;
}

void
clawt_ipc_server_set_handler(ClawtIpcServer  *self,
                             ClawtIpcHandler  handler,
                             gpointer         user_data,
                             GDestroyNotify   destroy)
{
    g_return_if_fail(CLAWT_IS_IPC_SERVER(self));

    if (self->handler_destroy != NULL && self->handler_data != NULL)
        self->handler_destroy(self->handler_data);

    self->handler = handler;
    self->handler_data = user_data;
    self->handler_destroy = destroy;
}

void
clawt_ipc_server_set_tcp(ClawtIpcServer *self, const gchar *address,
                         guint16 port, const gchar *token)
{
    g_return_if_fail(CLAWT_IS_IPC_SERVER(self));

    g_free(self->tcp_token);

    g_ptr_array_set_size(self->tcp_addresses, 0);

    if (address != NULL && *address != '\0')
        g_ptr_array_add(self->tcp_addresses, g_strdup(address));

    self->tcp_port = port;
    self->tcp_token = g_strdup(token);
}

void
clawt_ipc_server_add_tcp_address(ClawtIpcServer *self, const gchar *address)
{
    guint i;

    g_return_if_fail(CLAWT_IS_IPC_SERVER(self));
    g_return_if_fail(address != NULL && *address != '\0');

    /*
     * Binding the same address twice fails with EADDRINUSE against
     * ourselves, which reads as another daemon already running -- the
     * one diagnosis that sends a person hunting for a process that is
     * not there.
     */
    for (i = 0; i < self->tcp_addresses->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->tcp_addresses, i),
                      address) == 0)
            return;
    }

    g_ptr_array_add(self->tcp_addresses, g_strdup(address));
}

void
clawt_ipc_server_set_optional_tcp_address(ClawtIpcServer *self,
                                          const gchar    *address)
{
    g_return_if_fail(CLAWT_IS_IPC_SERVER(self));
    g_return_if_fail(address != NULL && *address != '\0');

    clawt_ipc_server_add_tcp_address(self, address);
    g_ptr_array_add(self->optional_addresses, g_strdup(address));
}

static gboolean
address_is_optional(ClawtIpcServer *self, const gchar *address)
{
    guint i;

    for (i = 0; i < self->optional_addresses->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->optional_addresses, i),
                      address) == 0)
            return TRUE;
    }

    return FALSE;
}

gboolean
clawt_ipc_server_is_listening_on(ClawtIpcServer *self, const gchar *address)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_IPC_SERVER(self), FALSE);
    g_return_val_if_fail(address != NULL, FALSE);

    for (i = 0; i < self->bound_addresses->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->bound_addresses, i),
                      address) == 0)
            return TRUE;
    }

    return FALSE;
}

void
clawt_ipc_server_set_tls(ClawtIpcServer *self, const gchar *certificate_path,
                         const gchar *key_path)
{
    g_return_if_fail(CLAWT_IS_IPC_SERVER(self));

    g_free(self->tls_certificate);
    g_free(self->tls_key);

    self->tls_certificate = g_strdup(certificate_path);
    self->tls_key = g_strdup(key_path);
}

const gchar *
clawt_ipc_server_get_socket_path(ClawtIpcServer *self)
{
    g_return_val_if_fail(CLAWT_IS_IPC_SERVER(self), NULL);

    return self->socket_path;
}

guint
clawt_ipc_server_count_clients(ClawtIpcServer *self)
{
    g_return_val_if_fail(CLAWT_IS_IPC_SERVER(self), 0);

    return self->clients->len;
}

/* ── Client bookkeeping ──────────────────────────────────────────── */

static void
client_free(gpointer data)
{
    Client *client = data;

    if (client->pending != NULL)
        g_string_free(client->pending, TRUE);

    g_clear_pointer(&client->writing_bytes, g_bytes_unref);

    g_clear_object(&client->input);
    g_clear_object(&client->connection);
    g_free(client);
}

static Client *
client_ref(Client *client)
{
    client->ref_count++;

    return client;
}

static void
client_unref(gpointer data)
{
    Client *client = data;

    if (--client->ref_count > 0)
        return;

    client_free(client);
}

static void
client_close(Client *client)
{
    if (client->closing)
        return;

    client->closing = TRUE;

    if (client->connection != NULL)
        g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);

    g_ptr_array_remove(client->server->clients, client);
}

static void
on_write_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(Client) client = user_data;
    g_autoptr(GError) error = NULL;
    gsize written = 0;

    if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result,
                                          &written, &error)) {
        client->writing = FALSE;
        g_clear_pointer(&client->writing_bytes, g_bytes_unref);

        /*
         * A write failure is the ordinary way a client goes away -- the
         * terminal was closed, the GUI quit.  Dropping it quietly is
         * right; the daemon carries on.
         */
        client_close(client);
        return;
    }

    client->writing = FALSE;
    g_clear_pointer(&client->writing_bytes, g_bytes_unref);

    /* Anything queued while that write was in flight goes next. */
    if (client->pending->len > 0)
        flush_pending(client);
}

static void
flush_pending(Client *client)
{
    gsize length = 0;

    if (client->writing || client->closing || client->pending->len == 0)
        return;

    /*
     * The bytes being written are detached from the queue first.
     *
     * g_output_stream_write_all_async() keeps the pointer and length it
     * is given for as long as the write takes, which for a peer that has
     * stopped reading is many main-loop turns.  Appending to the same
     * GString in that window can reallocate it, and GIO then reads freed
     * memory -- sending whatever now occupies it to the peer, which may
     * be another client's data.  Handing over an immutable copy costs one
     * memcpy per flush and removes the whole class of problem.
     */
    g_clear_pointer(&client->writing_bytes, g_bytes_unref);
    client->writing_bytes = g_bytes_new(client->pending->str,
                                        client->pending->len);
    g_string_truncate(client->pending, 0);

    client->writing = TRUE;

    /*
     * The pointer and the length are taken before the call, not inside
     * its argument list: C does not define the order arguments are
     * evaluated, so passing g_bytes_get_data(..., &length) alongside
     * `length` can hand over a length read before it was set.
     */
    {
        const guchar *data = g_bytes_get_data(client->writing_bytes,
                                              &length);

        g_output_stream_write_all_async(client->output, data, length,
                                        G_PRIORITY_DEFAULT, NULL,
                                        on_write_done, client_ref(client));
    }
}

static void
client_send(Client *client, JsonNode *frame)
{
    g_autofree gchar *line = NULL;

    if (client->closing || frame == NULL)
        return;

    line = clawt_ipc_frame_to_line(frame);

    if (client->pending->len +
        (client->writing_bytes != NULL
             ? g_bytes_get_size(client->writing_bytes) : 0) +
        strlen(line) > MAX_PENDING_BYTES) {
        g_info("ipc: a client stopped reading; closing its connection");
        client_close(client);
        return;
    }

    g_string_append(client->pending, line);
    flush_pending(client);
}

/* ── Requests ────────────────────────────────────────────────────── */

static JsonNode *
handle_builtin(ClawtIpcServer *self, Client *client, JsonNode *request,
               gboolean *handled)
{
    const gchar *kind = clawt_ipc_frame_get_kind(request);
    JsonObject *payload = clawt_ipc_frame_get_payload(request);

    *handled = TRUE;

    if (g_strcmp0(kind, "control.hello") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        if (self->tcp_token != NULL && !client->authenticated) {
            const gchar *token = clawt_ipc_payload_string(payload, "token");

            if (g_strcmp0(token, self->tcp_token) != 0)
                return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                           "that token was not accepted");

            client->authenticated = TRUE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "version");
        json_builder_add_int_value(builder, CLAWT_IPC_VERSION);
        json_builder_set_member_name(builder, "daemon");
        json_builder_add_string_value(builder, CLAWT_VERSION_STRING);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "control.subscribe") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(GPtrArray) missed = NULL;
        gboolean complete = TRUE;
        guint64 cursor;
        guint i;

        client->subscribed = TRUE;
        cursor = (guint64)clawt_ipc_payload_int(payload, "cursor", 0);

        if (self->bus != NULL)
            missed = clawt_event_bus_replay(self->bus, cursor, &complete);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "cursor");
        json_builder_add_int_value(
            builder, self->bus != NULL
                     ? (gint64)clawt_event_bus_get_cursor(self->bus) : 0);

        /*
         * Told plainly when the replay could not be completed.  A client
         * that quietly receives a partial replay carries the hole for the
         * rest of its life and shows stale state it has no way to notice.
         */
        json_builder_set_member_name(builder, "resumed");
        json_builder_add_boolean_value(builder, complete);
        json_builder_end_object(builder);

        {
            JsonNode *reply = clawt_ipc_response_new(
                request, json_builder_get_root(builder));

            client_send(client, reply);
            json_node_unref(reply);
        }

        for (i = 0; missed != NULL && i < missed->len; i++) {
            JsonNode *frame = clawt_ipc_event_new(
                g_ptr_array_index(missed, i));

            client_send(client, frame);
            json_node_unref(frame);
        }

        /* Already answered above. */
        return NULL;
    }

    *handled = FALSE;
    return NULL;
}

static void
on_line_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(Client) client = user_data;
    ClawtIpcServer *self = client->server;
    g_autofree gchar *line = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) request = NULL;
    JsonNode *reply = NULL;
    gsize length = 0;
    gboolean handled = FALSE;

    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                                result, &length, &error);

    if (line == NULL) {
        client_close(client);
        return;
    }

    if (length > CLAWT_IPC_MAX_FRAME_BYTES) {
        reply = clawt_ipc_error_new(NULL, CLAWT_ERROR_PROTOCOL,
                                    "that frame is too large");
        client_send(client, reply);
        json_node_unref(reply);
        client_close(client);
        return;
    }

    if (length == 0) {
        /* A blank line is a keepalive, not a malformed frame. */
        read_next(client);
        return;
    }

    request = clawt_ipc_frame_from_line(line, &error);

    if (request == NULL || !clawt_ipc_frame_validate(request, &error)) {
        reply = clawt_ipc_error_new(request, CLAWT_ERROR_PROTOCOL,
                                    error->message);
        client_send(client, reply);
        json_node_unref(reply);

        /*
         * One bad frame does not end the connection.  A client that sent
         * something malformed usually recovers, and dropping it turns a
         * message-level mistake into a session-level one.
         */
        read_next(client);
        return;
    }

    /*
     * The token is checked before anything is dispatched, including the
     * built-ins.  It used to be checked only in the fall-through path,
     * and control.subscribe is answered inside handle_builtin() -- so an
     * unauthenticated TCP peer could subscribe to the whole event stream
     * without ever saying control.hello.
     */
    if (self->tcp_token != NULL && !client->authenticated &&
        g_strcmp0(clawt_ipc_frame_get_kind(request), "control.hello") != 0) {
        reply = clawt_ipc_error_new(
            request, CLAWT_ERROR_AUTH,
            "say control.hello with a token before anything else");
        client_send(client, reply);
        json_node_unref(reply);
        read_next(client);
        return;
    }

    reply = handle_builtin(self, client, request, &handled);

    if (!handled) {
        if (self->handler != NULL) {
            reply = self->handler(request, self->handler_data);
        } else {
            reply = clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                        "this daemon has no handler wired "
                                        "up");
        }
    }

    if (reply != NULL) {
        client_send(client, reply);
        json_node_unref(reply);
    }

    read_next(client);
}

static void
read_next(Client *client)
{
    if (client->closing)
        return;

    g_data_input_stream_read_line_async(client->input, G_PRIORITY_DEFAULT,
                                        NULL, on_line_read,
                                        client_ref(client));
}

static gboolean
on_incoming(GSocketService *service, GSocketConnection *connection,
            GObject *source, gpointer user_data)
{
    ClawtIpcServer *self = user_data;
    Client *client;
    gboolean is_unix;

    (void)service;
    (void)source;

    client = g_new0(Client, 1);
    client->ref_count = 1;
    client->server = self;
    client->connection = g_object_ref(connection);
    client->input = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_data_input_stream_set_newline_type(client->input,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);
    client->output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    client->pending = g_string_new(NULL);

    /*
     * A unix client is already authenticated by the socket's permissions:
     * it could only connect at all by being the owner.  A TCP client has
     * proved nothing yet, so it must present the token first.
     */
    {
        /*
         * get_local_address is (transfer full), so the address has to be
         * released -- once per connection, which adds up on a daemon that
         * has been running for a while.
         */
        g_autoptr(GSocketAddress) local =
            g_socket_connection_get_local_address(connection, NULL);

        is_unix = G_IS_UNIX_SOCKET_ADDRESS(local);
    }

    client->authenticated = is_unix;

    g_ptr_array_add(self->clients, client);
    read_next(client);

    return TRUE;
}

/* ── Events ──────────────────────────────────────────────────────── */

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtIpcServer *self = user_data;
    g_autoptr(JsonNode) frame = NULL;
    guint i;

    (void)bus;

    frame = clawt_ipc_event_new(event);

    /* Backwards: client_send() can close, which removes from the array. */
    for (i = self->clients->len; i > 0; i--) {
        Client *client = g_ptr_array_index(self->clients, i - 1);

        if (client->subscribed)
            client_send(client, frame);
    }
}

void
clawt_ipc_server_attach_bus(ClawtIpcServer *self, ClawtEventBus *bus)
{
    g_return_if_fail(CLAWT_IS_IPC_SERVER(self));
    g_return_if_fail(CLAWT_IS_EVENT_BUS(bus));

    if (self->bus != NULL && self->bus_handler != 0)
        g_signal_handler_disconnect(self->bus, self->bus_handler);

    /*
     * A reference rather than a borrowed pointer.  The handler has to be
     * disconnected at dispose time, and disconnecting from a bus that was
     * finalized first is a use-after-free -- which is exactly what happens
     * when a caller's cleanup order differs from the daemon's.
     */
    g_clear_object(&self->bus);
    self->bus = g_object_ref(bus);
    self->bus_handler = g_signal_connect(bus, "event",
                                         G_CALLBACK(on_bus_event), self);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

/*
 * Removes a socket left behind by a daemon that did not shut down cleanly,
 * but only after checking nothing is listening on it: unlinking a live
 * socket would leave the running daemon's clients talking to a path that
 * no longer exists while this one silently takes over new connections.
 */
static gboolean
clear_stale_socket(const gchar *path, GError **error)
{
    g_autoptr(GSocketClient) probe_client = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketConnection) probe = NULL;

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return TRUE;

    address = g_unix_socket_address_new(path);
    probe_client = g_socket_client_new();
    probe = g_socket_client_connect(probe_client,
                                    G_SOCKET_CONNECTABLE(address), NULL,
                                    NULL);

    if (probe != NULL) {
        g_io_stream_close(G_IO_STREAM(probe), NULL, NULL);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "another clawtilla daemon is already listening on %s",
                    path);
        return FALSE;
    }

    if (g_unlink(path) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not remove the stale socket %s: %s",
                    path, g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_ipc_server_start(ClawtIpcServer *self, GError **error)
{
    g_autoptr(GSocketAddress) address = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(CLAWT_IS_IPC_SERVER(self), FALSE);

    if (self->service != NULL)
        return TRUE;

    if (!clawt_check_socket_path(self->socket_path, error))
        return FALSE;

    dir = g_path_get_dirname(self->socket_path);
    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    if (!clear_stale_socket(self->socket_path, error))
        return FALSE;

    self->service = g_socket_service_new();
    address = g_unix_socket_address_new(self->socket_path);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service),
                                       address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, NULL,
                                       NULL, error)) {
        g_clear_object(&self->service);
        g_prefix_error(error, "listening on %s: ", self->socket_path);
        return FALSE;
    }

    /*
     * 0600.  Everything a client may do -- read transcripts, run commands
     * on an agent's computer, edit the config -- goes through this socket,
     * so its permissions are the whole access control on the unix path.
     */
    if (g_chmod(self->socket_path, 0600) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not restrict %s to the owner: %s",
                    self->socket_path, g_strerror(errno));
        clawt_ipc_server_stop(self);
        return FALSE;
    }

    if (self->tcp_addresses->len > 0 && self->tcp_port != 0) {
        guint i;
        guint bound = 0;

        /*
         * Refused without a token rather than started insecurely.  A unix
         * socket is protected by file permissions; a TCP port is reachable
         * by anything that can route to it, and defaulting to open would
         * turn one careless config line into a remote shell.
         */
        if (self->tcp_token == NULL || *self->tcp_token == '\0') {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "a TCP listener needs a token: without one "
                                "it would accept anyone who can reach the "
                                "port");
            clawt_ipc_server_stop(self);
            return FALSE;
        }

        for (i = 0; i < self->tcp_addresses->len; i++) {
            const gchar *text = g_ptr_array_index(self->tcp_addresses, i);
            g_autoptr(GSocketAddress) tcp = NULL;
            g_autoptr(GInetAddress) inet = NULL;

            gboolean optional = address_is_optional(self, text);
            g_autoptr(GError) local = NULL;

            inet = g_inet_address_new_from_string(text);

            if (inet == NULL) {
                if (optional) {
                    g_warning("ipc: '%s' is not an address this can bind "
                              "to; carrying on without it", text);
                    continue;
                }

                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "'%s' is not an address this can bind to", text);
                clawt_ipc_server_stop(self);
                return FALSE;
            }

            tcp = g_inet_socket_address_new(inet, self->tcp_port);

            if (!g_socket_listener_add_address(
                    G_SOCKET_LISTENER(self->service), tcp,
                    G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL,
                    NULL, &local)) {
                /*
                 * The unix socket is the daemon's real interface, and it
                 * is already bound by the time this runs.  Refusing to
                 * start over an address nobody asked for by name would
                 * mean one stale process, or a tailnet that came up
                 * twice, taking the whole fleet down with it.
                 */
                if (optional) {
                    g_warning("ipc: could not listen on %s:%u (%s); "
                              "clients on other machines will not reach "
                              "this daemon", text, self->tcp_port,
                              local->message);
                    continue;
                }

                clawt_ipc_server_stop(self);
                g_propagate_prefixed_error(error, g_steal_pointer(&local),
                                           "listening on %s:%u: ", text,
                                           self->tcp_port);
                return FALSE;
            }

            bound++;
            g_ptr_array_add(self->bound_addresses, g_strdup(text));
            g_info("ipc: listening on %s:%u", text, self->tcp_port);
        }

        /*
         * Only worth saying when something is actually listening.  A
         * daemon that bound no network address at all was warning about
         * the security of a listener it did not have.
         */
        if (bound > 0 && self->tls_certificate == NULL)
            g_warning("ipc: the TCP listener has no TLS certificate, so "
                      "the token crosses the network in the clear");
    }

    g_signal_connect(self->service, "incoming", G_CALLBACK(on_incoming),
                     self);
    g_socket_service_start(self->service);

    g_info("ipc: listening on %s", self->socket_path);

    return TRUE;
}

void
clawt_ipc_server_stop(ClawtIpcServer *self)
{
    guint i;

    g_return_if_fail(CLAWT_IS_IPC_SERVER(self));

    if (self->service != NULL) {
        g_socket_service_stop(self->service);
        g_socket_listener_close(G_SOCKET_LISTENER(self->service));
        g_clear_object(&self->service);
    }

    for (i = self->clients->len; i > 0; i--)
        client_close(g_ptr_array_index(self->clients, i - 1));

    if (self->socket_path != NULL)
        g_unlink(self->socket_path);
}

static void
clawt_ipc_server_dispose(GObject *object)
{
    ClawtIpcServer *self = CLAWT_IPC_SERVER(object);

    if (self->bus != NULL && self->bus_handler != 0) {
        g_signal_handler_disconnect(self->bus, self->bus_handler);
        self->bus_handler = 0;
    }

    g_clear_object(&self->bus);

    if (self->handler_destroy != NULL && self->handler_data != NULL) {
        self->handler_destroy(self->handler_data);
        self->handler_destroy = NULL;
        self->handler_data = NULL;
    }

    clawt_ipc_server_stop(self);

    G_OBJECT_CLASS(clawt_ipc_server_parent_class)->dispose(object);
}

static void
clawt_ipc_server_finalize(GObject *object)
{
    ClawtIpcServer *self = CLAWT_IPC_SERVER(object);

    g_clear_pointer(&self->clients, g_ptr_array_unref);
    g_free(self->socket_path);
    g_clear_pointer(&self->tcp_addresses, g_ptr_array_unref);
    g_clear_pointer(&self->optional_addresses, g_ptr_array_unref);
    g_clear_pointer(&self->bound_addresses, g_ptr_array_unref);
    g_free(self->tcp_token);
    g_free(self->tls_certificate);
    g_free(self->tls_key);

    G_OBJECT_CLASS(clawt_ipc_server_parent_class)->finalize(object);
}

static void
clawt_ipc_server_class_init(ClawtIpcServerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_ipc_server_dispose;
    object_class->finalize = clawt_ipc_server_finalize;
}

static void
clawt_ipc_server_init(ClawtIpcServer *self)
{
    self->clients = g_ptr_array_new_with_free_func(client_unref);
}
