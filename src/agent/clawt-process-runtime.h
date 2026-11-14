/*
 * clawt-process-runtime.h - Supervising a libreclaw child process
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The default way to host an agent.  A real `libreclaw -c <workspace>` runs
 * as its own process, so a crash stays contained, its environment and
 * credentials are genuinely separate, and it can live inside its own
 * container.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "agent/clawt-agent-runtime.h"
#include "computer/clawt-sandbox.h"
#include "net/clawt-egress-broker.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_PROCESS_RUNTIME (clawt_process_runtime_get_type())

G_DECLARE_FINAL_TYPE(ClawtProcessRuntime, clawt_process_runtime,
                     CLAWT, PROCESS_RUNTIME, ClawtAgentRuntime)

/**
 * clawt_process_runtime_new:
 * @config: (transfer none): the agent's configuration
 * @config_path: the rendered libreclaw config.yaml to run against
 *
 * Returns: (transfer full): a new #ClawtProcessRuntime
 */
ClawtProcessRuntime *clawt_process_runtime_new(ClawtAgentConfig *config,
                                               const gchar      *config_path);

/**
 * clawt_process_runtime_set_binary:
 * @self: a #ClawtProcessRuntime
 * @path: (nullable): the libreclaw binary, or %NULL to search PATH
 *
 * Overrides which libreclaw is run.  Chiefly for tests and for running
 * against a build tree rather than an installed copy.
 */
void clawt_process_runtime_set_binary(ClawtProcessRuntime *self,
                                      const gchar         *path);

/**
 * clawt_process_runtime_set_environment:
 * @self: a #ClawtProcessRuntime
 * @env: (element-type utf8 utf8): variables for the child
 *
 * Sets the child's environment.
 *
 * This is the whole environment, not an addition to the daemon's: the
 * daemon's own is deliberately not inherited, because a stray
 * ANTHROPIC_API_KEY leaking into a subscription CLI would quietly move it
 * onto pay-as-you-go billing nobody agreed to.
 */
void clawt_process_runtime_set_environment(ClawtProcessRuntime *self,
                                           GHashTable          *env);

/**
 * clawt_process_runtime_set_sandbox:
 * @self: a #ClawtProcessRuntime
 * @sandbox: (nullable): the bubblewrap sandbox to start the child in, or
 *   %NULL to start it unconfined
 *
 * Confines the libreclaw child, and so everything it spawns, in @sandbox
 * from the next start -- `runtime.confine: bwrap`, built by
 * clawt_agent_confine_build().  Set before every start rather than once,
 * because a runtime outlives the configuration it was built from.
 */
void clawt_process_runtime_set_sandbox(ClawtProcessRuntime *self,
                                       ClawtSandbox        *sandbox);

/**
 * clawt_process_runtime_set_egress:
 * @self: a #ClawtProcessRuntime
 * @network: the agent's `runtime.network`
 * @gate_socket: (nullable): the agent's egress-gate socket, for
 *   %CLAWT_RUNTIME_NETWORK_GATED
 * @broker: (nullable): the daemon's provider-key broker
 *
 * Where the next start's child may connect.  Anything but open needs a
 * sandbox (clawt_process_runtime_set_sandbox()) and is refused at
 * prepare without one.  Under gated the box runs clawtilla-relay, which
 * bridges 127.0.0.1:3128 to @gate_socket and 127.0.0.1:3129 to the
 * broker, and the child is handed HTTPS_PROXY and HTTP_PROXY for the
 * first; under none only the broker's port is bridged.  Keys the broker
 * can hold are replaced with a token minted for that one process, which
 * its exit revokes.
 */
void clawt_process_runtime_set_egress(ClawtProcessRuntime *self,
                                      ClawtRuntimeNetwork  network,
                                      const gchar         *gate_socket,
                                      ClawtEgressBroker   *broker);

/**
 * clawt_process_runtime_set_relay_binary:
 * @self: a #ClawtProcessRuntime
 * @path: (nullable): clawtilla-relay, or %NULL to look for it beside the
 *   running binary, one directory above it, then on PATH
 *
 * Overrides where clawtilla-relay is found.
 */
void clawt_process_runtime_set_relay_binary(ClawtProcessRuntime *self,
                                            const gchar         *path);

/**
 * clawt_process_runtime_prepare:
 * @self: a #ClawtProcessRuntime
 * @out_argv: (out) (array zero-terminated=1) (transfer full): the command
 * @out_environment: (out) (array zero-terminated=1) (transfer full): its
 *   environment, as `NAME=value` strings
 * @error: (out) (optional): return location for a #GError
 *
 * Works out exactly what the next start will spawn, without spawning it.
 *
 * Start runs this and nothing else decides the command, so a test can
 * read the whole of it -- the bwrap argv and the environment the sandbox
 * owns -- as data, with no bubblewrap installed.  Under a sandbox it also
 * adds the libreclaw binary to it, refuses a bind whose file has a second
 * hard link (clawt_sandbox_check_links()), and fails rather than
 * downgrade when bubblewrap cannot be found.
 *
 * Returns: %TRUE if the child can be started
 */
gboolean clawt_process_runtime_prepare(ClawtProcessRuntime  *self,
                                       GStrv                *out_argv,
                                       GStrv                *out_environment,
                                       GError              **error);

/**
 * clawt_process_runtime_get_superseded_exits:
 * @self: a #ClawtProcessRuntime
 *
 * How many times a child this runtime had already let go of was seen to
 * exit.
 *
 * A runtime replaces its child in place, so a wait started for one child
 * can complete after another has taken its place -- which happens when
 * the stop path gives up on a child the kernel will not reap.  Those
 * exits are not the runtime's own and are ignored; this is the only way
 * to tell from outside that any happened.
 *
 * Returns: the count since this runtime was created
 */
guint clawt_process_runtime_get_superseded_exits(ClawtProcessRuntime *self);


G_END_DECLS
