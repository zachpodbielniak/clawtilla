/*
 * clawt-desktop-relay.h - stdio MCP, from the agent's CLI into its VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An AI CLI is given MCP servers by naming a command in `.mcp.json`, and
 * it speaks to that command over stdin and stdout.  The server the agent
 * wants is inside its own VM, and SSH already carries stdio -- so the
 * whole transport is `ssh ... clawtilla-desktop-mcp` and there is no
 * protocol to translate.
 *
 * There is still something to do in the middle, and it is the reason this
 * is not simply an entry naming ssh.  `computer.desktop.allow_input` and
 * `allow_spawn` are grants clawtilla makes, not grants the guest knows
 * about: gnome-desktop-mcp offers every tool it has to whoever connects.
 * A bare ssh relay would hand an observe-only agent the ability to type
 * and click, which is precisely the silent widening of a grant this
 * codebase refuses everywhere else.
 *
 * So the relay reads the JSON-RPC going past: it removes tools the agent
 * may not use from `tools/list`, and answers a call to one of them with an
 * error rather than forwarding it.  Everything else is copied through
 * untouched.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_desktop_relay_run:
 * @argv: (array zero-terminated=1): the command that reaches the guest's
 *   MCP server
 * @permitted: (array zero-terminated=1) (nullable): the tools this agent
 *   may use; %NULL permits none
 *
 * Runs the relay until either end closes, filtering as it goes.
 *
 * Returns: the exit status for the process
 */
gint clawt_desktop_relay_run(GStrv argv, GStrv permitted);

/**
 * clawt_desktop_relay_filter_outbound:
 * @line: one JSON-RPC message from the agent's MCP client
 * @permitted: (array zero-terminated=1) (nullable): the tools it may use
 * @refusal: (out) (transfer full) (nullable): a JSON-RPC error to answer
 *   with instead, when the call is refused and expects an answer
 *
 * Decides whether a message may be forwarded to the guest.
 *
 * Separated from the plumbing so the policy can be asserted on without a
 * VM, an SSH connection or a GNOME session -- which is otherwise the only
 * way to find out that a refused tool was being forwarded.
 *
 * Returns: %TRUE if @line should be sent on
 */
gboolean clawt_desktop_relay_filter_outbound(const gchar  *line,
                                             GStrv         permitted,
                                             gchar       **refusal);

/**
 * clawt_desktop_relay_filter_inbound:
 * @line: one JSON-RPC message from the guest's MCP server
 * @permitted: (array zero-terminated=1) (nullable): the tools the agent
 *   may use
 *
 * Removes tools the agent may not use from a `tools/list` result.
 *
 * A tool that is refused when called but still advertised is worse than
 * one that is absent: the agent plans around it, calls it, and has to work
 * out from an error that it never had it.
 *
 * Returns: (transfer full): the message to pass on, which is @line
 *   unchanged when there was nothing to remove
 */
gchar *clawt_desktop_relay_filter_inbound(const gchar *line,
                                          GStrv        permitted);

G_END_DECLS
