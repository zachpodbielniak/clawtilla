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
 * clawt_desktop_set_guest_available:
 * @self: a #ClawtDesktop
 * @available: whether the agent has a VM with a desktop in it
 *
 * Tells the desktop that this agent's own VM is an option.
 *
 * Set by the factory, which is the only thing that knows what computer
 * the agent was given.  It is what makes `auto` resolve to the guest: an
 * agent with a machine of its own should be driving that screen rather
 * than the one somebody is sitting at.
 */
void clawt_desktop_set_guest_available(ClawtDesktop *self,
                                       gboolean      available);

/**
 * clawt_desktop_get_guest_available:
 * @self: a #ClawtDesktop
 *
 * Returns: %TRUE if the agent's own VM has a desktop
 */
gboolean clawt_desktop_get_guest_available(ClawtDesktop *self);

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
 * clawt_desktop_set_allow_recording:
 * @self: a #ClawtDesktop
 * @allow: whether the agent may record what a person does
 *
 * `computer.desktop.allow_recording`: the fourth grant, off by default.
 *
 * Its own grant rather than part of @allow_input, and the difference is
 * not a nicety.  Injecting a keystroke and capturing one are opposite
 * directions: the first does something to a screen, the second writes
 * down what the person at it typed -- into any window, including the
 * ones they had forgotten were open.  Folding it into the observing
 * tools would have made "may take a screenshot" silently mean "may
 * record my keystrokes", which is exactly the quiet widening this key
 * exists to prevent.
 *
 * Both compositors gate their own side too, behind a consent flag that
 * enabling automation does not set.  This is clawtilla's half of that,
 * enforced where every other desktop grant is: in the tool list the
 * relay is given.
 */
void clawt_desktop_set_allow_recording(ClawtDesktop *self, gboolean allow);

/**
 * clawt_desktop_get_allow_recording:
 * @self: a #ClawtDesktop
 *
 * Returns: %TRUE if this agent may record a demonstration
 */
gboolean clawt_desktop_get_allow_recording(ClawtDesktop *self);

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
 * The desktop tools this agent may use.
 *
 * The input-injecting ones are omitted when `allow_input` is off, the
 * spawning ones when `allow_spawn` is off, and the recording ones when
 * `allow_recording` is off -- three independent gates, because an
 * operator may want any one of them without the others.
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
 * clawt_desktop_tool_is_acting:
 * @tool_name: a tool name
 *
 * Whether @tool_name does something to the screen rather than only
 * looking at it.
 *
 * Asked by the relay so that a takeover can refuse the agent's *actions*
 * without refusing its screenshots -- an agent that can still see what
 * the person is doing can wait usefully; one that has been blinded as
 * well simply stops.
 *
 * A static question about the tool, not about this agent: what this
 * agent may do is clawt_desktop_tool_is_permitted(), which is a
 * different gate and stays where it is.
 *
 * Returns: %TRUE if it injects input
 */
gboolean clawt_desktop_tool_is_acting(const gchar *tool_name);

/**
 * clawt_desktop_tool_is_recording:
 * @tool_name: a tool name
 *
 * Whether @tool_name captures what a person is doing.
 *
 * A static question about the tool, like clawt_desktop_tool_is_acting()
 * and answered from the same table the permission check walks -- a
 * second list would be the copy that drifted, and it would be the one
 * deciding whether a keylogger counts as one.
 *
 * `screenshot_frame` is **not** one of these. Taking a picture of a
 * screen and transcribing what was typed into it are different acts,
 * and a live preview must not need the recording grant.
 *
 * Returns: %TRUE if it records input
 */
gboolean clawt_desktop_tool_is_recording(const gchar *tool_name);

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
