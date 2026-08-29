/*
 * clawt-desktop.c - Letting an agent see and drive a desktop
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-desktop.h"

#include <gio/gunixsocketaddress.h>

struct _ClawtDesktop {
    GObject parent_instance;

    ClawtDesktopBackend  backend;
    ClawtDesktopBackend  resolved;
    gchar               *socket_path;
    gboolean             allow_input;
    gboolean             allow_spawn;
    gboolean             allow_recording;

    /*
     * Whether this agent has a VM with a desktop in it.  Set by the
     * factory, which is the only thing that knows what computer the agent
     * was given.
     */
    gboolean             guest_available;
};

G_DEFINE_FINAL_TYPE(ClawtDesktop, clawt_desktop, G_TYPE_OBJECT)

/*
 * The tools that only look.
 *
 * gowl and gnome-desktop-mcp name most of these the same way, and where
 * they differ both spellings are listed -- an agent should not have to know
 * which compositor it is talking to.
 */
static const gchar *const observing_tools[] = {
    "describe_desktop", "list_clients", "list_windows", "list_monitors",
    "get_monitors", "get_tag_state", "get_focused_client", "get_window",
    "find_window", "list_workspaces", "screenshot", "screenshot_client",
    "screenshot_window", "screenshot_monitor", "screenshot_region",
    "screenshot_area", "pick_color", "get_client_process_info",

    /*
     * The pollable frame, which is an *observing* tool and has to be
     * named here to be one.
     *
     * gnome-desktop-mcp grew `screenshot_frame` alongside the recorder,
     * and the two arrived together -- which is exactly why they must not
     * be in the same category. Taking a picture of a screen and
     * transcribing what was typed into it are different acts, and a
     * frame left out of this list would either be refused to an agent
     * that may already screenshot, or end up behind the recording grant
     * because that is where it was added.
     */
    "screenshot_frame",

    "list_keybinds", "get_config", "ping",

    /*
     * Housekeeping and status, added after watching a real
     * gnome-desktop-mcp answer tools/list: it offers cleanup_screenshots
     * (which deletes temporary files the server itself wrote) and the
     * pair that reads and flips its own automation switch.
     *
     * set_enabled is here on purpose.  It grants nothing -- an agent
     * cannot use it to reach an input tool, because that filtering
     * happens on this side -- and it is the one thing that lets an agent
     * rescue itself when the guest's own autostart lost the race to
     * switch automation on.
     */
    "cleanup_screenshots", "get_enabled", "set_enabled",
    NULL
};

/*
 * The tools that act.  Separated because clicking is a different grant from
 * looking, and an observe-only agent is genuinely useful.
 */
/*
 * Launching a process is not "acting on the desktop" like clicking is.
 *
 * spawn goes straight to the compositor over its own socket, so it never
 * passes ClawtSandbox: no path check, no sudo block, and under
 * confine: bwrap it steps outside the sandbox entirely.  An operator who
 * chose the only mode that genuinely constrains a program would not
 * expect enabling desktop input to hand back unrestricted process
 * launching, so it needs saying yes to separately.
 */
static const gchar *const spawning_tools[] = {
    "spawn", "signal_client", NULL
};

/*
 * The tools that watch a person.
 *
 * A fourth category rather than more entries in observing_tools[], and
 * the distinction is the whole reason `computer.desktop.allow_recording`
 * exists: folding these in would make "may take a screenshot" silently
 * mean "may record my keystrokes". A screenshot is a picture of what is
 * on a screen; a recording is a transcript of what somebody typed into
 * it, into any window, including the ones they did not know were being
 * watched.
 *
 * `screenshot_frame` is emphatically *not* here -- it is an observing
 * tool, and putting it here would make watching a screen need the
 * keylogger grant.
 *
 * Both spellings are listed because gowl and gnome-desktop-mcp named
 * these differently, and an agent should not have to know which
 * compositor it is talking to. Every name here is one of theirs: gowl's
 * start_recording/drain_recording/stop_recording/recording_status, and
 * gnome-desktop-mcp's start_input_recording/drain_input_recording/
 * stop_input_recording/get_recording_status.
 */
static const gchar *const recording_tools[] = {
    "start_recording", "drain_recording", "stop_recording",
    "recording_status",
    "start_input_recording", "drain_input_recording",
    "stop_input_recording", "get_recording_status",
    NULL
};

static const gchar *const acting_tools[] = {
    "send_key", "key_press", "key_combo", "send_text", "type_text",
    "send_mouse", "mouse_click", "mouse_double_click", "mouse_down",
    "mouse_up", "mouse_move", "mouse_drag", "send_mouse_move",
    "send_scroll", "mouse_scroll",
    "focus_client", "focus_window", "close_client", "close_window",
    "move_client", "resize_client", "move_resize_window",
    "minimize_window", "unminimize_window", "maximize_window",
    "unmaximize_window", "move_client_to_tag", "set_client_tags",
    "view_tag", "activate_workspace",
    NULL
};

ClawtDesktop *
clawt_desktop_new(ClawtDesktopBackend backend, const gchar *socket_path)
{
    ClawtDesktop *self = g_object_new(CLAWT_TYPE_DESKTOP, NULL);

    self->backend = backend;
    self->resolved = backend;

    self->socket_path = (socket_path != NULL)
                        ? clawt_expand_path(socket_path)
                        : g_build_filename(g_get_user_runtime_dir(),
                                           "gowl-mcp.sock", NULL);

    return self;
}

void
clawt_desktop_set_guest_available(ClawtDesktop *self, gboolean available)
{
    g_return_if_fail(CLAWT_IS_DESKTOP(self));

    self->guest_available = available;
}

gboolean
clawt_desktop_get_guest_available(ClawtDesktop *self)
{
    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), FALSE);

    return self->guest_available;
}

void
clawt_desktop_set_allow_input(ClawtDesktop *self, gboolean allow)
{
    g_return_if_fail(CLAWT_IS_DESKTOP(self));

    self->allow_input = allow;
}

void
clawt_desktop_set_allow_spawn(ClawtDesktop *self, gboolean allow)
{
    g_return_if_fail(CLAWT_IS_DESKTOP(self));

    self->allow_spawn = allow;
}

void
clawt_desktop_set_allow_recording(ClawtDesktop *self, gboolean allow)
{
    g_return_if_fail(CLAWT_IS_DESKTOP(self));

    self->allow_recording = allow;
}

gboolean
clawt_desktop_get_allow_recording(ClawtDesktop *self)
{
    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), FALSE);

    return self->allow_recording;
}

static gboolean
gowl_socket_answers(ClawtDesktop *self)
{
    g_autoptr(GSocketClient) client = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketConnection) connection = NULL;

    if (self->socket_path == NULL ||
        !g_file_test(self->socket_path, G_FILE_TEST_EXISTS))
        return FALSE;

    /*
     * Connecting rather than only checking the file exists.  A socket left
     * behind by a compositor that has since exited looks identical to a
     * live one on disk, and an agent told it has a desktop it cannot reach
     * wastes turns discovering otherwise.
     */
    address = g_unix_socket_address_new(self->socket_path);
    client = g_socket_client_new();
    connection = g_socket_client_connect(client,
                                         G_SOCKET_CONNECTABLE(address),
                                         NULL, NULL);

    if (connection == NULL)
        return FALSE;

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    return TRUE;
}

static gboolean
gnome_server_is_installed(void)
{
    g_autofree gchar *path = g_find_program_in_path("gnome-desktop-mcp");

    return path != NULL;
}

ClawtDesktopBackend
clawt_desktop_resolve_backend(ClawtDesktop *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), CLAWT_DESKTOP_BACKEND_AUTO);

    if (self->backend != CLAWT_DESKTOP_BACKEND_AUTO) {
        self->resolved = self->backend;
        return self->resolved;
    }

    /*
     * The agent's own VM first, when it has one.
     *
     * Not a preference between equals: the other three backends drive the
     * screen a person is sitting at, and this one drives a machine that
     * exists to be driven.  An agent that misjudges a click in its own VM
     * has broken its own VM.
     */
    if (self->guest_available) {
        self->resolved = CLAWT_DESKTOP_BACKEND_GUEST;
        return self->resolved;
    }

    /*
     * gowl next: it is native, needs no Python and no shell extension, and
     * speaks the richer vocabulary.  GNOME is the fallback for sessions not
     * running it.
     */
    if (gowl_socket_answers(self)) {
        self->resolved = CLAWT_DESKTOP_BACKEND_GOWL;
        return self->resolved;
    }

    if (gnome_server_is_installed()) {
        self->resolved = CLAWT_DESKTOP_BACKEND_GNOME;
        return self->resolved;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "no desktop control is available: gowl's MCP socket is not "
                "answering at %s (is gowl built with MCP=1 and modules.mcp "
                "enabled?) and gnome-desktop-mcp is not installed",
                self->socket_path);

    self->resolved = CLAWT_DESKTOP_BACKEND_AUTO;

    return self->resolved;
}

gboolean
clawt_desktop_is_available(ClawtDesktop *self, GError **error)
{
    ClawtDesktopBackend backend;

    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), FALSE);

    backend = clawt_desktop_resolve_backend(self, error);

    switch (backend) {
    case CLAWT_DESKTOP_BACKEND_GOWL:
        if (gowl_socket_answers(self))
            return TRUE;

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "gowl's MCP socket at %s is not answering; check that "
                    "gowl was built with MCP=1 and has modules.mcp enabled",
                    self->socket_path);
        return FALSE;

    case CLAWT_DESKTOP_BACKEND_GNOME:
        if (gnome_server_is_installed())
            return TRUE;

        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "gnome-desktop-mcp is not installed");
        return FALSE;

    case CLAWT_DESKTOP_BACKEND_GUEST:
        /*
         * Answered from the configuration rather than by dialling the
         * guest.  Whether the VM is up, has finished installing GNOME and
         * has anybody logged in is the VM's own question, asked where the
         * VM is -- and this one is called while a client waits.
         */
        if (self->guest_available)
            return TRUE;

        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this agent has no VM to put a desktop in; "
                            "computer.desktop.backend is guest but "
                            "computer.type is not vm");
        return FALSE;

    case CLAWT_DESKTOP_BACKEND_AUTO:
    default:
        return FALSE;
    }
}

GStrv
clawt_desktop_get_tool_names(ClawtDesktop *self)
{
    GPtrArray *out;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), NULL);

    out = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; observing_tools[i] != NULL; i++)
        g_ptr_array_add(out, g_strdup(observing_tools[i]));

    if (self->allow_input) {
        for (i = 0; acting_tools[i] != NULL; i++)
            g_ptr_array_add(out, g_strdup(acting_tools[i]));
    }

    /*
     * Independent of allow_input, not nested inside it.  Watching a
     * demonstration and clicking are two different grants and either
     * can be given without the other -- an operator who wants to teach
     * a task by doing it has no reason to also let the agent move their
     * pointer.
     */
    if (self->allow_recording) {
        for (i = 0; recording_tools[i] != NULL; i++)
            g_ptr_array_add(out, g_strdup(recording_tools[i]));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

gboolean
clawt_desktop_tool_is_permitted(ClawtDesktop *self, const gchar *tool_name)
{
    gsize i;

    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), FALSE);
    g_return_val_if_fail(tool_name != NULL, FALSE);

    for (i = 0; observing_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, observing_tools[i]) == 0)
            return TRUE;
    }

    /*
     * Checked **before** the allow_input gate below, not after it.
     *
     * Recording does not depend on being allowed to click, and an
     * early return on !allow_input would make the recording grant
     * unreachable for exactly the configuration it is most likely to
     * be used in: somebody who wants to demonstrate a task without
     * also handing their pointer over.
     */
    for (i = 0; recording_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, recording_tools[i]) == 0)
            return self->allow_recording;
    }

    if (!self->allow_input)
        return FALSE;

    for (i = 0; acting_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, acting_tools[i]) == 0)
            return TRUE;
    }

    /*
     * Launching or signalling a process needs its own grant, because it
     * is not confined by anything else clawtilla has.
     */
    for (i = 0; spawning_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, spawning_tools[i]) == 0)
            return self->allow_spawn;
    }

    /*
     * An unrecognised tool is refused rather than passed through.  A newer
     * compositor may add one that injects input, and defaulting to allow
     * would quietly widen an observe-only grant the next time somebody
     * upgraded.
     */
    return FALSE;
}

gboolean
clawt_desktop_tool_is_acting(const gchar *tool_name)
{
    gsize i;

    if (tool_name == NULL)
        return FALSE;

    /*
     * Walked from the same table clawt_desktop_tool_is_permitted() uses.
     * A second list here would be a copy, and the copy that drifted
     * would be the one deciding whether a takeover actually stops the
     * agent -- which is only discovered when somebody's pointer jumps
     * while they are using it.
     */
    for (i = 0; acting_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, acting_tools[i]) == 0)
            return TRUE;
    }

    /*
     * Spawning counts too. Launching an application puts a window on the
     * screen somebody has taken, which is exactly as disruptive as a
     * click and rather harder to undo.
     */
    for (i = 0; spawning_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, spawning_tools[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

gboolean
clawt_desktop_tool_is_recording(const gchar *tool_name)
{
    gsize i;

    if (tool_name == NULL)
        return FALSE;

    /*
     * Walked from the same table clawt_desktop_tool_is_permitted()
     * uses, for the reason clawt_desktop_tool_is_acting() gives: a
     * second list here would be the copy that drifted, and it would be
     * the one deciding whether a keylogger counts as a keylogger.
     */
    for (i = 0; recording_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, recording_tools[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

gchar *
clawt_desktop_describe(ClawtDesktop *self)
{
    const gchar *backend_name;
    const gchar *recording = "";

    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), NULL);

    /*
     * Resolved first, and it has to be.
     *
     * `resolved` is only set by clawt_desktop_resolve_backend(), and
     * nothing on the description path had called it -- so a desktop built
     * from a config saying `auto` still held AUTO here and fell through
     * to the gowl wording. A guest agent was told, in its own prompt,
     * that it was driving the gowl compositor and that "anything you
     * click is clicked on the user's real screen".
     *
     * Exactly backwards, and backwards in the dangerous direction: an
     * agent that believes it is on somebody's real screen when it is in
     * its own VM is merely timid, but this was the reverse of that.
     */
    clawt_desktop_resolve_backend(self, NULL);

    /*
     * The guest desktop is described separately, and differently on
     * purpose.  Telling an agent that its clicks land on the user's real
     * screen when they land in its own VM makes it needlessly cautious;
     * telling it the reverse is far worse.
     */
    if (self->resolved == CLAWT_DESKTOP_BACKEND_GUEST) {
        /*
         * Where to look first when those tools do not work, said in the
         * description rather than left to be discovered.
         *
         * The tools are a GNOME Shell extension inside the guest, and
         * everything that could stop it loading happened at first boot
         * in a log nobody reads.  What an agent sees is "DBus object
         * has no attribute", which names nothing -- so two of them each
         * spent a long turn reading dconf, listing extensions and
         * introspecting the bus to arrive at a fact the guest had
         * written down.  One sentence here is cheaper than that turn,
         * every time.
         */
        const gchar *diagnose =
            " If those tools fail with a D-Bus error, do not investigate "
            "the bus: read " CLAWT_GUEST_DESKTOP_STATUS_FILE " in the VM. "
            "The guest records there whether its half installed, and a "
            "failure in that file is the answer. Re-run "
            "`" CLAWT_GUEST_DESKTOP_INSTALL_SCRIPT "` to try again -- and "
            "tell the user, because GNOME Shell only picks up a newly "
            "installed extension when the session restarts.";

        /*
         * How to start an application, because the obvious way is wrong
         * and succeeds.
         *
         * `computer_exec` is an SSH connection with no session
         * environment, so an agent asked to open a browser arrives at
         * `DISPLAY=:0 firefox` by itself. A window appears and the
         * application is now on Xwayland rather than in the session --
         * a different compositing and input path, and nothing it can
         * query says which one it got.
         */
        const gchar *launching =
            " To start an application in that desktop, run "
            "`" CLAWT_GUEST_DESKTOP_RUN_SCRIPT " <command>` through "
            "clawtilla_computer_exec. Do not set DISPLAY yourself: a "
            "plain shell here has no session, so `DISPLAY=:0 <app>` "
            "appears to work and quietly puts the application on "
            "Xwayland instead of in the session. It returns as soon as "
            "the application has started, so confirm with list_windows "
            "rather than by waiting.";

        /*
         * And the one an agent cannot possibly work out from its tools.
         *
         * `focused` is the window manager's idea of focus. GNOME's
         * Activities overview takes the *keyboard* without changing it,
         * so a window reads focused: true while every keystroke goes to
         * the overview's search box. An agent typed a URL into it for a
         * good part of a session and had no way to see that, because
         * the one field that would have told it says the opposite.
         */
        const gchar *keyboard =
            " One trap: `focused: true` is the window manager's focus "
            "and does not mean your keystrokes arrive there. If GNOME's "
            "Activities overview is open -- which it is at login, before "
            "anything has a window -- it holds the keyboard while the "
            "window still reports focused. Send Escape before typing "
            "into a window you have just focused, and check a "
            "screenshot if text is not appearing where you expect.";

        /*
         * Screenshots are a file the agent can open, which it was not
         * told and had no way to guess.
         *
         * They land in the workspace share now, so an agent's own `read`
         * reaches them -- but the tool returns the path inside the
         * guest, so an agent that takes the return value at face value
         * still cannot open it.
         */
        const gchar *looking =
            " Screenshots are written into your workspace share, so read "
            "one with your own tools and look at it. The tool returns "
            "the path inside the VM; the same file is under your "
            "workspace on the host, which is the one `read` opens.";

        /*
         * And how to turn a picture into a coordinate.
         *
         * An agent given a screen and a click tool estimates positions
         * from an assumed layout, misses by a few dozen pixels, and
         * concludes the pointer tools are unreliable. One worked out the
         * OCR route by trial and error over a session and reported it as
         * a discovery -- which is the signal that it belonged here.
         */
        const gchar *clicking =
            " Before clicking, get the position rather than estimating "
            "it: `tesseract <file> - tsv` in the VM prints a bounding "
            "box per word, and the centre of the box for the text you "
            "want is the coordinate. Guessing from an assumed layout "
            "misses controls by a few dozen pixels and looks like the "
            "pointer being wrong.";

        if (self->allow_input)
            return g_strconcat(
                "There is a GNOME desktop inside your own VM, logged in and "
                "waiting. You can list its windows, take screenshots of it, "
                "and send it keystrokes and pointer events. It is yours: "
                "nothing you do there touches the user's screen. The tools "
                "arrive from the `clawtilla-desktop` MCP server.",
                launching, looking, clicking, keyboard, diagnose, NULL);

        /*
         * An observe-only agent gets the launcher and not the keyboard
         * paragraph: it cannot send Escape, and advice it cannot act on
         * is noise in a prompt. A screenshot shows it the overview.
         */
        return g_strconcat(
            "There is a GNOME desktop inside your own VM. You can list its "
            "windows and take screenshots of it, but you cannot send "
            "keystrokes or pointer events. The tools arrive from the "
            "`clawtilla-desktop` MCP server.", launching, looking,
            diagnose, NULL);
    }

    backend_name = (self->resolved == CLAWT_DESKTOP_BACKEND_GNOME)
                   ? "the GNOME desktop" : "the gowl compositor";

    /*
     * Said out loud when it is on.
     *
     * A tool an agent has and does not know about is a tool it will not
     * use -- but that is the smaller half. The larger half is that an
     * agent which can record somebody's keyboard should be told, in the
     * same breath, that what it captures is theirs and not to be
     * repeated back. An instruction the agent cannot see is one it will
     * violate.
     */
    if (self->allow_recording)
        recording =
            " You may also record a demonstration on that screen, with "
            "start_recording and stop_recording. That captures every key "
            "the person presses, in any window -- so treat a recording as "
            "the user's private material: do not quote it back, do not "
            "summarise what was typed, and do not copy anything from it "
            "that looks like a password or a token.";

    if (self->allow_input)
        return g_strdup_printf(
            "You can see and control %s: list windows, take screenshots, "
            "and send keystrokes and pointer events. Anything you click is "
            "clicked on the user's real screen.%s", backend_name, recording);

    return g_strdup_printf(
        "You can observe %s: list windows and take screenshots. You cannot "
        "send keystrokes or pointer events.%s", backend_name, recording);
}

const gchar *
clawt_desktop_get_socket_path(ClawtDesktop *self)
{
    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), NULL);

    return self->socket_path;
}

static void
clawt_desktop_finalize(GObject *object)
{
    ClawtDesktop *self = CLAWT_DESKTOP(object);

    g_clear_pointer(&self->socket_path, g_free);

    G_OBJECT_CLASS(clawt_desktop_parent_class)->finalize(object);
}

static void
clawt_desktop_class_init(ClawtDesktopClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_desktop_finalize;
}

static void
clawt_desktop_init(ClawtDesktop *self)
{
    self->backend = CLAWT_DESKTOP_BACKEND_AUTO;
    self->resolved = CLAWT_DESKTOP_BACKEND_AUTO;
    self->allow_input = FALSE;
    self->allow_recording = FALSE;
}
