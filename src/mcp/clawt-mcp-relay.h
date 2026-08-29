/*
 * clawt-mcp-relay.h - stdio MCP, with a tool filter in the middle
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An AI CLI is given MCP servers by naming a command in `.mcp.json`, and
 * it speaks to that command over stdin and stdout.  That leaves room for
 * something in the middle, and clawtilla has two reasons to want one.
 *
 * The first is that a grant clawtilla makes is not a grant the server
 * knows about.  gnome-desktop-mcp offers every tool it has to whoever
 * connects; so does most of what is on the other end of one of these.  A
 * bare pass-through would hand an observe-only agent the ability to type
 * and click, which is precisely the silent widening of a grant this
 * codebase refuses everywhere else.  So the relay reads the JSON-RPC
 * going past: it removes tools the agent may not use from `tools/list`,
 * and answers a call to one of them with an error rather than forwarding
 * it.
 *
 * The second is credentials.  A connector's token belongs to the server
 * process and nowhere else -- not in the agent's `.mcp.json`, not in its
 * environment, not in the process table.  Handing it to the child here
 * is the only place it exists outside the file clawtilla wrote it to.
 *
 * Everything else is copied through untouched.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_mcp_relay_run:
 * @argv: (array zero-terminated=1): the command that reaches the guest's
 *   MCP server
 * @envp: (array zero-terminated=1) (nullable): `NAME=value` pairs added
 *   to the server's environment, which is where a brokered credential goes
 * @permitted: (array zero-terminated=1) (nullable): the tools this agent
 *   may use; %NULL permits none
 *
 * Runs the relay until either end closes, filtering as it goes.
 *
 * Returns: the exit status for the process
 */
gint clawt_mcp_relay_run(GStrv argv, GStrv envp, GStrv permitted,
                         const gchar *hint);

/**
 * ClawtMcpRelayGate: (skip)
 * @tool: the tool about to be called
 * @refusal: (out) (transfer full) (nullable): what to tell the agent
 *   instead, when the answer is no
 * @user_data: whatever was handed to clawt_mcp_relay_run_gated()
 *
 * A second opinion, asked per call, on a tool the agent is otherwise
 * allowed.
 *
 * The permitted list answers "may this agent ever do this", which is
 * fixed for the life of the relay. This answers "may it do it right
 * now", which is not: somebody can take the screen while the relay is
 * running, and the relay has no way to hear about it.
 *
 * **It must fail open.** A gate that cannot reach whoever knows the
 * answer has to allow the call. This is cooperation between an operator
 * and their own agent, not a boundary against a hostile one -- nothing
 * here confines a program or protects a secret -- and a daemon hiccup
 * that bricked every desktop tool mid-turn would cost far more than the
 * race it prevented. Every real permission gate in clawtilla fails
 * closed; this is not one of them, and the difference is the point.
 *
 * Returns: %TRUE if the call may go through
 */
typedef gboolean (*ClawtMcpRelayGate)(const gchar  *tool,
                                      gchar       **refusal,
                                      gpointer      user_data);

/**
 * clawt_mcp_relay_run_gated:
 * @argv: (array zero-terminated=1): the server to run
 * @envp: (array zero-terminated=1) (nullable): added to its environment
 * @permitted: (array zero-terminated=1) (nullable): the tools this agent
 *   may use
 * @hint: (nullable): one sentence on why, for a refusal from @permitted
 * @gate: (scope call) (nullable): asked once per permitted tool call
 * @gate_data: data for @gate
 *
 * The relay, with a per-call gate in front of it.
 *
 * Returns: the exit status for the process
 */
gint clawt_mcp_relay_run_gated(GStrv              argv,
                               GStrv              envp,
                               GStrv              permitted,
                               const gchar       *hint,
                               ClawtMcpRelayGate  gate,
                               gpointer           gate_data);

/**
 * clawt_mcp_relay_call_name:
 * @line: one JSON-RPC message from the agent's MCP client
 *
 * The tool a `tools/call` message names, or %NULL for anything else.
 *
 * Exposed because a gate has to know what it is being asked about, and
 * a second JSON parse in the caller would be a second answer to "is
 * this a tool call" -- which would differ on a notification, the shape
 * that has no id and therefore no way to be refused.
 *
 * Returns: (transfer full) (nullable): the tool name
 */
gchar *clawt_mcp_relay_call_name(const gchar *line);

/**
 * clawt_mcp_relay_build_refusal:
 * @line: the `tools/call` message being refused
 * @message: what to tell the agent
 *
 * A JSON-RPC error carrying the caller's own id.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL when @line is
 *   a notification and there is nothing to answer into
 */
gchar *clawt_mcp_relay_build_refusal(const gchar *line,
                                     const gchar *message);

/**
 * clawt_mcp_relay_run_unfiltered:
 * @argv: (array zero-terminated=1): the server to run
 * @envp: (array zero-terminated=1) (nullable): `NAME=value` pairs added to
 *   its environment
 *
 * Runs the relay passing every message through untouched.
 *
 * Wanted when the relay is there for the credential rather than for the
 * grant -- a connector with no tool list restricts nothing, and it needs
 * saying out loud: clawt_mcp_relay_run() with a %NULL @permitted allows
 * *no* tool, which is the right default for a grant and precisely the
 * wrong one here.  An agent handed an empty tool list looks exactly like
 * an agent whose server failed to start.
 *
 * Returns: the exit status for the process
 */
gint clawt_mcp_relay_run_unfiltered(GStrv argv, GStrv envp);

/**
 * clawt_mcp_relay_filter_outbound:
 * @line: one JSON-RPC message from the agent's MCP client
 * @permitted: (array zero-terminated=1) (nullable): the tools it may use
 * @hint: (nullable): one sentence on why, in the words of whoever imposed
 *   the restriction
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
gboolean clawt_mcp_relay_filter_outbound(const gchar  *line,
                                         GStrv         permitted,
                                         const gchar  *hint,
                                         gchar       **refusal);

/**
 * clawt_mcp_relay_filter_inbound:
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
gchar *clawt_mcp_relay_filter_inbound(const gchar *line,
                                          GStrv        permitted);

G_END_DECLS
