/*
 * clawt-agent-confine.h - Putting an agent's own CLI in a sandbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * computer.host.confine decides what clawtilla_computer_exec may touch.
 * The agent's own Bash, Read and Write are the AI CLI's, which libreclaw
 * spawns, and they never went near that check.  `runtime.confine: bwrap`
 * wraps the libreclaw child itself -- and with it the CLI, every command
 * the CLI runs and every MCP server it starts -- in one bubblewrap
 * sandbox, built by the same #ClawtSandbox the exec path uses so the two
 * cannot disagree about what a bind or a denial means.
 *
 * What goes in is decided here, from the agent's configuration, so a test
 * can read the whole box as data: the argv clawt_sandbox_wrap_argv()
 * builds and the environment clawt_sandbox_wrap_environment() builds.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "computer/clawt-sandbox.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * clawt_agent_confine_wanted:
 * @config: an agent's configuration
 *
 * Whether @config asks for its own process to be confined, after
 * `defaults.confine` has had its say.
 *
 * Returns: %TRUE for `runtime.confine: bwrap`
 */
gboolean clawt_agent_confine_wanted(ClawtAgentConfig *config);

/**
 * clawt_agent_confine_available:
 *
 * Whether bubblewrap is on this machine's PATH -- the daemon's, since the
 * daemon is what runs it.
 *
 * Returns: %TRUE if `bwrap` can be found
 */
gboolean clawt_agent_confine_available(void);

/**
 * clawt_agent_confine_check:
 * @config: an agent's configuration that asks for confinement
 * @error: (out) (optional): return location for a #GError
 *
 * Whether the confinement @config asks for can be given.
 *
 * Two refusals, both the never-downgrade rule: an embedded runtime is the
 * daemon and has no child to put in a sandbox, and a missing `bwrap`
 * cannot build one.  Either way the agent is a shadow with this message,
 * never an agent that runs unconfined.  An agent that does not ask for
 * confinement always passes.
 *
 * Returns: %TRUE if the agent can be started as configured
 */
gboolean clawt_agent_confine_check(ClawtAgentConfig  *config,
                                   GError           **error);

/**
 * clawt_agent_confine_default_login_paths:
 * @provider: (nullable): the agent's `model.provider`
 *
 * Where @provider's CLI keeps its login when no `accounts:` entry points
 * it elsewhere, which is what the sandbox binds read-write so the CLI can
 * refresh its OAuth tokens.
 *
 * A table rather than a question to the CLI, because none of them can be
 * asked: ai-glib knows the variable that *moves* a login and not where it
 * is by default.  Paths are absolute, under the daemon's own home and XDG
 * directories.
 *
 * Returns: (transfer full) (array zero-terminated=1): the paths, possibly
 *   none
 */
GStrv clawt_agent_confine_default_login_paths(const gchar *provider);

/**
 * clawt_agent_confine_build:
 * @config: an agent's configuration
 * @state_dir: the agent's state directory, `<state_dir>/agents/<id>`
 * @mounts: (nullable) (element-type ClawtMount): what the agent's
 *   computer mounts, from clawt_computer_get_mounts(); %NULL reads the
 *   agent's own `mounts:` instead
 * @sockets: (nullable) (array zero-terminated=1): the daemon sockets the
 *   agent dials -- its link and the IPC socket its clawtilla-mcp-server
 *   uses
 * @libreclaw: (nullable): the libreclaw binary that will run inside
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the sandbox an agent's own process runs in.
 *
 * Granted, each in the predicate and the builder at once because both are
 * the one #ClawtSandbox: the workspace (writable, and the working
 * directory), the state directory (writable: the rendered config, the
 * session store, the usage database and the link token are there), the
 * computer's mounts in their declared modes, the CLI's login directory
 * (writable), the AI CLI and every program the workspace's .mcp.json
 * names (read-only), the daemon's sockets, and `runtime.confine_paths`
 * (read-only).  $HOME is an empty tmpfs those come back on top of; /tmp is
 * private.  The network follows `runtime.network`: shared when open, a
 * namespace of its own with only loopback in it otherwise.
 * On a host computer, `computer.host.deny_paths` are denied here as well.
 *
 * Paths that do not exist are left out rather than bound: bwrap refuses
 * to start on a missing source, and a login directory nobody has created
 * yet is not a reason the agent cannot run.
 *
 * Returns: (transfer full): the sandbox, or %NULL with @error set
 */
ClawtSandbox *clawt_agent_confine_build(ClawtAgentConfig     *config,
                                        const gchar          *state_dir,
                                        GPtrArray            *mounts,
                                        const gchar * const  *sockets,
                                        const gchar          *libreclaw,
                                        GError              **error);

/**
 * clawt_agent_confine_label:
 *
 * What the clients call the setting, so the three of them say it the same
 * way.
 *
 * Returns: (transfer none): the label
 */
const gchar *clawt_agent_confine_label(void);

/**
 * clawt_agent_confine_explanation:
 *
 * One short paragraph under the setting: what it does, what it costs, and
 * what happens when bubblewrap is missing.
 *
 * Returns: (transfer none): the explanation
 */
const gchar *clawt_agent_confine_explanation(void);

/**
 * clawt_agent_confine_notice:
 * @mode: (nullable): the agent's `runtime.confine` nickname
 * @runtime_type: (nullable): the agent's `runtime.type` nickname
 * @available: whether the daemon found bubblewrap
 *
 * The warning a client shows beside the setting when it cannot be given,
 * or %NULL when there is nothing to warn about.
 *
 * Decided here rather than in each client, because it is the same two
 * refusals clawt_agent_confine_check() makes and a client that worded its
 * own would drift from the reason the agent is actually disabled for.
 *
 * Returns: (transfer none) (nullable): the notice
 */
const gchar *clawt_agent_confine_notice(const gchar *mode,
                                        const gchar *runtime_type,
                                        gboolean     available);

G_END_DECLS
