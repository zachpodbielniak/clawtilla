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
