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

    /*
     * One reader, and a table of what it is still waiting to hand back.
     *
     * There used to be no table: a synchronous request read the stream
     * itself.  That works right up until something starts the async
     * reader, and then two readers race for the same lines -- one of them
     * gets a partial frame, decides the daemon has gone, and tears down a
     * perfectly healthy connection.
     */
    GHashTable   *pending;      /* request id -> Pending */
    GQueue       *incoming;     /* ClawtEvent*, waiting to be emitted */
    GSource      *drain_source;
    GMainContext *context;      /* the context the reader is attached to */

    gboolean  auto_reconnect;
    GSource  *reconnect_source;
    guint     reconnect_delay;
};

G_DEFINE_FINAL_TYPE(ClawtClient, clawt_client, G_TYPE_OBJECT)

enum {
    SIGNAL_CONNECTED,
    SIGNAL_DISCONNECTED,
    SIGNAL_RESYNC,
    SIGNAL_EVENT,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void read_next(ClawtClient *self);
static gboolean try_reconnect(gpointer user_data);
static void schedule_reconnect(ClawtClient *self);

/*
 * One outstanding request.
 *
 * A synchronous caller waits on `done` while iterating the context; an
 * asynchronous one is handed its GTask when the reply lands.
 */
typedef struct {
    JsonNode *reply;        /* the whole frame, not just the payload */
    GError   *error;
    gboolean  done;
    GTask    *task;         /* set for an async request */
} Pending;

static void
pending_free(gpointer data)
{
    Pending *pending = data;

    g_clear_pointer(&pending->reply, json_node_unref);
    g_clear_error(&pending->error);
    g_clear_object(&pending->task);
    g_free(pending);
}

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
clawt_client_is_reconnecting(ClawtClient *self)
{
    g_return_val_if_fail(CLAWT_IS_CLIENT(self), FALSE);

    return self->reconnect_source != NULL;
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

/*
 * Hands queued events to the application, one turn of the loop later.
 */
static gboolean
drain_events(gpointer user_data)
{
    ClawtClient *self = user_data;

    g_clear_pointer(&self->drain_source, g_source_unref);

    for (;;) {
        g_autoptr(ClawtEvent) event = g_queue_pop_head(self->incoming);

        if (event == NULL)
            break;

        g_signal_emit(self, signals[SIGNAL_EVENT], 0, event);
    }

    return G_SOURCE_REMOVE;
}

static void
dispatch_event(ClawtClient *self, JsonNode *frame)
{
    JsonObject *payload = clawt_ipc_frame_get_payload(frame);
    g_autoptr(ClawtEvent) event = NULL;
    const gchar *kind;

    if (payload == NULL || !json_object_has_member(payload, "kind"))
        return;

    if (json_node_get_value_type(json_object_get_member(payload, "kind")) !=
        G_TYPE_STRING)
        return;

    kind = json_object_get_string_member(payload, "kind");

    event = clawt_event_new(kind,
                            json_object_has_member(payload, "subject")
                                ? json_object_get_string_member(payload,
                                                                "subject")
                                : NULL);

    if (json_object_has_member(payload, "ts") &&
        json_node_get_value_type(json_object_get_member(payload, "ts")) ==
            G_TYPE_INT64)
        clawt_event_set_timestamp(event,
                                  json_object_get_int_member(payload, "ts"));

    if (json_object_has_member(payload, "cursor") &&
        json_node_get_value_type(json_object_get_member(payload, "cursor")) ==
            G_TYPE_INT64) {
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

    if (json_object_has_member(payload, "detail") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(payload, "detail"))) {
        JsonObject *detail = json_object_get_object_member(payload, "detail");
        g_autoptr(GList) members = json_object_get_members(detail);
        GList *l;

        for (l = members; l != NULL; l = l->next)
            clawt_event_set_detail(
                event, l->data,
                json_object_get_string_member(detail, l->data));
    }

    /*
     * Queued rather than emitted here.
     *
     * A handler is application code: it may open a dialog, run a nested
     * loop, or make a request and wait.  Running it inside the reader's
     * own callback makes all of that re-entrant on the one socket.
     * Delivering from an idle costs a turn of the loop and means a
     * handler always runs with the reader idle and armed.
     */
    g_queue_push_tail(self->incoming, g_steal_pointer(&event));

    /*
     * Attached to this client's own context, not with g_idle_add(), which
     * would put it on the global default -- where an embedded client's
     * loop never looks, so events would simply never be delivered.
     */
    if (self->drain_source == NULL) {
        self->drain_source = g_idle_source_new();

        g_source_set_callback(self->drain_source, drain_events,
                              g_object_ref(self), g_object_unref);
        g_source_attach(self->drain_source, self->context);
    }
}

/*
 * Hands a reply to whoever is waiting for it.
 *
 * A reply with no waiter is dropped rather than warned about: a caller
 * that timed out and gave up is entitled to have gone away.
 */
static void
complete_pending(ClawtClient *self, JsonNode *frame)
{
    const gchar *id = clawt_ipc_frame_get_id(frame);
    Pending *pending;

    if (id == NULL)
        return;

    pending = g_hash_table_lookup(self->pending, id);

    if (pending == NULL)
        return;

    pending->reply = json_node_ref(frame);
    pending->done = TRUE;

    if (pending->task != NULL) {
        g_autoptr(GTask) task = g_steal_pointer(&pending->task);

        if (clawt_ipc_frame_is_error(frame)) {
            g_task_return_error(task, clawt_ipc_frame_to_error(frame));
        } else {
            JsonObject *payload = clawt_ipc_frame_get_payload(frame);

            g_task_return_pointer(
                task,
                payload != NULL
                    ? json_node_init_object(json_node_alloc(), payload)
                    : json_node_new(JSON_NODE_OBJECT),
                (GDestroyNotify)json_node_unref);
        }

        g_hash_table_remove(self->pending, id);
    }
}

/*
 * Fails everything still waiting when the connection goes.
 *
 * Without this a synchronous caller would sit out its full timeout after
 * the daemon had already gone -- two minutes of a frozen window for
 * something that is already known.
 */
static void
fail_all_pending(ClawtClient *self, const gchar *reason)
{
    g_autoptr(GList) ids = g_hash_table_get_keys(self->pending);
    GList *l;

    for (l = ids; l != NULL; l = l->next) {
        Pending *pending = g_hash_table_lookup(self->pending, l->data);

        if (pending == NULL || pending->done)
            continue;

        pending->done = TRUE;
        pending->error = g_error_new_literal(CLAWT_ERROR,
                                             CLAWT_ERROR_NOT_CONNECTED,
                                             reason);

        if (pending->task != NULL) {
            g_autoptr(GTask) task = g_steal_pointer(&pending->task);

            g_task_return_error(task, g_error_copy(pending->error));
            g_hash_table_remove(self->pending, l->data);
        }
    }
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

    fail_all_pending(self, "the daemon closed the connection");

    /*
     * Scheduled *before* the signal, not after.
     *
     * A subscriber's whole reason to exist is to draw the state, and
     * clawt_client_is_reconnecting() answered FALSE inside the handler
     * because the retry had not been arranged yet -- so both clients
     * were told the connection had gone and then found nothing to say
     * about it.  Found by killing a daemon under the real GTK client and
     * reading the probe, not by any test: the unit test samples the
     * state in a polling loop, which is always after this returns.
     */
    if (self->auto_reconnect && self->reconnect_source == NULL) {
        if (self->reconnect_delay == 0)
            self->reconnect_delay = RECONNECT_BASE_SECONDS;

        schedule_reconnect(self);
    }

    g_signal_emit(self, signals[SIGNAL_DISCONNECTED], 0);
}

/*
 * The retry timer, on the client's *own* context.
 *
 * g_timeout_add_seconds() attaches to the global default, which is not
 * the context this client's reader is on unless somebody happened to
 * make it so -- and dispatching a source pushes nothing, so a
 * handle_disconnect() reached from the reader has no thread-default to
 * inherit either.  An embedded host running its own loop therefore had a
 * client that lost its daemon and never once tried to get it back: the
 * window stays as it was, no event arrives, and nothing says why.  That
 * is the same trap already recorded here for two timers, an idle, a
 * GTask and an async exec.
 *
 * The source is kept rather than its id, because g_source_remove() only
 * knows about the default context.
 */
static void
schedule_reconnect(ClawtClient *self)
{
    self->reconnect_source = g_timeout_source_new_seconds(
        self->reconnect_delay);
    g_source_set_callback(self->reconnect_source, try_reconnect, self, NULL);
    g_source_attach(self->reconnect_source,
                    (self->context != NULL) ? self->context
                                            : g_main_context_default());
}

static gboolean
try_reconnect(gpointer user_data)
{
    ClawtClient *self = user_data;
    g_autoptr(GError) error = NULL;

    g_clear_pointer(&self->reconnect_source, g_source_unref);

    if (clawt_client_connect(self, &error)) {
        self->reconnect_delay = RECONNECT_BASE_SECONDS;

        if (self->subscribed) {
            gboolean resumed = TRUE;

            /*
             * Resumed from the last cursor actually seen -- and the
             * answer is passed on.
             *
             * The daemon replays from a bounded buffer, so a cursor that
             * fell off it comes back `resumed: false`, which means this
             * client has a hole.  For a long time nothing here told the
             * application, which then showed stale state indefinitely:
             * precisely the shape a reconnect after a long outage takes,
             * which is when it matters most.
             *
             * ::resync now says so, and both graphical clients answer it
             * by re-reading rather than waiting for events that are not
             * coming.
             */
            clawt_client_subscribe(self, self->cursor, &resumed, NULL);

            if (!resumed) {
                g_warning("client: the daemon could not resume from cursor "
                          "%" G_GUINT64_FORMAT "; this client has missed "
                          "events and its view may be stale", self->cursor);

                g_signal_emit(self, signals[SIGNAL_RESYNC], 0);
            }
        }

        return G_SOURCE_REMOVE;
    }

    /*
     * Asked again, because it can have changed while this attempt was in
     * flight.  A connect blocks for as long as the far end takes -- up
     * to the whole request timeout -- and clawt_client_set_auto_reconnect()
     * turned off during that window used to be ignored: the retry loop
     * carried on for ever, each turn holding the caller's context for
     * another two minutes.  Turning it off is how a caller says stop,
     * and it has to work from inside the attempt as well as before it.
     */
    if (!self->auto_reconnect)
        return G_SOURCE_REMOVE;

    /* Doubling, capped: a daemon that is down stays down for a while. */
    self->reconnect_delay = MIN(self->reconnect_delay * 2,
                                RECONNECT_MAX_SECONDS);
    schedule_reconnect(self);

    return G_SOURCE_REMOVE;
}

static void
on_line_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(ClawtClient) self = user_data;
    g_autofree gchar *line = NULL;
    g_autoptr(JsonNode) frame = NULL;

    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                                result, NULL, NULL);

    if (line == NULL) {
        handle_disconnect(self);
        return;
    }

    /*
     * Re-armed before anything is dispatched, not after.
     *
     * Whatever runs below may issue a request of its own and wait for the
     * answer -- a client refreshing its view when an event arrives does
     * exactly that.  With the re-arm at the end of this function there
     * was no outstanding read while that happened, so nothing could read
     * the reply and the nested request sat there until it timed out,
     * taking the outer one with it.
     */
    read_next(self);

    frame = clawt_ipc_frame_from_line(line, NULL);

    if (frame != NULL) {
        if (g_strcmp0(clawt_ipc_frame_get_kind(frame), "event") == 0)
            dispatch_event(self, frame);
        else
            complete_pending(self, frame);
    }
}

static void
read_next(ClawtClient *self)
{
    if (self->input == NULL)
        return;

    /*
     * Armed on the client's *own* context, always.
     *
     * g_data_input_stream_read_line_async() attaches to whatever is
     * thread-default at the moment it is called, and dispatching a source
     * pushes nothing -- so a reconnect, which happens from a timeout
     * callback, armed the reader on the *global* default instead.  The
     * socket then belonged to a loop nobody was running: the client
     * reconnected, said hello, and never received another line.  That is
     * the same failure the reconnect timer had, one function along, and
     * it is invisible in a client whose context happens to be the
     * default -- which is both graphical clients and neither embedded
     * one.
     *
     * A reference for the life of the read.  There is always exactly one
     * outstanding -- every completion re-arms -- so dropping the last
     * reference to a connected client left GIO holding a pointer to a
     * freed object, and the next line to arrive read it.  The server's
     * Client is reference counted for the same reason.
     */
    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);

    g_data_input_stream_read_line_async(self->input, G_PRIORITY_DEFAULT,
                                        NULL, on_line_read,
                                        g_object_ref(self));

    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);
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

/*
 * The handshake, over the same reader as everything else.
 *
 * It used to read the socket directly, which deadlocks an in-process
 * client: the daemon that must answer runs on the thread now blocked
 * waiting for its answer.  Going through the ordinary request path means
 * the context keeps turning, so a host that embeds the daemon and
 * connects to it -- cmacs, most of all -- works.
 */
static gboolean
say_hello(ClawtClient *self, GError **error)
{
    g_autoptr(JsonNode) payload = NULL;
    g_autoptr(JsonNode) reply = NULL;

    if (self->token != NULL) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "token");
        json_builder_add_string_value(builder, self->token);
        json_builder_end_object(builder);

        payload = json_builder_get_root(builder);
    }

    reply = clawt_client_request(self, "control.hello",
                                 g_steal_pointer(&payload), error);

    return reply != NULL;
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

        /*
         * A network connection is armed with keepalive; a unix one has
         * nothing to arm.
         *
         * Without it a route that goes away -- a laptop that suspends, a
         * tailnet that reconnects somewhere else -- leaves this end
         * holding a read that will never complete.  Nothing fails, so
         * handle_disconnect() never runs, so the reconnect this client
         * already knows how to do never happens: the window stays
         * connected, shows no new message for the rest of the day, and
         * the next thing typed into it waits out the request timeout and
         * is lost.  Measured against a black-holed proxy: 150 seconds
         * with `connected=yes` throughout and not one of the events sent
         * in that time.
         */
        {
            GSocket *socket = g_socket_connection_get_socket(connection);
            g_autoptr(GError) local = NULL;

            if (socket != NULL &&
                !clawt_ipc_socket_keepalive(socket, &local))
                g_warning("ipc: %s; a connection to %s:%u that goes away "
                          "may not be noticed", local->message, self->host,
                          self->port);
        }

        self->stream = G_IO_STREAM(g_object_ref(connection));
    }

    self->connection = g_object_ref(connection);
    self->input = g_data_input_stream_new(
        g_io_stream_get_input_stream(self->stream));
    g_data_input_stream_set_newline_type(self->input,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);
    self->output = g_io_stream_get_output_stream(self->stream);

    /*
     * The reader starts before the handshake, and the handshake goes
     * through it like every other request.  One reader owns the stream
     * for the whole life of the connection; nothing else ever reads it.
     */
    if (self->context == NULL) {
        GMainContext *context = g_main_context_get_thread_default();

        self->context = g_main_context_ref(context != NULL
                                           ? context
                                           : g_main_context_default());
    }

    read_next(self);

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

    if (self->reconnect_source != NULL) {
        g_source_destroy(self->reconnect_source);
        g_clear_pointer(&self->reconnect_source, g_source_unref);
    }

    if (self->stream == NULL)
        return;

    g_io_stream_close(self->stream, NULL, NULL);

    g_clear_object(&self->input);
    g_clear_object(&self->connection);
    g_clear_object(&self->stream);
    self->output = NULL;

    /*
     * Outstanding requests are failed, not abandoned.  An async caller
     * whose GTask is never completed waits for ever -- a spinner that
     * never stops -- and a synchronous one sat out its full two-minute
     * timeout before reporting the wrong reason.
     */
    fail_all_pending(self, "the connection was closed");
}

/* ── Requests ────────────────────────────────────────────────────── */

/*
 * Registers a request and sends it.
 *
 * Returns: (transfer none) (nullable): the pending slot, or %NULL
 */
static Pending *
begin_request(ClawtClient *self, const gchar *kind, JsonNode *payload,
              gchar **out_id, GError **error)
{
    g_autoptr(JsonNode) request = NULL;
    g_autofree gchar *id = NULL;
    Pending *pending;

    if (self->stream == NULL) {
        /*
         * g_clear_pointer, not json_node_unref: the payload is optional
         * and a plain unref of %NULL trips an assertion inside json-glib,
         * which is a confusing way to be told you are not connected.
         */
        g_clear_pointer(&payload, json_node_unref);

        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "not connected to the daemon");
        return NULL;
    }

    id = g_strdup_printf("r%u", ++self->request_serial);
    request = clawt_ipc_request_new(kind, id);

    if (payload != NULL)
        clawt_ipc_frame_set_payload(request, payload);

    pending = g_new0(Pending, 1);
    g_hash_table_insert(self->pending, g_strdup(id), pending);

    if (!send_frame(self, request, error)) {
        g_hash_table_remove(self->pending, id);
        return NULL;
    }

    *out_id = g_steal_pointer(&id);

    return pending;
}

static JsonNode *
finish_request(ClawtClient *self, const gchar *id, const gchar *kind,
               gint timeout_seconds, GError **error)
{
    Pending *pending = g_hash_table_lookup(self->pending, id);
    JsonNode *result = NULL;

    if (pending == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the request went missing before it was answered");
        return NULL;
    }

    if (pending->error != NULL) {
        g_propagate_error(error, g_steal_pointer(&pending->error));
    } else if (!pending->done) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                    "the daemon did not answer %s within %d seconds",
                    kind, timeout_seconds);
    } else if (clawt_ipc_frame_is_error(pending->reply)) {
        g_propagate_error(error, clawt_ipc_frame_to_error(pending->reply));
    } else {
        JsonObject *payload = clawt_ipc_frame_get_payload(pending->reply);

        result = (payload != NULL)
                 ? json_node_init_object(json_node_alloc(), payload)
                 : json_node_new(JSON_NODE_OBJECT);
    }

    g_hash_table_remove(self->pending, id);

    return result;
}

JsonNode *
clawt_client_request(ClawtClient  *self,
                     const gchar  *kind,
                     JsonNode     *payload,
                     GError      **error)
{
    return clawt_client_request_full(self, kind, payload,
                                     REQUEST_TIMEOUT_SECONDS, error);
}

JsonNode *
clawt_client_request_full(ClawtClient  *self,
                          const gchar  *kind,
                          JsonNode     *payload,
                          gint          timeout_seconds,
                          GError      **error)
{
    g_autofree gchar *id = NULL;
    Pending *pending;
    gint64 deadline;

    g_return_val_if_fail(CLAWT_IS_CLIENT(self), NULL);
    g_return_val_if_fail(kind != NULL, NULL);

    if (timeout_seconds <= 0)
        timeout_seconds = REQUEST_TIMEOUT_SECONDS;

    pending = begin_request(self, kind, payload, &id, error);

    if (pending == NULL)
        return NULL;

    deadline = g_get_monotonic_time() +
               ((gint64)timeout_seconds * G_USEC_PER_SEC);

    /*
     * Iterate the context rather than reading the socket.
     *
     * The reader is already running there and owns the stream; a second
     * read here would race it for lines. Iterating also means a caller
     * inside a main loop -- the GTK client, most of all -- keeps
     * dispatching everything else while it waits, instead of freezing.
     */
    while (!pending->done && g_get_monotonic_time() < deadline) {
        if (self->stream == NULL)
            break;

        g_main_context_iteration(self->context, TRUE);

        /*
         * Looked up again rather than reused: iterating the context can
         * complete this request, and completing it removes and frees the
         * slot.  The table itself is never rebuilt -- the entry is what
         * goes away.
         */
        pending = g_hash_table_lookup(self->pending, id);

        if (pending == NULL)
            break;
    }

    return finish_request(self, id, kind, timeout_seconds, error);
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
    g_autofree gchar *id = NULL;
    g_autoptr(GError) error = NULL;
    Pending *pending;

    g_return_if_fail(CLAWT_IS_CLIENT(self));
    g_return_if_fail(kind != NULL);

    task = g_task_new(self, cancellable, callback, user_data);

    /*
     * No thread.  The reply is delivered by the reader that is already
     * running on this context; handing the whole request to a worker
     * thread would put a second reader on the same stream, which is the
     * bug this replaced.
     */
    pending = begin_request(self, kind, payload, &id, &error);

    if (pending == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    pending->task = g_object_ref(task);

    /*
     * A reply that arrived while the request was being registered is
     * already sitting in the slot; hand it over rather than waiting for a
     * reader that has nothing left to read.
     */
    if (pending->done)
        complete_pending(self, pending->reply);
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

    if (self->drain_source != NULL) {
        g_source_destroy(self->drain_source);
        g_clear_pointer(&self->drain_source, g_source_unref);
    }

    /*
     * The retry timer holds a bare pointer to this object, so it must go
     * before the object does -- and destroy, not merely unref: an
     * attached source is owned by its context as well.
     */
    if (self->reconnect_source != NULL) {
        g_source_destroy(self->reconnect_source);
        g_clear_pointer(&self->reconnect_source, g_source_unref);
    }

    g_queue_free_full(g_steal_pointer(&self->incoming),
                      (GDestroyNotify)clawt_event_free);
    g_clear_pointer(&self->pending, g_hash_table_unref);
    g_clear_pointer(&self->context, g_main_context_unref);
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
     * ClawtClient::resync:
     * @self: the client
     *
     * The daemon could not replay from where this client had got to.
     *
     * A reconnect asks to resume from the last cursor actually seen, and
     * the daemon replays from a bounded buffer -- so an outage longer
     * than that buffer comes back `resumed: false`, which means this
     * client has a hole in its view.  It is precisely the shape a
     * reconnect after a long outage takes, which is when it matters
     * most, and until now the only thing that happened was a warning in
     * the journal: the window went on showing state from before the
     * outage indefinitely, with nothing to say so.
     *
     * A client answering this re-reads whatever it holds rather than
     * waiting for events that are not coming.
     */
    signals[SIGNAL_RESYNC] =
        g_signal_new("resync", CLAWT_TYPE_CLIENT, G_SIGNAL_RUN_LAST, 0,
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
    self->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          pending_free);
    self->incoming = g_queue_new();
}
