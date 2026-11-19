/*
 * clawt-display-server.h - A private display that GUI apps draw on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One virtual screen, started for a `display` computer (since 0.51.0):
 * gowl run headless, Xvfb, or TigerVNC's Xvnc.  The object starts the
 * server, tells commands how to reach it (DISPLAY, WAYLAND_DISPLAY),
 * takes a picture of it, sends it keys and clicks, and starts apps on
 * it.  It knows nothing about agents; ClawtDisplayComputer is the
 * computer that owns one.
 *
 * The children it starts die with the daemon: they are forked from one
 * thread that lasts as long as the process (clawt_display_spawn_lasting()),
 * because PR_SET_PDEATHSIG follows the forking *thread*.  The browser's
 * Xvfb is forked there too.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "clawt-enums.h"
#include "computer/clawt-observable.h"
#include "computer/clawt-sandbox.h"

G_BEGIN_DECLS

/* ── Children that die with the daemon ───────────────────────────── */

/**
 * clawt_display_spawn_lasting:
 * @launcher: how to start it; its child setup is replaced by one that
 *   asks for SIGTERM when the daemon goes
 * @argv: (array zero-terminated=1): the command
 * @error: (out) (optional): return location for a #GError
 *
 * g_subprocess_launcher_spawnv(), forked from one thread that lasts as
 * long as the process, so the child is signalled when the daemon dies
 * rather than when the thread that asked happens to end.
 *
 * Returns: (transfer full) (nullable): the child
 */
GSubprocess *clawt_display_spawn_lasting(GSubprocessLauncher *launcher,
                                         const gchar * const *argv,
                                         GError             **error);

/**
 * clawt_display_child_alive:
 * @child: (nullable): a child
 *
 * Returns: %TRUE while the kernel still has it
 */
gboolean clawt_display_child_alive(GSubprocess *child);

/**
 * clawt_display_end_child:
 * @child: (nullable): a child
 *
 * SIGTERM, a short wait, then SIGKILL.  A child already gone is fine.
 */
void clawt_display_end_child(GSubprocess *child);

/**
 * clawt_display_read_displayfd:
 * @server: an X server started with `-displayfd 1` and a stdout pipe
 * @timeout_ms: how long to wait for it
 * @error: (out) (optional): return location for a #GError
 *
 * The display number the server chose, as it wrote it.
 *
 * Returns: the number, or -1 with @error set
 */
gint clawt_display_read_displayfd(GSubprocess *server,
                                  guint        timeout_ms,
                                  GError     **error);

/* ── Pure helpers (tested without a display) ─────────────────────── */

/**
 * clawt_display_png_encode:
 * @width: pixels across
 * @height: pixels down
 * @rgb: (array): @height rows of @width packed RGB triples
 * @error: (out) (optional): return location for a #GError
 *
 * A PNG of @rgb, written here with GIO's zlib compressor so no image
 * library is linked for it.
 *
 * Returns: (transfer full) (nullable): the PNG
 */
GBytes *clawt_display_png_encode(guint         width,
                                 guint         height,
                                 const guint8 *rgb,
                                 GError      **error);

/**
 * clawt_display_xwd_to_png:
 * @data: (array length=length): an XWD dump, as Xvfb's `-fbdir` keeps it
 * @length: its size
 * @error: (out) (optional): return location for a #GError
 *
 * The screen in Xvfb's memory-mapped framebuffer file, as a PNG.  ZPixmap
 * at 24 or 32 bits per pixel, either byte order; anything else is
 * refused with what it was.
 *
 * Returns: (transfer full) (nullable): the PNG
 */
GBytes *clawt_display_xwd_to_png(const guint8 *data,
                                 gsize         length,
                                 GError      **error);

/**
 * clawt_display_xdotool_argv:
 * @xdotool: the xdotool binary
 * @event: what to do
 *
 * The xdotool command that performs @event on the display in DISPLAY.
 * A click moves first, so it lands where it was aimed rather than where
 * the pointer was left.
 *
 * Returns: (transfer full) (nullable): the argv, or %NULL for an event
 *   with nothing to send
 */
GStrv clawt_display_xdotool_argv(const gchar     *xdotool,
                                 ClawtInputEvent *event);

/**
 * clawt_display_import_argv:
 * @import: ImageMagick's import
 * @display: the X display, e.g. ":3"
 *
 * Returns: (transfer full): the command that writes the root window as a
 *   PNG to standard output
 */
GStrv clawt_display_import_argv(const gchar *import,
                                const gchar *display);

/**
 * clawt_display_server_argv:
 * @backend: %CLAWT_DISPLAY_BACKEND_XVFB, _XVNC or _GOWL
 * @binary: the server binary
 * @width: pixels across
 * @height: pixels down
 * @depth: bits per pixel (X servers)
 * @fbdir: (nullable): where Xvfb keeps its framebuffer file
 * @vnc_port: Xvnc's loopback port
 * @config: (nullable): gowl's generated config
 *
 * The command that starts the server.  X servers write the display they
 * chose to standard output (`-displayfd 1`) and listen on no TCP port;
 * Xvnc's VNC server listens on loopback only, with no password -- it is
 * reachable from this machine alone, as the daemon's socket is.
 *
 * Returns: (transfer full): the argv
 */
GStrv clawt_display_server_argv(ClawtDisplayBackend backend,
                                const gchar        *binary,
                                guint               width,
                                guint               height,
                                guint               depth,
                                const gchar        *fbdir,
                                guint               vnc_port,
                                const gchar        *config);

/**
 * clawt_display_gowl_config:
 *
 * The YAML a headless gowl is started with: its IPC and its MCP module
 * switched on, nothing else of the operator's own config.
 *
 * Returns: (transfer full): the YAML
 */
gchar *clawt_display_gowl_config(void);

/**
 * clawt_display_gowl_environment:
 * @run_dir: the private runtime directory
 * @width: pixels across
 * @height: pixels down
 *
 * What a headless gowl is started with beyond the shared allowlist: its
 * own XDG_RUNTIME_DIR (so its sockets are not the operator's), wlroots'
 * headless backend and software renderer, the output size, and no
 * systemd session.
 *
 * Returns: (transfer full) (element-type utf8 utf8): the variables
 */
GHashTable *clawt_display_gowl_environment(const gchar *run_dir,
                                           guint        width,
                                           guint        height);

/**
 * clawt_display_parse_resolution:
 * @text: (nullable): e.g. "1280x800"
 * @width: (out): pixels across
 * @height: (out): pixels down
 *
 * Returns: %TRUE for a sensible size (64 to 8192 each way)
 */
gboolean clawt_display_parse_resolution(const gchar *text,
                                        guint       *width,
                                        guint       *height);

/**
 * clawt_display_resolve:
 * @backend: what was asked for; %CLAWT_DISPLAY_BACKEND_AUTO tries gowl,
 *   then Xvnc, then Xvfb
 * @gowl_binary: (nullable): a pinned gowl, tried instead of searching
 * @chosen: (out): the backend that works
 * @binary: (out) (transfer full): its binary
 * @notes: (out) (transfer full) (nullable): what was skipped and why,
 *   one line each, or %NULL
 * @error: (out) (optional): return location for a #GError
 *
 * Which display server to run, found by running each candidate rather
 * than by taking the first on PATH: a gowl that cannot load its wlroots
 * sits in /usr/local/bin ahead of a working one surprisingly often.
 *
 * Returns: %TRUE when one works
 */
gboolean clawt_display_resolve(ClawtDisplayBackend  backend,
                               const gchar         *gowl_binary,
                               ClawtDisplayBackend *chosen,
                               gchar              **binary,
                               gchar              **notes,
                               GError             **error);

/* ── The server ──────────────────────────────────────────────────── */

#define CLAWT_TYPE_DISPLAY_SERVER (clawt_display_server_get_type())

G_DECLARE_FINAL_TYPE(ClawtDisplayServer, clawt_display_server,
                     CLAWT, DISPLAY_SERVER, GObject)

/**
 * clawt_display_server_new:
 * @backend: what to run (%CLAWT_DISPLAY_BACKEND_AUTO resolves at start)
 * @dir: a directory of its own, for logs, the framebuffer and gowl's
 *   sockets; created at start and emptied at stop
 * @width: pixels across
 * @height: pixels down
 *
 * Returns: (transfer full): a server, not yet started
 */
ClawtDisplayServer *clawt_display_server_new(ClawtDisplayBackend backend,
                                             const gchar        *dir,
                                             guint               width,
                                             guint               height);

/**
 * clawt_display_server_set_depth:
 * @self: a #ClawtDisplayServer
 * @depth: bits per pixel for an X server (16, 24 or 32)
 */
void clawt_display_server_set_depth(ClawtDisplayServer *self, guint depth);

/**
 * clawt_display_server_set_vnc_port:
 * @self: a #ClawtDisplayServer
 * @port: Xvnc's loopback port, or 0 for one picked free at start
 */
void clawt_display_server_set_vnc_port(ClawtDisplayServer *self, guint port);

/**
 * clawt_display_server_set_gowl_binary:
 * @self: a #ClawtDisplayServer
 * @path: (nullable): a gowl to use instead of searching
 */
void clawt_display_server_set_gowl_binary(ClawtDisplayServer *self,
                                          const gchar        *path);

/**
 * clawt_display_server_start:
 * @self: a #ClawtDisplayServer
 * @error: (out) (optional): return location for a #GError
 *
 * Starts the server and waits until it can be reached.  Already running
 * is success.
 *
 * Returns: %TRUE when the screen is up
 */
gboolean clawt_display_server_start(ClawtDisplayServer *self,
                                    GError            **error);

/**
 * clawt_display_server_stop:
 * @self: a #ClawtDisplayServer
 *
 * Stops the apps it started, then the server, and empties its directory.
 */
void clawt_display_server_stop(ClawtDisplayServer *self);

/**
 * clawt_display_server_is_running:
 * @self: a #ClawtDisplayServer
 *
 * Returns: %TRUE while the server process is alive
 */
gboolean clawt_display_server_is_running(ClawtDisplayServer *self);

/**
 * clawt_display_server_get_backend:
 * @self: a #ClawtDisplayServer
 *
 * Returns: the backend running, or the one asked for before a start
 */
ClawtDisplayBackend clawt_display_server_get_backend(ClawtDisplayServer *self);

/**
 * clawt_display_server_get_size:
 * @self: a #ClawtDisplayServer
 * @width: (out) (optional): pixels across
 * @height: (out) (optional): pixels down
 */
void clawt_display_server_get_size(ClawtDisplayServer *self,
                                   guint              *width,
                                   guint              *height);

/**
 * clawt_display_server_dup_environment:
 * @self: a #ClawtDisplayServer
 *
 * What a command needs to draw on this screen: DISPLAY for an X server;
 * WAYLAND_DISPLAY (an absolute socket path) for gowl.  Empty before a
 * start.
 *
 * Returns: (transfer full) (element-type utf8 utf8): the variables
 */
GHashTable *clawt_display_server_dup_environment(ClawtDisplayServer *self);

/**
 * clawt_display_server_dup_bind_paths:
 * @self: a #ClawtDisplayServer
 *
 * The paths a sandbox must bind for a client to reach the screen, and
 * nothing more: the X socket `/tmp/.X11-unix/X<N>` for an X server, the
 * Wayland socket file for gowl.  Never a directory (0.51.4): the X
 * socket directory holds the operator's own X server, and gowl's runtime
 * directory holds gowl.sock and gowl-mcp.sock, whose `spawn` runs a
 * command outside any box.  Empty before a start; N changes per start.
 *
 * Returns: (transfer full): the paths, possibly empty
 */
GStrv clawt_display_server_dup_bind_paths(ClawtDisplayServer *self);

/**
 * clawt_display_server_dup_notes:
 * @self: a #ClawtDisplayServer
 *
 * Returns: (transfer full) (nullable): what the last resolution skipped,
 *   and why, or %NULL
 */
gchar *clawt_display_server_dup_notes(ClawtDisplayServer *self);

/**
 * clawt_display_server_dup_failure:
 * @self: a #ClawtDisplayServer
 *
 * Why the server is not running, when it died: the last lines it wrote.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL
 */
gchar *clawt_display_server_dup_failure(ClawtDisplayServer *self);

/**
 * clawt_display_server_get_mcp_socket:
 * @self: a #ClawtDisplayServer
 *
 * Returns: (nullable): gowl's MCP socket, or %NULL for an X server
 */
const gchar *clawt_display_server_get_mcp_socket(ClawtDisplayServer *self);

/**
 * clawt_display_server_dup_viewer_uri:
 * @self: a #ClawtDisplayServer
 *
 * Returns: (transfer full) (nullable): `vnc://127.0.0.1:<port>` for Xvnc,
 *   else %NULL
 */
gchar *clawt_display_server_dup_viewer_uri(ClawtDisplayServer *self);

/**
 * clawt_display_server_capture:
 * @self: a #ClawtDisplayServer
 * @error: (out) (optional): return location for a #GError
 *
 * A picture of the whole screen, as a PNG.
 *
 * Returns: (transfer full) (nullable): the PNG
 */
GBytes *clawt_display_server_capture(ClawtDisplayServer *self,
                                     GError            **error);

/**
 * clawt_display_server_send_input:
 * @self: a #ClawtDisplayServer
 * @event: what to do
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was sent
 */
gboolean clawt_display_server_send_input(ClawtDisplayServer *self,
                                         ClawtInputEvent    *event,
                                         GError            **error);

/**
 * clawt_display_server_launch:
 * @self: a #ClawtDisplayServer
 * @command: a shell command line
 * @environment: (nullable) (element-type utf8 utf8): extra variables
 * @sandbox: (nullable): the agent's confinement, applied to the app as it
 *   is to a command it runs (`computer.host.confine`)
 * @pid: (out) (optional): the app's process id, or 0 when the compositor
 *   that started it does not say
 * @error: (out) (optional): return location for a #GError
 *
 * Starts an app on the screen, not waiting for it.  On gowl, unconfined,
 * it goes through the compositor's own spawn -- its JSON-RPC `spawn`
 * where it has one, which answers with the pid, else its MCP `spawn`
 * tool -- so it gets XWayland's DISPLAY as well as WAYLAND_DISPLAY;
 * under bwrap it is started here, inside the box, as a Wayland client.
 * The app is stopped with the server.
 *
 * @pid is set to 0 when there is none to give: gowl's MCP spawn does not
 * say, and an app that finished before it was asked (a startup `echo`)
 * has already been reaped.  Neither is a failure.
 *
 * Returns: %TRUE when it was started
 */
gboolean clawt_display_server_launch(ClawtDisplayServer *self,
                                     const gchar        *command,
                                     GHashTable         *environment,
                                     ClawtSandbox       *sandbox,
                                     gint               *pid,
                                     GError            **error);

/**
 * clawt_display_server_list_windows:
 * @self: a #ClawtDisplayServer
 * @error: (out) (optional): return location for a #GError
 *
 * The windows on the screen: each `id` and `title`, and `app` where the
 * server knows it.
 *
 * Returns: (transfer full) (nullable): an array node
 */
JsonNode *clawt_display_server_list_windows(ClawtDisplayServer *self,
                                            GError            **error);

/**
 * clawt_display_server_dup_focused_title:
 * @self: a #ClawtDisplayServer
 *
 * Returns: (transfer full) (nullable): the focused window's title
 */
gchar *clawt_display_server_dup_focused_title(ClawtDisplayServer *self);

G_END_DECLS
