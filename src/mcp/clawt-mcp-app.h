/*
 * clawt-mcp-app.h - An MCP server's app, shown in the conversation
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An MCP server can ship an interactive view for a tool: a `ui://`
 * resource of HTML (MIME type `text/html;profile=mcp-app`) named in the
 * tool's `_meta.ui.resourceUri` -- the MCP Apps extension,
 * `io.modelcontextprotocol/ui`.  mcp-glib 1.0.1 knows nothing of it, so
 * this is done at the JSON level, in the relay, which is the one place a
 * connector's server is ever spoken to.
 *
 * The relay is the only thing that holds the credential and the only
 * time a connection exists, so the view is read there, when the tool is
 * called, and handed to the daemon; the daemon keeps it under the
 * agent's state directory.  The web client fetches it from the daemon
 * only when somebody opens it, and draws it in an iframe with
 * `sandbox="allow-scripts"` -- an opaque origin, so no same-origin access
 * to the client -- from a `srcdoc` whose first element is a
 * Content-Security-Policy built here.  GTK and lrg have no HTML engine;
 * they link to the web client's view of the same app.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * CLAWT_MCP_APP_MIME:
 *
 * The MIME type of an MCP app's view.
 */
#define CLAWT_MCP_APP_MIME "text/html;profile=mcp-app"

/**
 * CLAWT_MCP_APP_EXTENSION:
 *
 * The capability a client advertises in `initialize` so a server knows it
 * can show apps.
 */
#define CLAWT_MCP_APP_EXTENSION "io.modelcontextprotocol/ui"

/**
 * CLAWT_MCP_APP_MAX_HTML:
 *
 * The largest view kept, in bytes.  Past it the call is still shown, as
 * a call whose view was too big.
 */
#define CLAWT_MCP_APP_MAX_HTML (2 * 1024 * 1024)

/**
 * CLAWT_MCP_APP_IFRAME_SANDBOX:
 *
 * The iframe's whole `sandbox` attribute.  Scripts run; nothing else is
 * granted -- no same origin, no forms, no popups, no top navigation.
 */
#define CLAWT_MCP_APP_IFRAME_SANDBOX "allow-scripts"

/**
 * CLAWT_MCP_APP_KEEP_COUNT:
 *
 * How many apps an agent's conversations keep; the oldest go first.
 */
#define CLAWT_MCP_APP_KEEP_COUNT 200

/**
 * CLAWT_MCP_APP_KEEP_SECONDS:
 *
 * How long an app is kept: thirty days.
 */
#define CLAWT_MCP_APP_KEEP_SECONDS (30 * 24 * 60 * 60)

/**
 * clawt_mcp_app_tool_resource:
 * @tool: one tool definition from a `tools/list` result
 *
 * The view a tool ships: `_meta.ui.resourceUri`, or the older flat
 * `_meta["ui/resourceUri"]`.  Only a `ui://` URI is a view.
 *
 * Returns: (transfer full) (nullable): the URI, or %NULL for a tool with
 *   none
 */
gchar *clawt_mcp_app_tool_resource(JsonObject *tool);

/**
 * clawt_mcp_app_tool_model_visible:
 * @tool: one tool definition
 *
 * Whether the model may see and call @tool.  `_meta.ui.visibility`
 * defaults to both the model and the app; a tool whose list leaves the
 * model out belongs to the app's view.
 *
 * Returns: %TRUE when the model may use it
 */
gboolean clawt_mcp_app_tool_model_visible(JsonObject *tool);

/**
 * clawt_mcp_app_tool_app_visible:
 * @tool: one tool definition
 *
 * Whether an app's view may call @tool: `_meta.ui.visibility` names
 * `app`, or is absent (the extension's default is both).  A tool whose
 * list leaves the app out is the model's alone, and an app's call of it
 * is refused.
 *
 * Returns: %TRUE when an app may call it
 */
gboolean clawt_mcp_app_tool_app_visible(JsonObject *tool);

/**
 * clawt_mcp_app_domain_ok:
 * @entry: one origin from a view's `_meta.ui.csp`
 * @websocket: whether `wss://` is allowed too (connect-src)
 * @loopback: whether `http://localhost` and `http://127.0.0.1` are
 *   allowed -- only for a server that is itself local
 *
 * Whether an origin a view asks for may go into its policy: `https://`
 * and a dotted host, a port or a leading `*.` at most, and nothing that
 * could end the directive or the attribute -- no path, query, quote,
 * semicolon, comma or space.
 *
 * Returns: %TRUE if @entry is allowed
 */
gboolean clawt_mcp_app_domain_ok(const gchar *entry,
                                 gboolean     websocket,
                                 gboolean     loopback);

/**
 * clawt_mcp_app_build_csp:
 * @ui: (nullable): the view's `_meta.ui`
 * @loopback: whether a local origin may be allowed
 * @out_refused: (out) (optional) (transfer full) (array zero-terminated=1):
 *   origins the view asked for that were left out
 *
 * The Content-Security-Policy a view runs under: nothing by default,
 * inline script and style, and only the origins its `csp.resourceDomains`
 * and `csp.connectDomains` ask for that clawt_mcp_app_domain_ok() allows,
 * at most sixteen of each.  `'self'` is never named, because the frame's
 * origin is opaque; frames, objects, a `<base>` and form posts are
 * refused outright.
 *
 * Returns: (transfer full): the policy
 */
gchar *clawt_mcp_app_build_csp(JsonObject   *ui,
                               gboolean      loopback,
                               GStrv        *out_refused);

/**
 * clawt_mcp_app_build_document:
 * @html: the view, as the server sent it
 * @csp: its policy, from clawt_mcp_app_build_csp()
 *
 * The iframe's `srcdoc`: a doctype, then the policy as a `<meta>` --
 * before a single byte of the server's -- then the view with its own
 * byte-order mark and doctype taken off.
 *
 * Returns: (transfer full): the document
 */
gchar *clawt_mcp_app_build_document(const gchar *html,
                                    const gchar *csp);

/**
 * clawt_mcp_app_new_id:
 *
 * A fresh app id: `a` and sixteen hex digits.
 *
 * Returns: (transfer full) (nullable): the id
 */
gchar *clawt_mcp_app_new_id(void);

/**
 * clawt_mcp_app_id_is_valid:
 * @id: (nullable): an app id from a client
 *
 * Returns: %TRUE when @id has the shape clawt_mcp_app_new_id() makes
 */
gboolean clawt_mcp_app_id_is_valid(const gchar *id);

/**
 * clawt_mcp_app_web_path:
 * @agent_id: the agent whose conversation it is in
 * @app_id: the app
 *
 * The web client's page for one app, `/a/<agent>/app/<app>`.
 *
 * Returns: (transfer full): the path
 */
gchar *clawt_mcp_app_web_path(const gchar *agent_id,
                              const gchar *app_id);

/**
 * clawt_mcp_app_web_url:
 * @base: (nullable): where the web client is reached --
 *   `connectors.apps_web_url`; %NULL for the default
 * @agent_id: the agent
 * @app_id: the app
 *
 * What GTK and lrg open for an app they cannot draw.
 *
 * Returns: (transfer full): the URL
 */
gchar *clawt_mcp_app_web_url(const gchar *base,
                             const gchar *agent_id,
                             const gchar *app_id);

/**
 * clawt_mcp_app_link_label:
 * @title: (nullable): the app's title
 *
 * What the link to an app says, in GTK and lrg alike.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_mcp_app_link_label(const gchar *title);

/**
 * clawt_mcp_app_store_dir:
 * @config: the fleet configuration
 * @agent_id: the agent
 *
 * Where an agent's apps are kept: `mcp-apps` under its state directory.
 *
 * Returns: (transfer full): the directory
 */
gchar *clawt_mcp_app_store_dir(ClawtConfig *config,
                               const gchar *agent_id);

/**
 * clawt_mcp_app_save:
 * @dir: an agent's app directory, made 0700 if missing
 * @id: the app's id
 * @record: what the relay reported, plus what the daemon adds
 * @error: (out) (optional): return location for a #GError
 *
 * Keeps one app at 0600, then sweeps the directory.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_mcp_app_save(const gchar  *dir,
                            const gchar  *id,
                            JsonObject   *record,
                            GError      **error);

/**
 * clawt_mcp_app_load:
 * @dir: an agent's app directory
 * @id: the app's id
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the record, or %NULL with @error
 *   set when it is not there
 */
JsonObject *clawt_mcp_app_load(const gchar  *dir,
                               const gchar  *id,
                               GError      **error);

/**
 * clawt_mcp_app_sweep:
 * @dir: an agent's app directory
 * @now: the time, in seconds since the epoch
 * @keep_count: how many to keep at most
 * @keep_seconds: how old one may be
 *
 * Returns: how many apps were removed
 */
guint clawt_mcp_app_sweep(const gchar *dir,
                          gint64       now,
                          guint        keep_count,
                          gint64       keep_seconds);

/**
 * ClawtMcpAppsSeenFunc: (skip)
 * @report: what the relay saw of one call of an app's tool -- members
 *   `connector`, `tool`, `title`, `resource_uri`, `html` (absent with
 *   `blocked` saying why when the view could not be kept), `csp`,
 *   `refused`, `input` and `result`
 * @user_data: data for the callback
 *
 * Told once per call of a tool that ships a view.
 */
typedef void (*ClawtMcpAppsSeenFunc)(JsonObject *report,
                                     gpointer    user_data);

/**
 * ClawtMcpApps: (skip)
 *
 * The relay's side of apps: which tools ship a view, which calls are
 * open, and which views it has read.
 */
typedef struct _ClawtMcpApps ClawtMcpApps;

/**
 * clawt_mcp_apps_new: (skip)
 * @connector: (nullable): the connector, for reports
 * @loopback: whether the server is local, which lets a view reach a
 *   local origin
 *
 * Returns: (transfer full): the state
 */
ClawtMcpApps *clawt_mcp_apps_new(const gchar *connector,
                                 gboolean     loopback);

/**
 * clawt_mcp_apps_free: (skip)
 * @self: (nullable): the state
 */
void clawt_mcp_apps_free(ClawtMcpApps *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMcpApps, clawt_mcp_apps_free)

/**
 * clawt_mcp_apps_set_seen_func: (skip)
 * @self: the state
 * @func: (nullable): told of each app call
 * @user_data: data for @func
 */
void clawt_mcp_apps_set_seen_func(ClawtMcpApps         *self,
                                  ClawtMcpAppsSeenFunc  func,
                                  gpointer              user_data);

/**
 * clawt_mcp_apps_set_app_caller: (skip)
 * @self: the state
 * @app_caller: %TRUE when the client on the other side is an app's view
 *   calling its own server's tools, not the agent
 *
 * Turns the relay around for an app's own calls (`connector.app_call`):
 * `tools/list` is passed whole and remembered by visibility, and a
 * `tools/call` is refused unless the server listed that tool as visible
 * to apps (clawt_mcp_app_tool_app_visible()) -- the model's own tools
 * are never an app's to call.  No view is read for these calls.
 */
void clawt_mcp_apps_set_app_caller(ClawtMcpApps *self,
                                   gboolean      app_caller);

/**
 * clawt_mcp_apps_outbound: (skip)
 * @self: the state
 * @line: a message on its way to the server
 * @to_server: (out) (transfer full) (nullable): what to send instead --
 *   `initialize` with the extension advertised, or @line
 * @to_client: (out) (transfer full) (nullable): a refusal, for a call of
 *   a tool that belongs to an app's view
 */
void clawt_mcp_apps_outbound(ClawtMcpApps  *self,
                             const gchar   *line,
                             gchar        **to_server,
                             gchar        **to_client);

/**
 * clawt_mcp_apps_inbound: (skip)
 * @self: the state
 * @line: a message from the server
 * @to_server: (out) (transfer full) (nullable): a `resources/read` of the
 *   relay's own, for a view it has not read yet
 *
 * Watches a message on its way to the client: remembers which tools
 * ship a view (and takes the app's own tools out of the model's list),
 * asks for a view when one of them answers, and keeps the answer to its
 * own request back from the client.
 *
 * Returns: (transfer full) (nullable): what to pass on, or %NULL when
 *   the message was the relay's own answer
 */
gchar *clawt_mcp_apps_inbound(ClawtMcpApps  *self,
                              const gchar   *line,
                              gchar        **to_server);

G_END_DECLS
