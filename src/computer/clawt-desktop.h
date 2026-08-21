/*
 * clawt-desktop.h - Letting an agent see and drive a desktop
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An add-on rather than a computer type, so it works alongside whichever
 * one the agent has.
 *
 * Two backends, both speaking MCP:
 *
 *   gowl exposes an MCP server on a unix socket.  Native, no Python, and
 *   the same tool vocabulary clawtilla already speaks -- so clawtilla
 *   connects to the socket directly rather than through gowl's stdio relay,
 *   which exists for clients that can only speak stdio.
 *
 *   gnome-desktop-mcp is a stdio server plus a GNOME Shell extension, for
 *   sessions not running gowl.
 *
 * An agent with this can read anything on the screen and click anything on
 * it.  Observing and acting are separate grants for that reason.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_DESKTOP (clawt_desktop_get_type())

G_DECLARE_FINAL_TYPE(ClawtDesktop, clawt_desktop, CLAWT, DESKTOP, GObject)

/**
 * clawt_desktop_new:
 * @backend: which implementation, or auto to probe
 * @socket_path: (nullable): gowl's MCP socket
 *
 * Returns: (transfer full): a new #ClawtDesktop
 */
ClawtDesktop *clawt_desktop_new(ClawtDesktopBackend  backend,
                                const gchar         *socket_path);

/**
 * clawt_desktop_set_allow_input:
 * @self: a #ClawtDesktop
 * @allow: whether key and pointer injection is permitted
 *
 * With this off the agent can look but not touch, which is a genuinely
 * useful amount of access and a much smaller grant.
 */
void clawt_desktop_set_allow_input(ClawtDesktop *self, gboolean allow);

/**
 * clawt_desktop_set_allow_spawn:
 * @self: a #ClawtDesktop
 * @allow: whether the agent may launch and signal processes
 *
 * Off by default, and separate from @allow_input.
 *
 * The compositor's spawn tool starts a process through the compositor's
 * own socket, so nothing clawtilla has constrains it: not the path
 * checks, not the sudo block, and not bwrap.  An operator who chose
 * bwrap -- the one mode that genuinely confines a running program --
 * would not expect turning on desktop input to hand back unrestricted
 * process launching.
 */
void clawt_desktop_set_allow_spawn(ClawtDesktop *self, gboolean allow);

/**
 * clawt_desktop_resolve_backend:
 * @self: a #ClawtDesktop
 * @error: (out) (optional): return location for a #GError
 *
 * Works out which backend to use, probing when set to auto.
 *
 * Returns: the resolved backend, or the configured one if it could not be
 *   reached
 */
ClawtDesktopBackend clawt_desktop_resolve_backend(ClawtDesktop  *self,
                                                  GError       **error);

/**
 * clawt_desktop_is_available:
 * @self: a #ClawtDesktop
 * @error: (out) (optional): return location for a #GError
 *
 * Whether the desktop can actually be reached right now.
 *
 * Returns: %TRUE if it is usable
 */
gboolean clawt_desktop_is_available(ClawtDesktop  *self,
                                    GError       **error);

/**
 * clawt_desktop_get_tool_names:
 * @self: a #ClawtDesktop
 *
 * The desktop tools this agent may use, with the input-injecting ones
 * omitted when allow_input is off.
 *
 * Returns: (transfer full) (array zero-terminated=1): the tool names
 */
GStrv clawt_desktop_get_tool_names(ClawtDesktop *self);

/**
 * clawt_desktop_tool_is_permitted:
 * @self: a #ClawtDesktop
 * @tool_name: a tool name
 *
 * Returns: %TRUE if the agent may call it
 */
gboolean clawt_desktop_tool_is_permitted(ClawtDesktop *self,
                                         const gchar  *tool_name);

/**
 * clawt_desktop_describe:
 * @self: a #ClawtDesktop
 *
 * A description for the agent's prompt.
 *
 * Returns: (transfer full): the description
 */
gchar *clawt_desktop_describe(ClawtDesktop *self);

/**
 * clawt_desktop_get_socket_path:
 * @self: a #ClawtDesktop
 *
 * Returns: (transfer none) (nullable): the socket gowl is expected on
 */
const gchar *clawt_desktop_get_socket_path(ClawtDesktop *self);

G_END_DECLS
