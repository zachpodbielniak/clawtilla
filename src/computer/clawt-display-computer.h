/*
 * clawt-display-computer.h - GUI apps on a screen of their own
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `computer.type: display` (since 0.51.0).  The agent's commands run on
 * the host, exactly as a `host` computer's do -- the same confinement,
 * the same environment allowlist -- with one difference: a private
 * virtual display is started with the computer, every command is told
 * where it is (DISPLAY or WAYLAND_DISPLAY), and the computer is
 * #ClawtObservable, so the Computer tab can watch it and a person can
 * take it.  Nobody is sitting at that screen, which is what makes it the
 * right place for an agent to click.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "computer/clawt-computer.h"
#include "computer/clawt-host-computer.h"
#include "computer/clawt-display-server.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_DISPLAY_COMPUTER (clawt_display_computer_get_type())

G_DECLARE_FINAL_TYPE(ClawtDisplayComputer, clawt_display_computer,
                     CLAWT, DISPLAY_COMPUTER, ClawtComputer)

/**
 * clawt_display_computer_new:
 * @agent_id: the agent it belongs to
 * @host: (transfer none): the host computer its commands run on,
 *   already confined as the agent's configuration says
 * @server: (transfer none): the display, not yet started
 *
 * Returns: (transfer full): the computer
 */
ClawtComputer *clawt_display_computer_new(const gchar        *agent_id,
                                          ClawtHostComputer  *host,
                                          ClawtDisplayServer *server);

/**
 * clawt_display_computer_set_environment:
 * @self: a #ClawtDisplayComputer
 * @env: (element-type utf8 utf8) (nullable): the agent's own `env:`
 *
 * What commands get beyond the allowlist, before the display's own
 * variables are added on top at start.
 */
void clawt_display_computer_set_environment(ClawtDisplayComputer *self,
                                            GHashTable           *env);

/**
 * clawt_display_computer_set_startup:
 * @self: a #ClawtDisplayComputer
 * @commands: (nullable) (array zero-terminated=1): apps started once the
 *   display is up (`computer.display.startup`)
 */
void clawt_display_computer_set_startup(ClawtDisplayComputer *self,
                                        const gchar * const  *commands);

/**
 * clawt_display_computer_get_server:
 * @self: a #ClawtDisplayComputer
 *
 * Returns: (transfer none): its display
 */
ClawtDisplayServer *clawt_display_computer_get_server(
    ClawtDisplayComputer *self);

/**
 * clawt_display_computer_get_host:
 * @self: a #ClawtDisplayComputer
 *
 * Returns: (transfer none): the host computer its commands run on
 */
ClawtHostComputer *clawt_display_computer_get_host(ClawtDisplayComputer *self);

/**
 * clawt_display_computer_launch:
 * @self: a #ClawtDisplayComputer
 * @command: a shell command line
 * @pid: (out) (optional): its process id, or 0 when the compositor does
 *   not say
 * @error: (out) (optional): return location for a #GError
 *
 * Starts an app on the display under the agent's confinement, without
 * waiting for it.  Starts the computer first when it is not running.
 *
 * Returns: %TRUE when it was started
 */
gboolean clawt_display_computer_launch(ClawtDisplayComputer *self,
                                       const gchar          *command,
                                       gint                 *pid,
                                       GError              **error);

/**
 * clawt_display_computer_action_is_acting:
 * @action: an action name, as clawt_display_computer_perform() takes it
 *
 * Whether @action changes the screen: refused while a person holds it,
 * and only allowed with computer.display.allow_input (or, for launch,
 * allow_spawn).  Looking -- screenshot, windows -- never is.
 *
 * Returns: %TRUE for click, move, type, key, scroll and launch
 */
gboolean clawt_display_computer_action_is_acting(const gchar *action);

/**
 * clawt_display_computer_perform:
 * @self: a #ClawtDisplayComputer
 * @action: screenshot, click, move, type, key, scroll, launch or windows
 * @arguments: (nullable): `x`, `y`, `button`, `dx`, `dy`, `text`,
 *   `command`, as the action takes them
 * @png: (out) (transfer full) (optional) (nullable): the picture, for
 *   screenshot
 * @error: (out) (optional): return location for a #GError
 *
 * Does one thing to the display, starting the computer first when it
 * starts on first use.  Blocks: run it off the main context.  The
 * grants and the takeover are the caller's to check -- the IPC verb and
 * the agent's tools both do, before calling this.
 *
 * Returns: (transfer full) (nullable): what happened, as an object node
 *   (`sent`; `started` and, when known, `pid`; `windows`; or the
 *   picture's `width` and `height`)
 */
JsonNode *clawt_display_computer_perform(ClawtDisplayComputer *self,
                                         const gchar          *action,
                                         JsonObject           *arguments,
                                         GBytes              **png,
                                         GError              **error);

/**
 * clawt_display_status_line:
 * @screen: (nullable): what `computer.screen` answered
 *
 * The sentence every client shows under a virtual display's screen
 * (since 0.51.0): which server is drawing it, whether it is up, why it
 * died if it did, and what `auto` skipped on the way.  One function, so
 * GTK, the web client and lrg say the same thing.
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL for a
 *   computer that is not a display
 */
gchar *clawt_display_status_line(JsonObject *screen);

/**
 * clawt_display_computer_describe_display:
 * @backend: the backend running
 * @width: pixels across
 * @height: pixels down
 * @viewer: (nullable): a VNC viewer address, for Xvnc
 *
 * The paragraph an agent reads about its screen.  A function of its
 * inputs alone, so it can be checked as text.
 *
 * Returns: (transfer full): the paragraph
 */
gchar *clawt_display_computer_describe_display(ClawtDisplayBackend backend,
                                               guint               width,
                                               guint               height,
                                               const gchar        *viewer);

G_END_DECLS
