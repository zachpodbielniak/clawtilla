/*
 * clawt-mcp-http-client.c - A daemon-side client for a streamable-HTTP MCP server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Agents reach a remote MCP server through `clawtilla connector relay`,
 * a process of its own that may block.  The daemon may not: anything it
 * asks of a server goes through here, asynchronously, on whatever
 * context the first call was made from.  The office's character
 * generator is the first user -- generating through a connector's
 * sign-in rather than a separate API key, because the two are billed
 * differently (docs/office-custodial-team.org).
 *
 * The protocol is the small part of streamable HTTP a client needs:
 * POST each JSON-RPC message, keep the `Mcp-Session-Id` the initialize
 * reply carries, and read the answer as JSON or as an event stream,
 * whichever the server sent.
 */

#include "clawtilla.h"
#include "clawt-http.h"

#include <string.h>

#define PROTOCOL_VERSION "2025-06-18"
#define READ_CHUNK (64 * 1024)

struct _ClawtMcpHttpClient {
    GObject      parent_instance;
    gchar       *name;
    gchar       *url;
    gchar       *header_name;
    gchar       *header_value;
    SoupSession *session;
    gchar       *session_id;
    gboolean     initialized;
    gint64       next_id;
};

G_DEFINE_FINAL_TYPE(ClawtMcpHttpClient, clawt_mcp_http_client, G_TYPE_OBJECT)

static void
clawt_mcp_http_client_finalize(GObject *object)
{
    ClawtMcpHttpClient *self = CLAWT_MCP_HTTP_CLIENT(object);

    g_free(self->name);
    g_free(self->url);
    g_free(self->header_name);
    /* a live credential: not left in freed memory */
    if (self->header_value != NULL)
        memset(self->header_value, 0, strlen(self->header_value));
    g_free(self->header_value);
    g_clear_object(&self->session);
    g_free(self->session_id);

    G_OBJECT_CLASS(clawt_mcp_http_client_parent_class)->finalize(object);
}

static void
clawt_mcp_http_client_class_init(ClawtMcpHttpClientClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_mcp_http_client_finalize;
}

static void
clawt_mcp_http_client_init(ClawtMcpHttpClient *self)
{
    self->next_id = 1;
}

ClawtMcpHttpClient *
clawt_mcp_http_client_new(const gchar *name,
                          const gchar *url,
                          const gchar *header_name,
                          const gchar *header_value)
{
    ClawtMcpHttpClient *self;

    g_return_val_if_fail(url != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_MCP_HTTP_CLIENT, NULL);
    self->name = g_strdup(name != NULL ? name : "?");
    self->url = g_strdup(url);
    self->header_name = g_strdup(header_name);
    self->header_value = g_strdup(header_value);
    return self;
}

void
clawt_mcp_http_client_set_session(ClawtMcpHttpClient *self, SoupSession *session)
{
    g_return_if_fail(CLAWT_IS_MCP_HTTP_CLIENT(self));

    g_set_object(&self->session, session);
}

SoupSession *
clawt_mcp_http_client_get_session(ClawtMcpHttpClient *self)
{
    g_return_val_if_fail(CLAWT_IS_MCP_HTTP_CLIENT(self), NULL);

    /*
     * Made here, on the caller's thread-default context, never in the
     * constructor: a session made under one context and used from
     * another sends its request and never reads the answer.
     */
    if (self->session == NULL)
        self->session = soup_session_new();
    return self->session;
}

/* ── A bounded send ─────────────────────────────────────────────── */

typedef struct {
    SoupSession  *session;
    SoupMessage  *message;
    GInputStream *stream;
    GByteArray   *body;
    gsize         max_bytes;
} Bounded;

static void
bounded_free(Bounded *b)
{
    g_clear_object(&b->session);
    g_clear_object(&b->message);
    g_clear_object(&b->stream);
    if (b->body != NULL)
        g_byte_array_unref(b->body);
    g_free(b);
}

static void bounded_read_next(GTask *task);

static void
on_bounded_chunk(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    Bounded *b = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
                                                               &error);
    gsize length;

    if (chunk == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    length = g_bytes_get_size(chunk);
    if (length == 0) {
        g_task_return_pointer(task, g_byte_array_free_to_bytes(g_steal_pointer(&b->body)),
                              (GDestroyNotify)g_bytes_unref);
        return;
    }
    if (b->body->len + length > b->max_bytes) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "the reply is longer than %" G_GSIZE_FORMAT " bytes",
                                b->max_bytes);
        return;
    }
    g_byte_array_append(b->body, g_bytes_get_data(chunk, NULL), (guint)length);
    bounded_read_next(g_steal_pointer(&task));
}

static void
bounded_read_next(GTask *task)
{
    Bounded *b = g_task_get_task_data(task);

    g_input_stream_read_bytes_async(b->stream, READ_CHUNK, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable(task), on_bounded_chunk, task);
}

static void
on_bounded_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GTask *task = user_data;
    Bounded *b = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;

    b->stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
    if (b->stream == NULL || !clawt_http_check_redirect(b->message, &error)) {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }
    b->body = g_byte_array_new();
    bounded_read_next(task);
}

void
clawt_mcp_http_send_bounded_async(SoupSession         *session,
                                  SoupMessage         *message,
                                  gsize                max_bytes,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    Bounded *b = g_new0(Bounded, 1);

    b->session = g_object_ref(session);
    b->message = g_object_ref(message);
    b->max_bytes = max_bytes;
    g_task_set_task_data(task, b, (GDestroyNotify)bounded_free);
    soup_session_send_async(session, message, G_PRIORITY_DEFAULT, cancellable,
                            on_bounded_sent, task);
}

GBytes *
clawt_mcp_http_send_bounded_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Replies ────────────────────────────────────────────────────── */

/* The response to @id in one JSON text, or NULL when it is something else. */
static JsonObject *
response_in(const gchar *text, gsize length, gint64 id)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;
    JsonObject *object;

    if (!json_parser_load_from_data(parser, text, (gssize)length, NULL))
        return NULL;
    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;
    object = json_node_get_object(root);
    if (!json_object_has_member(object, "id") ||
        json_object_get_int_member_with_default(object, "id", -1) != id)
        return NULL;
    return json_object_ref(object);
}

JsonObject *
clawt_mcp_http_parse_reply(const gchar  *body,
                           gsize         length,
                           const gchar  *content_type,
                           gint64        id,
                           GError      **error)
{
    JsonObject *found = NULL;

    if (body == NULL || length == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "the server sent an empty reply");
        return NULL;
    }

    /*
     * An event stream: each event's data lines joined by newlines, and
     * notifications may come before the answer -- so look at every event
     * for the one answering this request.
     */
    if (content_type != NULL && g_str_has_prefix(content_type, "text/event-stream")) {
        g_autofree gchar *copy = g_strndup(body, length);
        g_auto(GStrv) lines = g_strsplit(copy, "\n", -1);
        g_autoptr(GString) data = g_string_new(NULL);
        guint i;

        for (i = 0; lines[i] != NULL && found == NULL; i++) {
            gchar *line = g_strchomp(lines[i]);

            if (g_str_has_prefix(line, "data:")) {
                const gchar *value = line + 5;

                if (*value == ' ')
                    value++;
                if (data->len > 0)
                    g_string_append_c(data, '\n');
                g_string_append(data, value);
                continue;
            }
            /* a blank line ends the event */
            if (*line == '\0' && data->len > 0) {
                found = response_in(data->str, data->len, id);
                g_string_truncate(data, 0);
            }
        }
        if (found == NULL && data->len > 0)
            found = response_in(data->str, data->len, id);
    } else {
        found = response_in(body, length, id);
    }

    if (found == NULL)
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the server's reply did not answer request %" G_GINT64_FORMAT, id);
    return found;
}

gchar *
clawt_mcp_http_result_text(JsonObject *result)
{
    GString *text = g_string_new(NULL);
    JsonArray *content;
    guint i;

    content = result != NULL && json_object_has_member(result, "content") &&
              JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "content"))
        ? json_object_get_array_member(result, "content") : NULL;
    for (i = 0; content != NULL && i < json_array_get_length(content); i++) {
        JsonNode *node = json_array_get_element(content, i);
        JsonObject *part;

        if (!JSON_NODE_HOLDS_OBJECT(node))
            continue;
        part = json_node_get_object(node);
        if (g_strcmp0(json_object_get_string_member_with_default(part, "type", ""), "text") != 0)
            continue;
        if (text->len > 0)
            g_string_append_c(text, ' ');
        g_string_append(text, json_object_get_string_member_with_default(part, "text", ""));
    }
    return g_string_free(text, FALSE);
}

/* ── Calling a tool ─────────────────────────────────────────────── */

typedef enum {
    STAGE_INITIALIZE,
    STAGE_INITIALIZED,
    STAGE_CALL
} Stage;

typedef struct {
    gchar       *tool;
    JsonObject  *arguments;
    Stage        stage;
    gint64       id;
    SoupMessage *message;
} Call;

static void
call_free(Call *call)
{
    g_free(call->tool);
    if (call->arguments != NULL)
        json_object_unref(call->arguments);
    g_clear_object(&call->message);
    g_free(call);
}

static void send_stage(GTask *task);

static gchar *
rpc_text(const gchar *method, gint64 id, JsonObject *params)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc");
    json_builder_add_string_value(b, "2.0");
    if (id >= 0) {
        json_builder_set_member_name(b, "id");
        json_builder_add_int_value(b, id);
    }
    json_builder_set_member_name(b, "method");
    json_builder_add_string_value(b, method);
    if (params != NULL) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);

        json_node_set_object(node, params);
        json_builder_set_member_name(b, "params");
        json_builder_add_value(b, node);
    }
    json_builder_end_object(b);
    root = json_builder_get_root(b);
    json_generator_set_root(generator, root);
    return json_generator_to_data(generator, NULL);
}

static void
on_stage_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GTask *task = user_data;
    ClawtMcpHttpClient *self = g_task_get_source_object(task);
    Call *call = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) body = clawt_mcp_http_send_bounded_finish(result, &error);
    g_autoptr(JsonObject) response = NULL;
    guint status;
    const gchar *data;
    gsize length = 0;

    (void)source;
    if (body == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "the connector '%s' could not reach %s: %s", self->name,
                                self->url, error->message);
        g_object_unref(task);
        return;
    }
    status = soup_message_get_status(call->message);

    /* signed out, or the sign-in expired: say which command fixes it */
    if (status == SOUP_STATUS_UNAUTHORIZED) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                "the connector '%s' needs a sign-in: "
                                "clawtilla connector connect %s", self->name, self->name);
        g_object_unref(task);
        return;
    }

    if (call->stage == STAGE_INITIALIZE) {
        const gchar *fresh = soup_message_headers_get_one(
            soup_message_get_response_headers(call->message), "Mcp-Session-Id");

        if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                    "the connector '%s' refused to start a session (HTTP %u)",
                                    self->name, status);
            g_object_unref(task);
            return;
        }
        if (fresh != NULL) {
            g_free(self->session_id);
            self->session_id = g_strdup(fresh);
        }
        call->stage = STAGE_INITIALIZED;
        send_stage(task);
        return;
    }
    if (call->stage == STAGE_INITIALIZED) {
        /* a notification: 202 and no body, and nothing to read from it */
        self->initialized = TRUE;
        call->stage = STAGE_CALL;
        send_stage(task);
        return;
    }

    data = g_bytes_get_data(body, &length);
    response = clawt_mcp_http_parse_reply(data, length,
        soup_message_headers_get_content_type(soup_message_get_response_headers(call->message),
                                              NULL),
        call->id, &error);
    if (response == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "the connector '%s' answered %s badly (HTTP %u): %s",
                                self->name, call->tool, status, error->message);
        g_object_unref(task);
        return;
    }
    if (json_object_has_member(response, "error")) {
        JsonObject *rpc = json_object_get_object_member(response, "error");

        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s", call->tool,
                                rpc != NULL
                                ? json_object_get_string_member_with_default(rpc, "message", "?")
                                : "?");
        g_object_unref(task);
        return;
    }
    {
        JsonObject *outcome = json_object_has_member(response, "result")
            ? json_object_get_object_member(response, "result") : NULL;

        if (outcome == NULL) {
            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                    "%s answered with no result", call->tool);
        } else if (json_object_get_boolean_member_with_default(outcome, "isError", FALSE)) {
            g_autofree gchar *text = clawt_mcp_http_result_text(outcome);

            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s",
                                    call->tool, *text != '\0' ? text : "refused");
        } else {
            g_task_return_pointer(task, json_object_ref(outcome),
                                  (GDestroyNotify)json_object_unref);
        }
    }
    g_object_unref(task);
}

static void
send_stage(GTask *task)
{
    ClawtMcpHttpClient *self = g_task_get_source_object(task);
    Call *call = g_task_get_task_data(task);
    g_autofree gchar *text = NULL;
    g_autoptr(GBytes) bytes = NULL;
    SoupMessageHeaders *headers;

    if (call->stage == STAGE_INITIALIZE) {
        g_autoptr(JsonObject) params = json_object_new();
        JsonObject *info = json_object_new();

        json_object_set_string_member(params, "protocolVersion", PROTOCOL_VERSION);
        json_object_set_object_member(params, "capabilities", json_object_new());
        json_object_set_string_member(info, "name", "clawtilla");
        json_object_set_string_member(info, "version", CLAWT_VERSION_STRING);
        json_object_set_object_member(params, "clientInfo", info);
        call->id = self->next_id++;
        text = rpc_text("initialize", call->id, params);
    } else if (call->stage == STAGE_INITIALIZED) {
        text = rpc_text("notifications/initialized", -1, NULL);
    } else {
        g_autoptr(JsonObject) params = json_object_new();

        json_object_set_string_member(params, "name", call->tool);
        json_object_set_object_member(params, "arguments",
                                      call->arguments != NULL
                                      ? json_object_ref(call->arguments) : json_object_new());
        call->id = self->next_id++;
        text = rpc_text("tools/call", call->id, params);
    }

    g_clear_object(&call->message);
    call->message = clawt_http_message_new("POST", self->url, CLAWT_HTTP_CARRIES_CREDENTIALS);
    if (call->message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "the connector '%s' names '%s', which is not a URL", self->name,
                                self->url);
        g_object_unref(task);
        return;
    }
    {
        gsize length = strlen(text);

        bytes = g_bytes_new_take(g_steal_pointer(&text), length);
    }
    soup_message_set_request_body_from_bytes(call->message, "application/json", bytes);
    headers = soup_message_get_request_headers(call->message);
    soup_message_headers_replace(headers, "Accept", "application/json, text/event-stream");
    if (self->header_name != NULL && self->header_value != NULL)
        soup_message_headers_replace(headers, self->header_name, self->header_value);
    /* a streamable-HTTP server refuses every request after initialize that lacks it */
    if (self->session_id != NULL)
        soup_message_headers_replace(headers, "Mcp-Session-Id", self->session_id);
    if (call->stage != STAGE_INITIALIZE)
        soup_message_headers_replace(headers, "MCP-Protocol-Version", PROTOCOL_VERSION);

    clawt_mcp_http_send_bounded_async(clawt_mcp_http_client_get_session(self), call->message,
                                      CLAWT_MCP_HTTP_MAX_REPLY_BYTES,
                                      g_task_get_cancellable(task), on_stage_reply, task);
}

void
clawt_mcp_http_client_call_tool_async(ClawtMcpHttpClient  *self,
                                      const gchar         *tool,
                                      JsonObject          *arguments,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    GTask *task;
    Call *call;

    g_return_if_fail(CLAWT_IS_MCP_HTTP_CLIENT(self));
    g_return_if_fail(tool != NULL);

    task = g_task_new(self, cancellable, callback, user_data);
    call = g_new0(Call, 1);
    call->tool = g_strdup(tool);
    call->arguments = arguments != NULL ? json_object_ref(arguments) : NULL;
    call->stage = self->initialized ? STAGE_CALL : STAGE_INITIALIZE;
    g_task_set_task_data(task, call, (GDestroyNotify)call_free);
    send_stage(task);
}

JsonObject *
clawt_mcp_http_client_call_tool_finish(ClawtMcpHttpClient  *self,
                                       GAsyncResult        *result,
                                       GError             **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}
