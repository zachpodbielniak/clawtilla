/*
 * clawt-mcp-http-client.h - A daemon-side client for a streamable-HTTP MCP server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/**
 * CLAWT_MCP_HTTP_MAX_REPLY_BYTES:
 *
 * The most of one reply the client will hold.  A tool result is text; a
 * server that sends more is refused while it is read, not after.
 */
#define CLAWT_MCP_HTTP_MAX_REPLY_BYTES (4 * 1024 * 1024)

#define CLAWT_TYPE_MCP_HTTP_CLIENT (clawt_mcp_http_client_get_type())

G_DECLARE_FINAL_TYPE(ClawtMcpHttpClient, clawt_mcp_http_client, CLAWT, MCP_HTTP_CLIENT,
                     GObject)

/**
 * clawt_mcp_http_client_new:
 * @name: the connector's name, for messages ("hf")
 * @url: the server's MCP endpoint
 * @header_name: (nullable): the credential's header, e.g. "Authorization"
 * @header_value: (nullable): its value; %NULL for a server not signed in to
 *
 * A client for one remote MCP server, for the daemon's own use -- an
 * agent reaches the same server through `clawtilla connector relay`.
 * Everything is asynchronous: the daemon never waits on the network.
 * The first call initializes the session and keeps the server's
 * `Mcp-Session-Id`; replies are read as JSON or as an event stream,
 * whichever the server chose, and bounded while they are read
 * (%CLAWT_MCP_HTTP_MAX_REPLY_BYTES).
 *
 * Returns: (transfer full): a new client
 */
ClawtMcpHttpClient *clawt_mcp_http_client_new(const gchar *name,
                                              const gchar *url,
                                              const gchar *header_name,
                                              const gchar *header_value);

/**
 * clawt_mcp_http_client_set_session:
 * @self: a #ClawtMcpHttpClient
 * @session: (transfer none): the session to send on
 *
 * For tests, which point a session's `remote-connectable` at a server on
 * a unix socket.  Without it the client makes its own session, on the
 * thread-default context of its first call, because libsoup binds a
 * session to the context it was made under.
 */
void clawt_mcp_http_client_set_session(ClawtMcpHttpClient *self, SoupSession *session);

/**
 * clawt_mcp_http_client_get_session:
 * @self: a #ClawtMcpHttpClient
 *
 * The session requests go out on, made now if the client has none --
 * so call it on the context the requests will run on.  A generator uses
 * it for the plain transfers around a tool call (an upload to a
 * presigned URL, a result's download), which carry no credential.
 *
 * Returns: (transfer none): the session
 */
SoupSession *clawt_mcp_http_client_get_session(ClawtMcpHttpClient *self);

/**
 * clawt_mcp_http_client_call_tool_async:
 * @self: a #ClawtMcpHttpClient
 * @tool: the tool's name
 * @arguments: (nullable) (transfer none): its arguments
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the result
 * @user_data: for @callback
 *
 * Calls one tool, initializing the session first if this is the first
 * call.  A result the server marked `isError` is an error, carrying the
 * result's text; a 401 is %CLAWT_ERROR_AUTH naming the sign-in.
 */
void clawt_mcp_http_client_call_tool_async(ClawtMcpHttpClient  *self,
                                           const gchar         *tool,
                                           JsonObject          *arguments,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);

/**
 * clawt_mcp_http_client_call_tool_finish:
 * @self: a #ClawtMcpHttpClient
 * @result: the result passed to the callback
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the call's `result` object --
 *   `content`, and `structuredContent` when the server sent one
 */
JsonObject *clawt_mcp_http_client_call_tool_finish(ClawtMcpHttpClient  *self,
                                                   GAsyncResult        *result,
                                                   GError             **error);

/**
 * clawt_mcp_http_send_bounded_async:
 * @session: the session to send on
 * @message: (transfer none): the request
 * @max_bytes: the most of the body to hold
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the body
 * @user_data: for @callback
 *
 * Sends @message and reads its body, refusing a body longer than
 * @max_bytes *while* it is read -- a limit applied after the read is a
 * check on memory already spent.  A redirect @message refused (see
 * clawt_http_message_new()) is an error naming where it pointed.
 */
void clawt_mcp_http_send_bounded_async(SoupSession         *session,
                                       SoupMessage         *message,
                                       gsize                max_bytes,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);

/**
 * clawt_mcp_http_send_bounded_finish:
 * @result: the result passed to the callback
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the body, whatever the status --
 *   read the status from the message
 */
GBytes *clawt_mcp_http_send_bounded_finish(GAsyncResult  *result,
                                           GError       **error);

/**
 * clawt_mcp_http_result_text:
 * @result: a tool call's result object
 *
 * Returns: (transfer full): its text content, joined; empty for none
 */
gchar *clawt_mcp_http_result_text(JsonObject *result);

/**
 * clawt_mcp_http_parse_reply:
 * @body: (array length=length): a reply body
 * @length: its length
 * @content_type: (nullable): the reply's Content-Type
 * @id: the request id the reply must answer
 * @error: (out) (optional): return location for a #GError
 *
 * Finds the JSON-RPC response to @id in a reply that is either one JSON
 * object or an event stream (`data:` lines, possibly carrying
 * notifications before the response).  Pure, so it is tested as data.
 *
 * Returns: (transfer full) (nullable): the response object
 */
JsonObject *clawt_mcp_http_parse_reply(const gchar  *body,
                                       gsize         length,
                                       const gchar  *content_type,
                                       gint64        id,
                                       GError      **error);

G_END_DECLS
