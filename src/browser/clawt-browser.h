/*
 * clawt-browser.h - A real web browser an agent can drive
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla_web_fetch reads what a server sends, which for a page built
 * by JavaScript is a script tag and an empty <div>.  A session here is a
 * real browser: gsurf (WebKitGTK) on a private Xvfb, driven over gsurf's
 * control socket -- newline-delimited JSON-RPC, built into every gsurf,
 * with page.text, page.eval, page.click, page.type, page.key and an
 * inline page.screenshot.
 *
 * Nothing here links GTK or WebKit.  The daemon must never link GTK, so
 * a session is two child processes and a socket, and libclawt only ever
 * speaks JSON to them.
 *
 * Everything that talks to a session blocks: starting one waits for an X
 * server and a browser, and a call waits for a page.  So every caller
 * runs on a worker thread -- the agent tools and the IPC verbs both go
 * through g_task_run_in_thread() -- and a session serialises its own
 * calls with a mutex, since a frame for the Computer page and an agent's
 * click can arrive together.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * clawt_browser_build_available:
 *
 * Whether this build can start a browser at all: it was built with
 * `GSURF=1`.  An agent configured for a browser in a build without one is
 * disabled with that reason, never run without the tools it was told it
 * has.
 *
 * Returns: %TRUE in a `GSURF=1` build
 */
gboolean clawt_browser_build_available(void);

/**
 * clawt_browser_resolve_gsurf:
 * @error: (out) (optional): return location for a #GError
 *
 * The gsurf to run, best first: beside the running binary, the install
 * location, the `GSURF_DIR` build this binary was made against, then
 * `PATH`.  A candidate older than 0.5.13 is passed over.  None found
 * names every place that was looked in, and why each was not used.
 *
 * Returns: (transfer full) (nullable): an absolute path, or %NULL
 */
gchar *clawt_browser_resolve_gsurf(GError **error);

#define CLAWT_TYPE_BROWSER (clawt_browser_get_type())

G_DECLARE_FINAL_TYPE(ClawtBrowser, clawt_browser, CLAWT, BROWSER, GObject)

/**
 * ClawtBrowserTransport:
 * @browser: the session being called
 * @method: the control method
 * @params: (nullable): its parameters, an object node
 * @error: (out) (optional): return location for a #GError
 * @user_data: what was given to clawt_browser_set_transport()
 *
 * Answers a control call in place of a real gsurf.  For tests: everything
 * above the socket -- the tools, the driver lease, the IPC verbs -- can
 * then run with no X server and no browser.
 *
 * Returns: (transfer full) (nullable): the call's `result`
 */
typedef JsonNode *(*ClawtBrowserTransport)(ClawtBrowser  *browser,
                                           const gchar   *method,
                                           JsonNode      *params,
                                           GError       **error,
                                           gpointer       user_data);

/**
 * clawt_browser_new:
 * @id: the session's id: an agent's id for its own, a `browsers:` id for
 *   a shared one
 * @dir: the session's directory: its profile, cache, config and logs
 *
 * Returns: (transfer full): a session, not yet started
 */
ClawtBrowser *clawt_browser_new(const gchar *id, const gchar *dir);

/**
 * clawt_browser_get_id:
 * @self: a #ClawtBrowser
 *
 * Returns: (transfer none): the session's id
 */
const gchar *clawt_browser_get_id(ClawtBrowser *self);

/**
 * clawt_browser_get_dir:
 * @self: a #ClawtBrowser
 *
 * Returns: (transfer none): the session's directory
 */
const gchar *clawt_browser_get_dir(ClawtBrowser *self);

/**
 * clawt_browser_configure:
 * @self: a #ClawtBrowser
 * @home: (nullable): the page it opens on
 * @width: window width in pixels, 0 for 1280
 * @height: window height in pixels, 0 for 800
 * @idle_minutes: stop after this long without a call, 0 for never
 *
 * Applied at the next start; a running session keeps its window.
 */
void clawt_browser_configure(ClawtBrowser *self,
                             const gchar  *home,
                             gint          width,
                             gint          height,
                             gint          idle_minutes);

/**
 * clawt_browser_set_transport:
 * @self: a #ClawtBrowser
 * @transport: (nullable) (scope notified) (closure user_data): answers calls instead of gsurf
 * @user_data: passed to @transport
 * @destroy: (nullable): frees @user_data
 *
 * With a transport set the session never spawns anything and counts as
 * running.  For tests.
 */
void clawt_browser_set_transport(ClawtBrowser          *self,
                                 ClawtBrowserTransport  transport,
                                 gpointer               user_data,
                                 GDestroyNotify         destroy);

/**
 * clawt_browser_start:
 * @self: a #ClawtBrowser
 * @error: (out) (optional): return location for a #GError
 *
 * Starts the X server and the browser and waits for the control socket.
 * Blocks for seconds: call from a worker thread.  A running session is
 * left alone.
 *
 * Returns: %TRUE when the session answers
 */
gboolean clawt_browser_start(ClawtBrowser *self, GError **error);

/**
 * clawt_browser_stop:
 * @self: a #ClawtBrowser
 *
 * Stops the browser, then the X server, waiting for each.  The profile
 * stays, so a login survives; the open page does not.
 */
void clawt_browser_stop(ClawtBrowser *self);

/**
 * clawt_browser_is_running:
 * @self: a #ClawtBrowser
 *
 * Returns: %TRUE while the browser process is alive (or a transport is set)
 */
gboolean clawt_browser_is_running(ClawtBrowser *self);

/**
 * clawt_browser_call:
 * @self: a #ClawtBrowser
 * @method: a gsurf control method (`page.text`, `navigate`, ...)
 * @params: (nullable) (transfer none): its parameters, an object node
 * @timeout_seconds: how long to wait for the answer
 * @error: (out) (optional): return location for a #GError
 *
 * One call, starting the session first if it is not running.  Blocks:
 * call from a worker thread.  Counts as use for the idle stop.
 *
 * Returns: (transfer full) (nullable): the call's `result`
 */
JsonNode *clawt_browser_call(ClawtBrowser  *self,
                             const gchar   *method,
                             JsonNode      *params,
                             guint          timeout_seconds,
                             GError       **error);

/**
 * clawt_browser_refresh_page:
 * @self: a #ClawtBrowser
 *
 * Reads the page's address and title into the session's record, for a
 * listing that must not wait on the browser.  Blocks; failures are
 * ignored, since a page mid-load answers neither.
 */
void clawt_browser_refresh_page(ClawtBrowser *self);

/**
 * clawt_browser_dup_uri:
 * @self: a #ClawtBrowser
 *
 * Returns: (transfer full) (nullable): the address last read
 */
gchar *clawt_browser_dup_uri(ClawtBrowser *self);

/**
 * clawt_browser_dup_title:
 * @self: a #ClawtBrowser
 *
 * Returns: (transfer full) (nullable): the title last read
 */
gchar *clawt_browser_dup_title(ClawtBrowser *self);

/**
 * clawt_browser_get_last_used:
 * @self: a #ClawtBrowser
 *
 * Returns: unix seconds of the last call, 0 for never
 */
gint64 clawt_browser_get_last_used(ClawtBrowser *self);

/**
 * clawt_browser_is_idle:
 * @self: a #ClawtBrowser
 * @now: unix seconds
 *
 * Returns: %TRUE when the session is running and has gone its idle
 *   minutes without a call
 */
gboolean clawt_browser_is_idle(ClawtBrowser *self, gint64 now);

/**
 * clawt_browser_claim:
 * @self: a #ClawtBrowser
 * @agent_id: the agent about to drive it
 * @now: unix seconds
 * @error: (out) (optional): return location for a #GError
 *
 * Takes the wheel of a shared session.  One agent drives at a time: two
 * agents clicking in one page undo each other's work and neither can
 * tell.  The claim lapses after %CLAWT_BROWSER_DRIVER_LEASE_SECONDS
 * without a call from its holder, so an agent that forgot to release is
 * not a session locked until the daemon restarts.
 *
 * Returns: %TRUE when @agent_id may drive; otherwise the error names the
 *   driver
 */
gboolean clawt_browser_claim(ClawtBrowser *self,
                             const gchar  *agent_id,
                             gint64        now,
                             GError      **error);

/**
 * clawt_browser_release:
 * @self: a #ClawtBrowser
 * @agent_id: the agent letting go
 *
 * Releases a claim @agent_id holds; anybody else's is left alone.
 *
 * Returns: %TRUE when @agent_id held it
 */
gboolean clawt_browser_release(ClawtBrowser *self, const gchar *agent_id);

/**
 * clawt_browser_dup_driver:
 * @self: a #ClawtBrowser
 * @now: unix seconds
 *
 * Returns: (transfer full) (nullable): the agent holding the wheel, or
 *   %NULL when nobody does or the claim has lapsed
 */
gchar *clawt_browser_dup_driver(ClawtBrowser *self, gint64 now);

/**
 * CLAWT_BROWSER_DRIVER_LEASE_SECONDS:
 *
 * How long a claim on a shared session lasts without a call.
 */
#define CLAWT_BROWSER_DRIVER_LEASE_SECONDS (120)

/**
 * CLAWT_BROWSER_SECRET_HOLD_MESSAGE:
 *
 * What a refused screenshot or script says while a secret is on the page.
 */
#define CLAWT_BROWSER_SECRET_HOLD_MESSAGE \
    "a secret was just filled on this page, so screenshots and scripts are " \
    "refused here until the browser navigates away -- submit the form or open " \
    "another page; clawtilla_browser_read still works"

/**
 * clawt_browser_fill_secret:
 * @self: a #ClawtBrowser
 * @selector: the field, a CSS selector
 * @value: the secret, or the one-time code made from one
 * @submit: press Enter after it
 * @name: the vault entry it came from, kept for the clients to show
 * @host: (nullable): the host the caller checked the page is on: gsurf
 *   refuses the fill unless the page is still there when it has found the
 *   field, and it is kept for the clients to show
 * @error: (out) (optional): return location for a #GError
 *
 * Has gsurf type @value into the field as key input (`page.fill_secret`,
 * gsurf 0.5.13), replacing what was there.  The session's secret hold is
 * set *before* anything is sent, so a fill that fails half-way still
 * refuses screenshots and scripts; it lasts until gsurf reports that
 * the page navigated away (clawt_browser_secret_held()) or the session
 * stops.  Blocks.  No error carries @value, and this copy of it is
 * wiped once sent.
 *
 * Returns: %TRUE when gsurf typed it
 */
gboolean clawt_browser_fill_secret(ClawtBrowser  *self,
                                   const gchar   *selector,
                                   const gchar   *value,
                                   gboolean       submit,
                                   const gchar   *name,
                                   const gchar   *host,
                                   GError       **error);

/**
 * clawt_browser_get_secret_hold:
 * @self: a #ClawtBrowser
 *
 * The hold as last known, without asking the browser -- for a listing
 * on the daemon's context, which must not wait on a page.
 *
 * Returns: whether a secret may be on the page
 */
gboolean clawt_browser_get_secret_hold(ClawtBrowser *self);

/**
 * clawt_browser_secret_held:
 * @self: a #ClawtBrowser
 *
 * Whether a secret is still on the page, asked of gsurf when one was
 * filled (`page.secret_hold`).  Fails closed: only gsurf answering that
 * its hold is gone lifts ours.  Blocks.
 *
 * Returns: %TRUE while screenshots and scripts must be refused
 */
gboolean clawt_browser_secret_held(ClawtBrowser *self);

/**
 * clawt_browser_dup_last_fill:
 * @self: a #ClawtBrowser
 * @out_name: (out) (optional) (transfer full) (nullable): the entry's name
 * @out_host: (out) (optional) (transfer full) (nullable): the host
 * @out_at: (out) (optional): unix seconds
 *
 * The last secret this session filled -- by name, never by value.
 *
 * Returns: %TRUE when there has been one
 */
gboolean clawt_browser_dup_last_fill(ClawtBrowser  *self,
                                     gchar        **out_name,
                                     gchar        **out_host,
                                     gint64        *out_at);

/**
 * clawt_browser_screenshot:
 * @self: a #ClawtBrowser
 * @error: (out) (optional): return location for a #GError
 *
 * A PNG of the visible page.  Blocks.  Refused with
 * %CLAWT_ERROR_PERMISSION_DENIED while a secret is on it
 * (clawt_browser_secret_held()).
 *
 * Returns: (transfer full) (nullable): the PNG's bytes
 */
GBytes *clawt_browser_screenshot(ClawtBrowser *self, GError **error);

/**
 * clawt_browser_wait_loaded:
 * @self: a #ClawtBrowser
 * @timeout_seconds: the longest to wait
 *
 * Polls `document.readyState` until it is `complete`.  A reply from
 * gsurf means a command was handed to the engine, not that the page has
 * handled it, so a navigation or a submit is followed by this.  Blocks.
 *
 * Returns: %TRUE when the page finished loading in time
 */
gboolean clawt_browser_wait_loaded(ClawtBrowser *self, guint timeout_seconds);

/* ── What the clients draw ─────────────────────────────────────────── */

/**
 * clawt_browser_list_find:
 * @reply: (nullable): a `browser.list` reply object
 * @agent_id: an agent
 *
 * Returns: (transfer none) (nullable): the session @agent_id drives, from
 *   the reply's `sessions`
 */
JsonObject *clawt_browser_list_find(JsonObject *reply, const gchar *agent_id);

/**
 * clawt_browser_session_line:
 * @session: (nullable): a session object, from `browser.list` or a
 *   `browser.*` reply
 *
 * The line every client draws above the picture: "Running -- Example
 * Domain (https://example.org/) -- driven by alpha", or that it is not
 * running and starts on the first browser call.
 *
 * When a secret has been filled in the session, a second line follows
 * from clawt_browser_fill_line() at the current time.
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_browser_session_line(JsonObject *session);

/**
 * clawt_browser_fill_line:
 * @session: (nullable): a session object with `secret_hold` and `last_fill`
 * @now: the current time, in microseconds since the epoch
 *
 * What every client says about the last secret filled: by the vault
 * entry's name and the host, never a value -- and, while the hold is on,
 * that there will be no picture until the page changes.
 *
 * Returns: (transfer full) (nullable): the line, or %NULL when nothing
 *   was filled
 */
gchar *clawt_browser_fill_line(JsonObject *session, gint64 now);

/**
 * clawt_vault_entry_label:
 * @entry: one of `vault.list`'s `entries`
 *
 * "github -- a password, on github.com: the bot's login", the same in
 * every client.  Names, never values: `vault.list` carries none.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_vault_entry_label(JsonObject *entry);

/**
 * clawt_vault_grants_with:
 * @reply: (nullable): a `vault.list` reply for one agent
 * @name: the entry being switched
 * @granted: whether it should be granted
 *
 * The agent's `browser.secrets` with @name switched, in the vault's
 * order, as the comma-separated value `agent.set` takes -- so a client
 * toggling one entry sends the whole list, and two clients agree on its
 * spelling.
 *
 * Returns: (transfer full): the list, possibly empty
 */
gchar *clawt_vault_grants_with(JsonObject *reply, const gchar *name, gboolean granted);

/* ── The sessions a daemon holds ──────────────────────────────────── */

#define CLAWT_TYPE_BROWSER_MANAGER (clawt_browser_manager_get_type())

G_DECLARE_FINAL_TYPE(ClawtBrowserManager, clawt_browser_manager, CLAWT,
                     BROWSER_MANAGER, GObject)

/**
 * clawt_browser_manager_new:
 * @root: where sessions keep their directories (`<state_dir>/browsers`)
 *
 * Returns: (transfer full): an empty manager
 */
ClawtBrowserManager *clawt_browser_manager_new(const gchar *root);

/**
 * clawt_browser_manager_session_key:
 * @mode: the agent's `browser.mode`
 * @agent_id: the agent
 * @use: (nullable): its `browser.use`
 *
 * The one spelling of which session an agent drives: `agent:<id>` for its
 * own, `shared:<use>` for a shared one.
 *
 * Returns: (transfer full) (nullable): the key, or %NULL for no browser
 */
gchar *clawt_browser_manager_session_key(ClawtBrowserMode  mode,
                                         const gchar      *agent_id,
                                         const gchar      *use);

/**
 * clawt_browser_manager_obtain:
 * @self: a #ClawtBrowserManager
 * @key: a key from clawt_browser_manager_session_key()
 *
 * The session for @key, made (not started) the first time it is asked
 * for.  Cheap: never touches a process.
 *
 * Returns: (transfer full): the session
 */
ClawtBrowser *clawt_browser_manager_obtain(ClawtBrowserManager *self,
                                           const gchar         *key);

/**
 * clawt_browser_manager_lookup:
 * @self: a #ClawtBrowserManager
 * @key: a session key
 *
 * Returns: (transfer full) (nullable): the session, or %NULL if it was
 *   never made
 */
ClawtBrowser *clawt_browser_manager_lookup(ClawtBrowserManager *self,
                                           const gchar         *key);

/**
 * clawt_browser_manager_list:
 * @self: a #ClawtBrowserManager
 *
 * Returns: (transfer full) (element-type ClawtBrowser): every session made,
 *   sorted by key
 */
GPtrArray *clawt_browser_manager_list(ClawtBrowserManager *self);

/**
 * clawt_browser_manager_set_transport:
 * @self: a #ClawtBrowserManager
 * @transport: (nullable) (scope forever) (closure user_data): given to every session made
 *   from now on
 * @user_data: passed to @transport
 *
 * For tests: sessions answer from @transport and never spawn.
 */
void clawt_browser_manager_set_transport(ClawtBrowserManager   *self,
                                         ClawtBrowserTransport  transport,
                                         gpointer               user_data);

/**
 * clawt_browser_manager_stop_all:
 * @self: a #ClawtBrowserManager
 *
 * Stops every running session.  Blocks.
 */
void clawt_browser_manager_stop_all(ClawtBrowserManager *self);

G_END_DECLS
