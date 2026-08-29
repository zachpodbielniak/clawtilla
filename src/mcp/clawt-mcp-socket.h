/*
 * clawt-mcp-socket.h - Calling one MCP tool over a unix socket
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla has spoken MCP in one direction until now: it *serves* tools
 * to agents, and relays somebody else's server to them.  Watching a host
 * desktop needs the other direction -- gowl exposes its compositor as an
 * MCP server on a unix socket, and a screenshot is a tools/call.
 *
 * Deliberately the smallest thing that does that.  It connects, does the
 * handshake, makes one call and closes; there is no session to keep
 * alive, no reconnect logic and no state to go stale.  A connection held
 * open across a compositor restart is a connection that answers nothing
 * and cannot say why, and this is called a few times a second at most
 * against a socket on the same machine.
 *
 * It blocks, on purpose.  Every caller is already on #ClawtObserver's
 * worker thread, and an async MCP client would be a main-loop
 * state machine for a call that takes single-digit milliseconds.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * clawt_mcp_socket_call:
 * @socket_path: an MCP server's unix socket
 * @tool: the tool to call
 * @arguments: (nullable) (transfer none): its arguments as an object
 *   node, or %NULL for none
 * @timeout_seconds: how long to wait for the whole exchange
 * @error: (out) (optional): return location for a #GError
 *
 * Connects, initialises, calls @tool once, and closes.
 *
 * A server that answers with `isError` is a failure here rather than a
 * result to inspect: every caller in this tree wants the picture or the
 * click, and a caller that had to remember to check a flag inside a
 * successful return is a caller that will forget.  The message the
 * server wrote is what comes back in @error, not a summary of it -- a
 * compositor that refuses because automation is switched off has said
 * exactly what is wrong, and restating it in our own words loses that.
 *
 * Returns: (transfer full) (nullable): the call's `result` object node
 */
JsonNode *clawt_mcp_socket_call(const gchar  *socket_path,
                                const gchar  *tool,
                                JsonNode     *arguments,
                                guint         timeout_seconds,
                                GError      **error);

/**
 * clawt_mcp_socket_result_image:
 * @result: (transfer none): what clawt_mcp_socket_call() returned
 *
 * The first image in an MCP tool result, decoded.
 *
 * MCP carries an image as a base64 `data` member on a content item, and
 * gowl's screenshot tools use exactly that.  Pulled out here rather than
 * at each caller because a result with no image at all is the ordinary
 * failure -- a compositor that answered with an error string in a text
 * item -- and it must come back as %NULL rather than as empty bytes,
 * which would reach a decoder and surface as a corrupt PNG somewhere
 * far away from the reason.
 *
 * Returns: (transfer full) (nullable): the image bytes
 */
GBytes *clawt_mcp_socket_result_image(JsonNode *result);

/**
 * clawt_mcp_socket_result_text:
 * @result: (transfer none): what clawt_mcp_socket_call() returned
 *
 * The concatenated text content of an MCP tool result.
 *
 * Returns: (transfer full) (nullable): the text
 */
gchar *clawt_mcp_socket_result_text(JsonNode *result);

G_END_DECLS
