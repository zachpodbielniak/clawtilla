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
    "list_keybinds", "get_config", "ping",
    NULL
};

/*
 * The tools that act.  Separated because clicking is a different grant from
 * looking, and an observe-only agent is genuinely useful.
 */
static const gchar *const acting_tools[] = {
    "send_key", "key_press", "key_combo", "send_text", "type_text",
    "send_mouse", "mouse_click", "mouse_double_click", "mouse_down",
    "mouse_up", "mouse_move", "mouse_drag", "send_mouse_move",
    "send_scroll", "mouse_scroll",
    "focus_client", "focus_window", "close_client", "close_window",
    "move_client", "resize_client", "move_resize_window",
    "minimize_window", "unminimize_window", "maximize_window",
    "unmaximize_window", "move_client_to_tag", "set_client_tags",
    "view_tag", "activate_workspace", "spawn", "signal_client",
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
clawt_desktop_set_allow_input(ClawtDesktop *self, gboolean allow)
{
    g_return_if_fail(CLAWT_IS_DESKTOP(self));

    self->allow_input = allow;
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
     * gowl first: it is native, needs no Python and no shell extension, and
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

    if (!self->allow_input)
        return FALSE;

    for (i = 0; acting_tools[i] != NULL; i++) {
        if (g_strcmp0(tool_name, acting_tools[i]) == 0)
            return TRUE;
    }

    /*
     * An unrecognised tool is refused rather than passed through.  A newer
     * compositor may add one that injects input, and defaulting to allow
     * would quietly widen an observe-only grant the next time somebody
     * upgraded.
     */
    return FALSE;
}

gchar *
clawt_desktop_describe(ClawtDesktop *self)
{
    const gchar *backend_name;

    g_return_val_if_fail(CLAWT_IS_DESKTOP(self), NULL);

    backend_name = (self->resolved == CLAWT_DESKTOP_BACKEND_GNOME)
                   ? "the GNOME desktop" : "the gowl compositor";

    if (self->allow_input)
        return g_strdup_printf(
            "You can see and control %s: list windows, take screenshots, "
            "and send keystrokes and pointer events. Anything you click is "
            "clicked on the user's real screen.", backend_name);

    return g_strdup_printf(
        "You can observe %s: list windows and take screenshots. You cannot "
        "send keystrokes or pointer events.", backend_name);
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
}
