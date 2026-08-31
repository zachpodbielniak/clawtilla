/*
 * clawt-link.c - One live connection to one agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "link/clawt-link.h"

#include <string.h>

/*
 * A frame larger than this is refused rather than buffered.
 *
 * Without a bound, one agent sending an unterminated line makes the daemon
 * allocate until it is killed -- taking every other agent with it.  Eight
 * megabytes is far above any real message and far below trouble.
 */
#define MAX_FRAME_BYTES (8 * 1024 * 1024)

enum {
    SIGNAL_HELLO,
    SIGNAL_MESSAGE,
    SIGNAL_TYPING,
    SIGNAL_CLOSED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtLink {
    GObject parent_instance;

    GSocketConnection *connection;
    GDataInputStream  *input;
    GOutputStream     *output;
    GCancellable      *cancellable;

    gchar    *agent_id;
    gchar    *agent_name;

    gboolean  open;
    gboolean  reading;
    gint64    last_seen;

    ClawtLinkMcpHandler mcp_handler;
    gpointer            mcp_data;
    GDestroyNotify      mcp_destroy;
};

G_DEFINE_FINAL_TYPE(ClawtLink, clawt_link, G_TYPE_OBJECT)

static void start_read(ClawtLink *self);

/* ── Writing ─────────────────────────────────────────────────────── */

static gboolean
send_frame(ClawtLink         *self,
           LcBridgeFrameKind  kind,
           const gchar       *id,
           JsonObject        *payload,
           GError           **error)
{
    g_autoptr(JsonNode) envelope = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autofree gchar *line = NULL;
    gsize length = 0;

    if (!self->open || self->output == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "the link is closed");
        return FALSE;
    }

    envelope = lc_bridge_proto_encode(kind, id, payload);
    generator = json_generator_new();
    json_generator_set_root(generator, envelope);
    line = json_generator_to_data(generator, &length);

    if (line == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "could not serialise a frame");
        return FALSE;
    }

    if (!g_output_stream_write_all(self->output, line, length, NULL,
                                   self->cancellable, error))
        return FALSE;

    return g_output_stream_write_all(self->output, "\n", 1, NULL,
                                     self->cancellable, error);
}

static gboolean
send_raw_node(ClawtLink *self, JsonNode *node, GError **error)
{
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autofree gchar *line = NULL;
    gsize length = 0;

    if (!self->open || self->output == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "the link is closed");
        return FALSE;
    }

    json_generator_set_root(generator, node);
    line = json_generator_to_data(generator, &length);

    if (!g_output_stream_write_all(self->output, line, length, NULL,
                                   self->cancellable, error))
        return FALSE;

    return g_output_stream_write_all(self->output, "\n", 1, NULL,
                                     self->cancellable, error);
}

/* ── Reading ─────────────────────────────────────────────────────── */

static void
handle_hello(ClawtLink *self, JsonObject *payload)
{
    const gchar *agent_id = NULL;
    const gchar *agent_name = NULL;
    const gchar *token = NULL;

    if (payload == NULL) {
        clawt_link_send_error(self, 400, "hello carried no payload");
        return;
    }

    if (json_object_has_member(payload, "agent_id"))
        agent_id = json_object_get_string_member(payload, "agent_id");
    if (json_object_has_member(payload, "agent_name"))
        agent_name = json_object_get_string_member(payload, "agent_name");
    if (json_object_has_member(payload, "token"))
        token = json_object_get_string_member(payload, "token");

    if (agent_id == NULL) {
        clawt_link_send_error(self, 400, "hello named no agent");
        return;
    }

    /*
     * A second hello on an established link is refused rather than
     * honoured.  Letting a connection change identity mid-stream would mean
     * messages already queued for one agent being delivered to another.
     */
    if (self->agent_id != NULL) {
        clawt_link_send_error(self, 409,
                              "this link has already identified itself");
        return;
    }

    g_free(self->agent_name);
    self->agent_name = g_strdup(agent_name);

    /*
     * The server decides whether to accept, because only it knows the
     * fleet.  It sets agent_id via the hello signal handler calling
     * clawt_link_send_welcome(), or closes the link.
     */
    g_signal_emit(self, signals[SIGNAL_HELLO], 0, agent_id, token);
}

static void
handle_chat_out(ClawtLink *self, JsonObject *payload)
{
    const gchar *room_id = NULL;
    const gchar *body = NULL;
    const gchar *thread_id = NULL;

    if (payload == NULL)
        return;

    /*
     * Anything before the handshake is dropped.  Without the agent id there
     * is nowhere to route it and no way to say who it came from, and
     * guessing from the connection would defeat the point of authenticating.
     */
    if (self->agent_id == NULL) {
        clawt_link_send_error(self, 401, "send a hello first");
        return;
    }

    if (json_object_has_member(payload, "room_id"))
        room_id = json_object_get_string_member(payload, "room_id");
    if (json_object_has_member(payload, "body"))
        body = json_object_get_string_member(payload, "body");
    if (json_object_has_member(payload, "thread_id"))
        thread_id = json_object_get_string_member(payload, "thread_id");

    if (body == NULL)
        return;

    g_signal_emit(self, signals[SIGNAL_MESSAGE], 0, room_id, body, thread_id);
}

static void
handle_typing(ClawtLink *self, JsonObject *payload)
{
    const gchar *room_id = NULL;
    gboolean typing = FALSE;

    if (payload == NULL || self->agent_id == NULL)
        return;

    if (json_object_has_member(payload, "room_id"))
        room_id = json_object_get_string_member(payload, "room_id");
    if (json_object_has_member(payload, "typing"))
        typing = json_object_get_boolean_member(payload, "typing");

    g_signal_emit(self, signals[SIGNAL_TYPING], 0, room_id, typing);
}

static void
handle_mcp_request(ClawtLink *self, const LcBridgeFrame *frame)
{
    g_autoptr(JsonNode) request = NULL;
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(GError) error = NULL;

    if (self->agent_id == NULL) {
        clawt_link_send_error(self, 401, "send a hello first");
        return;
    }

    if (self->mcp_handler == NULL || frame->payload == NULL)
        return;

    request = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(request, frame->payload);

    response = self->mcp_handler(self, request, self->mcp_data);
    if (response == NULL)
        return;

    {
        g_autoptr(JsonObject) payload = json_node_dup_object(response);

        if (!send_frame(self, LC_BRIDGE_FRAME_MCP_RESPONSE, frame->id,
                        payload, &error))
            g_warning("link %s: could not answer a tool call: %s",
                      self->agent_id, error->message);
    }
}

static void
handle_frame(ClawtLink *self, const LcBridgeFrame *frame)
{
    self->last_seen = g_get_monotonic_time();

    switch (frame->kind) {
    case LC_BRIDGE_FRAME_CONTROL_HELLO:
        handle_hello(self, frame->payload);
        break;

    case LC_BRIDGE_FRAME_CONTROL_PONG:
        /* The timestamp above is the whole point of a pong. */
        break;

    case LC_BRIDGE_FRAME_CONTROL_PING: {
        g_autoptr(GError) error = NULL;

        send_frame(self, LC_BRIDGE_FRAME_CONTROL_PONG, frame->id, NULL,
                   &error);
        break;
    }

    case LC_BRIDGE_FRAME_CONTROL_BYE:
        clawt_link_close(self, NULL);
        break;

    case LC_BRIDGE_FRAME_CHAT_MESSAGE_OUT:
        handle_chat_out(self, frame->payload);
        break;

    case LC_BRIDGE_FRAME_CHAT_TYPING:
        handle_typing(self, frame->payload);
        break;

    case LC_BRIDGE_FRAME_MCP_REQUEST:
        handle_mcp_request(self, frame);
        break;

    case LC_BRIDGE_FRAME_UNKNOWN:
    default:
        /*
         * Ignored rather than refused.  An agent built against a newer
         * clawtilla will send kinds this build does not know, and
         * disconnecting over one would break every mixed-version fleet.
         */
        break;
    }
}

static void
on_line_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(ClawtLink) self = user_data;   /* balances the ref in start_read */
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = NULL;
    gsize length = 0;

    self->reading = FALSE;

    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                                result, &length, &error);

    if (error != NULL) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_debug("link %s: read failed: %s",
                    self->agent_id != NULL ? self->agent_id : "(anonymous)",
                    error->message);
            clawt_link_close(self, NULL);
        }
        return;
    }

    if (line == NULL) {
        /* Clean end of stream: the agent went away. */
        clawt_link_close(self, NULL);
        return;
    }

    if (length > MAX_FRAME_BYTES) {
        g_warning("link %s: refusing a %" G_GSIZE_FORMAT " byte frame",
                  self->agent_id != NULL ? self->agent_id : "(anonymous)",
                  length);
        clawt_link_send_error(self, 413, "frame too large");
        start_read(self);
        return;
    }

    if (length > 0) {
        g_autoptr(JsonParser) parser = json_parser_new();
        g_autoptr(GError) parse_error = NULL;

        if (json_parser_load_from_data(parser, line, (gssize)length,
                                       &parse_error)) {
            LcBridgeFrame frame;

            if (lc_bridge_proto_decode(json_parser_get_root(parser), &frame,
                                       &parse_error))
                handle_frame(self, &frame);
            else
                clawt_link_send_error(self, 400, parse_error->message);
        } else {
            /*
             * One bad line does not end the connection.  The next is very
             * likely fine, and dropping an agent mid-conversation over a
             * truncated frame costs more than skipping it.
             */
            g_debug("link %s: unparseable frame ignored",
                    self->agent_id != NULL ? self->agent_id : "(anonymous)");
            clawt_link_send_error(self, 400, "unparseable frame");
        }
    }

    start_read(self);
}

static void
start_read(ClawtLink *self)
{
    if (!self->open || self->input == NULL || self->reading)
        return;

    self->reading = TRUE;

    /*
     * The read holds a reference.  Without it, a link dropped from the
     * server's table while a read is in flight is finalized under the
     * callback, and on_line_read touches freed memory.
     */
    g_data_input_stream_read_line_async(self->input, G_PRIORITY_DEFAULT,
                                        self->cancellable, on_line_read,
                                        g_object_ref(self));
}

/* ── Public API ──────────────────────────────────────────────────── */

ClawtLink *
clawt_link_new(GSocketConnection *connection)
{
    ClawtLink *self;

    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), NULL);

    self = g_object_new(CLAWT_TYPE_LINK, NULL);
    self->connection = g_object_ref(connection);

    self->input = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_data_input_stream_set_newline_type(self->input,
                                         G_DATA_STREAM_NEWLINE_TYPE_LF);

    self->output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    self->open = TRUE;
    self->last_seen = g_get_monotonic_time();

    return self;
}

void
clawt_link_start(ClawtLink *self)
{
    g_return_if_fail(CLAWT_IS_LINK(self));

    start_read(self);
}

void
clawt_link_close(ClawtLink *self, const gchar *reason)
{
    g_return_if_fail(CLAWT_IS_LINK(self));

    if (!self->open)
        return;

    if (reason != NULL) {
        g_autoptr(JsonObject) payload = json_object_new();
        g_autoptr(GError) error = NULL;

        json_object_set_string_member(payload, "reason", reason);
        send_frame(self, LC_BRIDGE_FRAME_CONTROL_BYE, NULL, payload, &error);
    }

    self->open = FALSE;
    g_cancellable_cancel(self->cancellable);

    if (self->connection != NULL)
        g_io_stream_close(G_IO_STREAM(self->connection), NULL, NULL);

    g_signal_emit(self, signals[SIGNAL_CLOSED], 0);
}

const gchar *
clawt_link_get_agent_id(ClawtLink *self)
{
    g_return_val_if_fail(CLAWT_IS_LINK(self), NULL);
    return self->agent_id;
}

const gchar *
clawt_link_get_agent_name(ClawtLink *self)
{
    g_return_val_if_fail(CLAWT_IS_LINK(self), NULL);
    return self->agent_name;
}

gboolean
clawt_link_is_open(ClawtLink *self)
{
    g_return_val_if_fail(CLAWT_IS_LINK(self), FALSE);
    return self->open;
}

gboolean
clawt_link_deliver(ClawtLink    *self,
                   const gchar  *room_id,
                   const gchar  *sender_id,
                   const gchar  *sender_name,
                   const gchar  *body,
                   const gchar  *thread_id,
                   const gchar  *session_peer,
                   GError      **error)
{
    g_autoptr(JsonObject) payload = json_object_new();

    g_return_val_if_fail(CLAWT_IS_LINK(self), FALSE);
    g_return_val_if_fail(body != NULL, FALSE);

    json_object_set_string_member(payload, "body", body);

    if (room_id != NULL)
        json_object_set_string_member(payload, "room_id", room_id);
    if (sender_id != NULL)
        json_object_set_string_member(payload, "sender", sender_id);
    if (sender_name != NULL)
        json_object_set_string_member(payload, "sender_name", sender_name);
    if (thread_id != NULL)
        json_object_set_string_member(payload, "thread_id", thread_id);

    /*
     * Omitted when it matches the sender: the common case needs no
     * hint, and a frame member that is always present stops reading as
     * the exception it marks.
     */
    if (session_peer != NULL && g_strcmp0(session_peer, sender_id) != 0)
        json_object_set_string_member(payload, "session_peer", session_peer);

    return send_frame(self, LC_BRIDGE_FRAME_CHAT_MESSAGE_IN, NULL, payload,
                      error);
}

gboolean
clawt_link_send_welcome(ClawtLink *self, GError **error)
{
    g_autoptr(JsonObject) payload = json_object_new();

    g_return_val_if_fail(CLAWT_IS_LINK(self), FALSE);

    json_object_set_string_member(payload, "server", "clawtilla");
    json_object_set_int_member(payload, "protocol",
                               LC_BRIDGE_PROTOCOL_VERSION);

    if (self->agent_id != NULL)
        json_object_set_string_member(payload, "agent_id", self->agent_id);

    return send_frame(self, LC_BRIDGE_FRAME_CONTROL_WELCOME, NULL, payload,
                      error);
}

void
clawt_link_send_error(ClawtLink *self, gint code, const gchar *message)
{
    g_autoptr(JsonNode) envelope = NULL;
    g_autoptr(GError) error = NULL;

    g_return_if_fail(CLAWT_IS_LINK(self));

    if (!self->open)
        return;

    envelope = lc_bridge_proto_encode_error(NULL, code, message);
    send_raw_node(self, envelope, &error);
}

void
clawt_link_ping(ClawtLink *self)
{
    g_autoptr(GError) error = NULL;

    g_return_if_fail(CLAWT_IS_LINK(self));

    send_frame(self, LC_BRIDGE_FRAME_CONTROL_PING, NULL, NULL, &error);
}

gint64
clawt_link_seconds_since_seen(ClawtLink *self)
{
    g_return_val_if_fail(CLAWT_IS_LINK(self), 0);

    return (g_get_monotonic_time() - self->last_seen) / G_USEC_PER_SEC;
}

void
clawt_link_set_mcp_handler(ClawtLink           *self,
                           ClawtLinkMcpHandler  handler,
                           gpointer             user_data,
                           GDestroyNotify       destroy)
{
    g_return_if_fail(CLAWT_IS_LINK(self));

    if (self->mcp_destroy != NULL && self->mcp_data != NULL)
        self->mcp_destroy(self->mcp_data);

    self->mcp_handler = handler;
    self->mcp_data = user_data;
    self->mcp_destroy = destroy;
}

/*
 * Internal: the server calls this from its hello handler once it has
 * decided the connection may have this identity.
 */
void
clawt_link_accept_identity(ClawtLink *self, const gchar *agent_id);

void
clawt_link_accept_identity(ClawtLink *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_LINK(self));

    g_free(self->agent_id);
    self->agent_id = g_strdup(agent_id);
}

/* ── Object lifecycle ────────────────────────────────────────────── */

static void
clawt_link_dispose(GObject *object)
{
    ClawtLink *self = CLAWT_LINK(object);

    self->open = FALSE;

    if (self->cancellable != NULL)
        g_cancellable_cancel(self->cancellable);

    if (self->mcp_destroy != NULL && self->mcp_data != NULL) {
        self->mcp_destroy(self->mcp_data);
        self->mcp_destroy = NULL;
        self->mcp_data = NULL;
    }

    g_clear_object(&self->input);
    self->output = NULL;

    if (self->connection != NULL) {
        g_io_stream_close(G_IO_STREAM(self->connection), NULL, NULL);
        g_clear_object(&self->connection);
    }

    g_clear_object(&self->cancellable);

    G_OBJECT_CLASS(clawt_link_parent_class)->dispose(object);
}

static void
clawt_link_finalize(GObject *object)
{
    ClawtLink *self = CLAWT_LINK(object);

    g_clear_pointer(&self->agent_id, g_free);
    g_clear_pointer(&self->agent_name, g_free);

    G_OBJECT_CLASS(clawt_link_parent_class)->finalize(object);
}

static void
clawt_link_class_init(ClawtLinkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_link_dispose;
    object_class->finalize = clawt_link_finalize;

    /**
     * ClawtLink::hello:
     * @self: the link
     * @agent_id: the agent it claims to be
     * @token: (nullable): the token it presented
     *
     * An agent has identified itself and is waiting to be accepted.
     */
    signals[SIGNAL_HELLO] =
        g_signal_new("hello", CLAWT_TYPE_LINK, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_STRING, G_TYPE_STRING);

    /**
     * ClawtLink::message:
     * @self: the link
     * @room_id: (nullable): the room it was sent to
     * @body: the message
     * @thread_id: (nullable): the thread it replies into
     *
     * The agent sent a message.
     */
    signals[SIGNAL_MESSAGE] =
        g_signal_new("message", CLAWT_TYPE_LINK, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 3,
                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    /**
     * ClawtLink::typing:
     * @self: the link
     * @room_id: (nullable): the room
     * @typing: whether the agent is composing
     */
    signals[SIGNAL_TYPING] =
        g_signal_new("typing", CLAWT_TYPE_LINK, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_STRING, G_TYPE_BOOLEAN);

    /**
     * ClawtLink::closed:
     * @self: the link
     *
     * The connection has gone.  Emitted exactly once.
     */
    signals[SIGNAL_CLOSED] =
        g_signal_new("closed", CLAWT_TYPE_LINK, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
clawt_link_init(ClawtLink *self)
{
    self->cancellable = g_cancellable_new();
    self->open = FALSE;
}
