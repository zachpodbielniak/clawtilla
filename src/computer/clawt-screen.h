/*
 * clawt-screen.h - Asking a compositor for a frame, and typing at it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two desktop backends, three places they are reached from -- gowl on
 * the daemon's own machine, GNOME on the daemon's own machine, and GNOME
 * inside an agent's VM over SSH.  The protocol details are here so that
 * the three differ only in *how the command is delivered*, which is the
 * one thing they genuinely differ in.
 *
 * Everything in this file that builds a command line is a pure function
 * and returns an argv, so what would be sent can be asserted on without
 * a compositor, a VM or a session bus.  That matters more here than
 * usual: getting one of these wrong produces a screen that never
 * updates, which looks exactly like a screen where nothing is happening.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

#include "computer/clawt-observable.h"

G_BEGIN_DECLS

/**
 * CLAWT_SCREEN_GNOME_BUS_NAME:
 *
 * gnome-desktop-mcp's half lives inside GNOME Shell, so its methods are
 * on the Shell's own bus name rather than on one of its own.
 */
#define CLAWT_SCREEN_GNOME_BUS_NAME "org.gnome.Shell"

/**
 * CLAWT_SCREEN_GNOME_OBJECT_PATH:
 *
 * Where the desktop-automation extension exports its interface.
 */
#define CLAWT_SCREEN_GNOME_OBJECT_PATH "/io/github/gnomemcp/DesktopAutomation"

/**
 * CLAWT_SCREEN_GNOME_INTERFACE:
 *
 * The interface those methods are on.
 */
#define CLAWT_SCREEN_GNOME_INTERFACE "io.github.gnomemcp.DesktopAutomation"

/**
 * CLAWT_SCREEN_FRAME_WIDTH:
 *
 * The width a supervision frame is downscaled to, in the compositor.
 *
 * Asked for rather than taken, because `Shell.Screenshot` hands back the
 * whole framebuffer -- which on a HiDPI guest is 3840 wide -- and a
 * preview panel in a sidebar is a few hundred pixels.  Downscaling where
 * the pixels already are is the difference between a frame that costs a
 * megabyte to move and one that costs tens of kilobytes.
 */
#define CLAWT_SCREEN_FRAME_WIDTH 1280

/**
 * ClawtScreenFrameInfo:
 * @path: (nullable): where the compositor wrote the frame, spelled the
 *   way the *screen's* machine spells it
 * @width: the written image's width
 * @height: its height
 * @hash: (nullable): a SHA-256 of the file's bytes
 * @stamp: when it was taken, microseconds since the epoch
 *
 * What `ScreenshotFrame` answers.
 *
 * @path is the guest's spelling for a VM, which is why nothing here
 * opens it: translating that to a path on this machine is the backend's
 * job, and it is the trap `docs/computers.org` already records for every
 * other path an agent is handed.
 */
typedef struct {
    gchar  *path;
    guint   width;
    guint   height;
    gchar  *hash;
    gint64  stamp;
} ClawtScreenFrameInfo;

/**
 * clawt_screen_frame_info_clear:
 * @self: a #ClawtScreenFrameInfo
 *
 * Frees what the parse allocated, leaving the struct zeroed.
 */
void clawt_screen_frame_info_clear(ClawtScreenFrameInfo *self);

/**
 * clawt_screen_gnome_frame_argv:
 * @max_width: the width to downscale to, or 0 for the framebuffer's own
 * @include_cursor: whether to draw the pointer into the frame
 *
 * The `gdbus` command that asks for one frame.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv clawt_screen_gnome_frame_argv(guint max_width, gboolean include_cursor);

/**
 * clawt_screen_gnome_monitors_argv:
 *
 * The `gdbus` command that lists the monitors.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv clawt_screen_gnome_monitors_argv(void);

/**
 * clawt_screen_parse_gdbus_string:
 * @text: what `gdbus call` printed
 *
 * The single string a one-string method answers with.
 *
 * gdbus prints a tuple even for one value -- `('[...]',)` -- so the
 * quotes and the parentheses have to come off before the JSON inside is
 * parseable. Shared with the frame parser's tokeniser so a path or a
 * payload containing a quote is handled the same way in both.
 *
 * Returns: (transfer full) (nullable): the string
 */
gchar *clawt_screen_parse_gdbus_string(const gchar *text);

/**
 * clawt_screen_gnome_input_argv:
 * @event: what to do to the screen
 *
 * The `gdbus` command that does it.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the
 *   argv, or %NULL for an event kind this backend has no method for
 */
GStrv clawt_screen_gnome_input_argv(ClawtInputEvent *event);

/**
 * clawt_screen_in_session_argv:
 * @argv: (array zero-terminated=1): a command to run inside a session
 *
 * Wraps @argv so it finds the logged-in user's session bus.
 *
 * An SSH connection arrives with no `DBUS_SESSION_BUS_ADDRESS`, and
 * every method here is on the session bus -- so a `gdbus` run straight
 * over ssh fails with "Cannot autolaunch D-Bus without X11", which reads
 * as the extension being absent rather than as the bus being unfindable.
 * The address is worked out inside the guest, where the uid is knowable,
 * for the same reason CLAWT_GUEST_DESKTOP_LAUNCHER does it there.
 *
 * Every argument is quoted on the way in, so a string somebody typed
 * into a takeover cannot become shell syntax.
 *
 * Returns: (transfer full) (array zero-terminated=1): `sh -c <script>`
 */
GStrv clawt_screen_in_session_argv(const gchar * const *argv);

/**
 * clawt_screen_parse_gdbus_frame:
 * @text: what `gdbus call` printed
 * @out: (out caller-allocates): where to put the answer
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the tuple `gdbus` prints for `ScreenshotFrame`.
 *
 * `gdbus` writes GVariant text -- `('/tmp/x.png', uint32 1280, uint32
 * 800, 'ab12...', int64 178...)` -- and the type annotations are the
 * reason this is a real parser rather than a scan for digits: the `32`
 * in `uint32` is a digit run, and reading it as the width produced a
 * frame reported as 32 pixels wide by a version of this that looked
 * plausible.
 *
 * Returns: %TRUE if @text was a frame tuple
 */
gboolean clawt_screen_parse_gdbus_frame(const gchar           *text,
                                        ClawtScreenFrameInfo  *out,
                                        GError               **error);

/**
 * clawt_screen_gnome_record_start_argv:
 * @max_seconds: the recording's own deadline
 * @max_events: the ring's capacity inside GNOME Shell
 *
 * The `gdbus` command that starts capturing input in a guest.
 *
 * A separate consent flag inside the extension governs whether this is
 * answered at all, and `SetEnabled` does not touch it -- an agent
 * allowed to click is not thereby allowed to watch somebody type. So a
 * refusal here is ordinary, and its text is the extension's own.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv clawt_screen_gnome_record_start_argv(guint max_seconds,
                                           guint max_events);

/**
 * clawt_screen_gnome_record_drain_argv:
 * @token: what the start answered with
 *
 * The `gdbus` command that takes what has been captured so far.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv clawt_screen_gnome_record_drain_argv(const gchar *token);

/**
 * clawt_screen_gnome_record_stop_argv:
 * @token: what the start answered with
 *
 * The `gdbus` command that ends the recording and takes the tail.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv clawt_screen_gnome_record_stop_argv(const gchar *token);

/**
 * clawt_screen_gnome_record_status_argv:
 *
 * The `gdbus` command that asks whether recording is permitted and
 * whether one is running.
 *
 * Worth asking before starting: it distinguishes "the consent flag is
 * off" from "the extension is not answering", and those send somebody
 * to entirely different places.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv clawt_screen_gnome_record_status_argv(void);

/**
 * clawt_screen_parse_gdbus_events:
 * @text: what `gdbus call` printed for a drain or a stop
 * @events_out: (out) (transfer full): the events, as a JSON array
 * @dropped_out: (out) (optional): how many the ring lost since the last
 *   drain
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the `(s, u)` tuple `DrainRecording` and `StopRecording` answer.
 *
 * A real parser rather than a scan, for the reason
 * clawt_screen_parse_gdbus_frame() gives: `gdbus` writes type
 * annotations, and the `32` in `uint32` is a digit run that a scan
 * reads as the value.
 *
 * Returns: %TRUE if @text was such a tuple
 */
gboolean clawt_screen_parse_gdbus_events(const gchar  *text,
                                         gchar       **events_out,
                                         guint        *dropped_out,
                                         GError      **error);

/**
 * clawt_screen_gowl_input_tool:
 * @event: what to do to the screen
 * @arguments: (out) (transfer full): the tool's arguments
 *
 * The gowl MCP tool that performs @event.
 *
 * Returns: (nullable): the tool name, or %NULL for an event kind gowl
 *   has no tool for
 */
const gchar *clawt_screen_gowl_input_tool(ClawtInputEvent  *event,
                                          JsonNode        **arguments);

/**
 * clawt_screen_png_size:
 * @bytes: (nullable): a PNG
 * @width: (out) (optional): its width
 * @height: (out) (optional): its height
 *
 * Reads a PNG's own IHDR rather than trusting whoever produced it.
 *
 * A frame is downscaled in the compositor, so a click at a point in the
 * picture is not a click at that point on the screen -- the ratio
 * between the two is what makes a coordinate usable, and half of that
 * ratio is the size of the file that actually got written. Taking it
 * from the request would describe what was asked for; taking it from
 * the image describes what arrived, and those differ every time the
 * screen is narrower than the ceiling.
 *
 * Returns: %TRUE if @bytes is a PNG with a readable header
 */
gboolean clawt_screen_png_size(GBytes *bytes, guint *width, guint *height);

/**
 * clawt_screen_hash_bytes:
 * @bytes: (nullable): some data
 *
 * The SHA-256 of @bytes, as lowercase hex.
 *
 * The same spelling the guest's own `ScreenshotFrame` uses, so a frame
 * that came from gowl -- which has no hash of its own -- can be compared
 * against the previous one by exactly the same rule as a frame that came
 * from a guest.  Two rules for "has this changed" would differ on the
 * backend nobody tested.
 *
 * Returns: (transfer full) (nullable): the hash
 */
gchar *clawt_screen_hash_bytes(GBytes *bytes);

G_END_DECLS
