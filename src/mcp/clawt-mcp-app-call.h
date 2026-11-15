/*
 * clawt-mcp-app-call.h - An MCP app calling its own server's tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An MCP app's view may ask the host to call a tool on the server that
 * shipped it (`tools/call` over postMessage).  In clawtilla the web
 * client hands that to the daemon (`connector.app_call`), and the daemon
 * runs the call through the same relay an agent's call goes through --
 * `clawtilla connector relay <name> --agent <id> --app-call` -- so the
 * connector's credential, its granted tool list, the agent's action
 * rules and their approvals all apply exactly as they would to the
 * agent, and the relay refuses any tool the server does not list as
 * visible to apps.  This file is the daemon's half: the MCP client that
 * speaks to that relay over its stdio, without blocking the loop.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_MCP_APP_CALL_TIMEOUT_SECONDS:
 *
 * How long one app call may take, approval included, before the relay
 * is stopped and the app is told it timed out.
 */
#define CLAWT_MCP_APP_CALL_TIMEOUT_SECONDS (600)

/**
 * CLAWT_MCP_APP_CALL_MAX_BYTES:
 *
 * The most the relay may write for one message.  Read in bounded chunks
 * and refused past this, never read whole and measured afterwards.
 */
#define CLAWT_MCP_APP_CALL_MAX_BYTES (4 * 1024 * 1024)

/**
 * CLAWT_MCP_APP_CALL_MAX_ARGUMENTS:
 *
 * The largest `arguments` object an app may send, as JSON text.
 */
#define CLAWT_MCP_APP_CALL_MAX_ARGUMENTS (256 * 1024)

/**
 * clawt_mcp_app_call_async: (skip)
 * @argv: (array zero-terminated=1): the relay to start
 * @tool: the tool the app asked for, by the server's own name
 * @arguments: (nullable): its arguments object
 * @timeout_seconds: the hard limit, or 0 for
 *   %CLAWT_MCP_APP_CALL_TIMEOUT_SECONDS
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the result
 * @user_data: data for @callback
 *
 * Starts @argv and speaks MCP to it on its stdio: `initialize`,
 * `tools/list` (every page), then -- only when the list says @tool is
 * visible to apps -- `tools/call`.  The relay refuses such a tool too;
 * this refuses it first, with a sentence, so a call the relay would
 * refuse is never sent.  Everything is asynchronous on the thread-default
 * context at the time of the call, which the daemon pushes; the relay's
 * stdin is closed and the process stopped once the call has an answer.
 *
 * Skipped for introspection: a #JsonNode in and out of an async pair
 * reads poorly in a binding, and only the daemon calls it.
 */
void clawt_mcp_app_call_async(const gchar * const  *argv,
                              const gchar          *tool,
                              JsonNode             *arguments,
                              guint                 timeout_seconds,
                              GCancellable         *cancellable,
                              GAsyncReadyCallback   callback,
                              gpointer              user_data);

/**
 * clawt_mcp_app_call_finish: (skip)
 * @result: the #GAsyncResult
 * @error: (out) (optional): why there is no result: the relay failed or
 *   ended, the server refused, the tool is not visible to apps
 *   (%CLAWT_ERROR_PERMISSION_DENIED), it does not exist
 *   (%CLAWT_ERROR_NOT_FOUND), or it took too long
 *   (%CLAWT_ERROR_TIMEOUT)
 *
 * Returns: (transfer full) (nullable): the `tools/call` result object,
 *   which may itself say `isError` -- a tool that ran and failed, or a
 *   refusal by the relay's gate
 */
JsonObject *clawt_mcp_app_call_finish(GAsyncResult  *result,
                                      GError       **error);

/**
 * clawt_mcp_app_call_note:
 * @connector: the connector
 * @title: (nullable): the app's title
 * @tool: the tool it called
 * @failure: (nullable): why it failed, %NULL when it ran
 *
 * The line put in the conversation for one app-initiated call, so the
 * transcript says the app called the tool -- not the model.  The tool
 * name and the failure are the server's words and are escaped with
 * clawt_preamble_escape_all().
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_mcp_app_call_note(const gchar *connector,
                               const gchar *title,
                               const gchar *tool,
                               const gchar *failure);

G_END_DECLS
