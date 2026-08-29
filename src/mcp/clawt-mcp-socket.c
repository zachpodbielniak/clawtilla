/*
 * clawt-mcp-socket.c - Calling one MCP tool over a unix socket
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mcp/clawt-mcp-socket.h"

#include <string.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

/*
 * A screenshot arrives as base64 in one JSON-RPC line, so a line here is
 * routinely megabytes -- the same reason the stdio relay raises its
 * buffer.  The default 4 KB would read one of these a fragment at a time.
 */
#define SOCKET_BUFFER_BYTES (64 * 1024)

/*
 * The protocol version clawtilla claims.  gowl accepts anything it
 * recognises and answers with its own; nothing here depends on which.
 */
#define MCP_PROTOCOL_VERSION "2024-11-05"

static gboolean
write_line(GOutputStream *stream, const gchar *line, GError **error)
{
    g_autofree gchar *framed = g_strconcat(line, "\n", NULL);

    return g_output_stream_write_all(stream, framed, strlen(framed), NULL,
                                     NULL, error);
}

static gchar *
render(JsonObject *object)
{
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

    json_node_set_object(node, object);
    json_generator_set_root(generator, node);
    json_generator_set_pretty(generator, FALSE);

    return json_generator_to_data(generator, NULL);
}

/*
 * Reads lines until one carries @id, or until the stream ends.
 *
 * A server is free to send notifications and log messages between the
 * request and its answer, and gowl does.  Taking the first line back as
 * the reply would work in testing and fail the first time anything else
 * was happening on that compositor.
 */
static JsonObject *
read_reply(GDataInputStream *in, gint64 id, GError **error)
{
    while (TRUE) {
        g_autofree gchar *line = NULL;
        g_autoptr(JsonParser) parser = NULL;
        JsonNode *root;
        JsonObject *object;

        line = g_data_input_stream_read_line(in, NULL, NULL, error);

        if (line == NULL) {
            if (error != NULL && *error == NULL)
                g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                    "the tool server closed without "
                                    "answering");
            return NULL;
        }

        parser = json_parser_new();

        if (!json_parser_load_from_data(parser, line, -1, NULL))
            continue;

        root = json_parser_get_root(parser);

        if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
            continue;

        object = json_node_get_object(root);

        if (object == NULL || !json_object_has_member(object, "id"))
            continue;

        if (json_object_get_int_member_with_default(object, "id", -1) != id)
            continue;

        if (json_object_has_member(object, "error")) {
            JsonObject *failure =
                JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "error"))
                ? json_object_get_object_member(object, "error") : NULL;

            g_set_error_literal(
                error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                (failure != NULL)
                ? json_object_get_string_member_with_default(
                      failure, "message", "the tool server refused")
                : "the tool server refused");
            return NULL;
        }

        if (!json_object_has_member(object, "result") ||
            !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object,
                                                           "result"))) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "the tool server answered with no result");
            return NULL;
        }

        return json_object_ref(json_object_get_object_member(object,
                                                             "result"));
    }
}

static JsonObject *
request_object(gint64 id, const gchar *method)
{
    JsonObject *object = json_object_new();

    json_object_set_string_member(object, "jsonrpc", "2.0");
    json_object_set_int_member(object, "id", id);
    json_object_set_string_member(object, "method", method);

    return object;
}

static gboolean
handshake(GOutputStream *out, GDataInputStream *in, GError **error)
{
    g_autoptr(JsonObject) initialize = request_object(1, "initialize");
    JsonObject *params = json_object_new();
    JsonObject *capabilities = json_object_new();
    JsonObject *info = json_object_new();
    g_autoptr(JsonObject) result = NULL;
    g_autofree gchar *line = NULL;
    g_autoptr(JsonObject) done = json_object_new();
    g_autofree gchar *done_line = NULL;

    json_object_set_string_member(params, "protocolVersion",
                                  MCP_PROTOCOL_VERSION);
    json_object_set_object_member(params, "capabilities", capabilities);
    json_object_set_string_member(info, "name", "clawtilla");
    json_object_set_string_member(info, "version", CLAWT_VERSION_STRING);
    json_object_set_object_member(params, "clientInfo", info);
    json_object_set_object_member(initialize, "params", params);

    line = render(initialize);

    if (!write_line(out, line, error))
        return FALSE;

    result = read_reply(in, 1, error);

    if (result == NULL)
        return FALSE;

    /*
     * The notification the specification requires before any call.  A
     * server is entitled to refuse everything until it arrives, and one
     * that does not is a server this would have worked against by luck.
     */
    json_object_set_string_member(done, "jsonrpc", "2.0");
    json_object_set_string_member(done, "method", "notifications/initialized");
    done_line = render(done);

    return write_line(out, done_line, error);
}

JsonNode *
clawt_mcp_socket_call(const gchar  *socket_path,
                      const gchar  *tool,
                      JsonNode     *arguments,
                      guint         timeout_seconds,
                      GError      **error)
{
    g_autoptr(GSocketClient) client = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GDataInputStream) in = NULL;
    g_autoptr(JsonObject) call = NULL;
    g_autoptr(JsonObject) result = NULL;
    JsonObject *params;
    GOutputStream *out;
    g_autofree gchar *line = NULL;

    g_return_val_if_fail(socket_path != NULL, NULL);
    g_return_val_if_fail(tool != NULL, NULL);

    address = g_unix_socket_address_new(socket_path);
    client = g_socket_client_new();

    if (timeout_seconds > 0)
        g_socket_client_set_timeout(client, timeout_seconds);

    connection = g_socket_client_connect(client,
                                         G_SOCKET_CONNECTABLE(address),
                                         NULL, error);

    if (connection == NULL) {
        /*
         * Prefixed with the path, because the message GIO writes says
         * only "Connection refused" -- and somebody reading that in a
         * log has no way to know which socket, or that a socket was
         * involved at all.
         */
        if (error != NULL && *error != NULL)
            g_prefix_error(error, "cannot reach the tool server at %s: ",
                           socket_path);
        return NULL;
    }

    out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    in = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_buffered_input_stream_set_buffer_size(G_BUFFERED_INPUT_STREAM(in),
                                            SOCKET_BUFFER_BYTES);

    if (!handshake(out, in, error))
        return NULL;

    call = request_object(2, "tools/call");
    params = json_object_new();
    json_object_set_string_member(params, "name", tool);

    if (arguments != NULL)
        json_object_set_member(params, "arguments", json_node_copy(arguments));

    json_object_set_object_member(call, "params", params);
    line = render(call);

    if (!write_line(out, line, error))
        return NULL;

    result = read_reply(in, 2, error);

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    if (result == NULL)
        return NULL;

    /*
     * `isError` is a failure, not a result with a flag on it.
     *
     * MCP reports a tool's own refusal inside a successful reply, so a
     * caller that only checked the return value would take "automation
     * is switched off" as a picture and hand an empty frame on. The
     * server's own words come back in the error.
     */
    if (json_object_get_boolean_member_with_default(result, "isError",
                                                    FALSE)) {
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        g_autofree gchar *text = NULL;

        json_node_set_object(node, result);
        text = clawt_mcp_socket_result_text(node);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "%s refused: %s", tool,
                    (text != NULL && *text != '\0') ? text
                                                    : "no reason given");
        return NULL;
    }

    {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);

        json_node_set_object(node, result);

        return node;
    }
}

/*
 * The `content` array of a tool result, or %NULL.
 */
static JsonArray *
result_content(JsonNode *result)
{
    JsonObject *object;

    if (result == NULL || !JSON_NODE_HOLDS_OBJECT(result))
        return NULL;

    object = json_node_get_object(result);

    if (object == NULL || !json_object_has_member(object, "content") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "content")))
        return NULL;

    return json_object_get_array_member(object, "content");
}

GBytes *
clawt_mcp_socket_result_image(JsonNode *result)
{
    JsonArray *content = result_content(result);
    guint i;

    if (content == NULL)
        return NULL;

    for (i = 0; i < json_array_get_length(content); i++) {
        JsonNode *element = json_array_get_element(content, i);
        JsonObject *item;
        const gchar *data;
        guchar *raw;
        gsize length = 0;

        if (!JSON_NODE_HOLDS_OBJECT(element))
            continue;

        item = json_node_get_object(element);
        data = json_object_get_string_member_with_default(item, "data", NULL);

        if (data == NULL || *data == '\0')
            continue;

        raw = g_base64_decode(data, &length);

        if (raw == NULL || length == 0) {
            g_free(raw);
            continue;
        }

        return g_bytes_new_take(raw, length);
    }

    return NULL;
}

gchar *
clawt_mcp_socket_result_text(JsonNode *result)
{
    JsonArray *content = result_content(result);
    g_autoptr(GString) out = NULL;
    guint i;

    if (content == NULL)
        return NULL;

    out = g_string_new(NULL);

    for (i = 0; i < json_array_get_length(content); i++) {
        JsonNode *element = json_array_get_element(content, i);
        JsonObject *item;
        const gchar *text;

        if (!JSON_NODE_HOLDS_OBJECT(element))
            continue;

        item = json_node_get_object(element);
        text = json_object_get_string_member_with_default(item, "text", NULL);

        if (text == NULL)
            continue;

        if (out->len > 0)
            g_string_append_c(out, '\n');

        g_string_append(out, text);
    }

    if (out->len == 0)
        return NULL;

    return g_strdup(out->str);
}
