/*
 * clawt-embedded-runtime.h - Running an agent inside the daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "agent/clawt-agent-runtime.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_EMBEDDED_RUNTIME (clawt_embedded_runtime_get_type())

G_DECLARE_FINAL_TYPE(ClawtEmbeddedRuntime, clawt_embedded_runtime,
                     CLAWT, EMBEDDED_RUNTIME, ClawtAgentRuntime)

/**
 * clawt_embedded_runtime_new:
 * @config: (transfer none): the agent's configuration
 * @config_path: the rendered libreclaw config.yaml
 * @main_context: (nullable): the context to attach sources to
 *
 * Runs the agent as an #LcApp inside this process rather than as a child.
 *
 * Cheaper than a process and the only option when there is no libreclaw
 * binary to run -- which is the case when clawtilla is embedded in
 * something else, such as cmacs.  In exchange the agent cannot have its
 * own environment or credentials, cannot be sandboxed away from the
 * daemon, and cannot be interrupted by a signal: there is no process to
 * signal.  clawt_agent_runtime_get_caps() reports that honestly rather
 * than offering controls that would do nothing.
 *
 * Returns: (transfer full): a new #ClawtEmbeddedRuntime
 */
ClawtEmbeddedRuntime *clawt_embedded_runtime_new(ClawtAgentConfig *config,
                                                 const gchar      *config_path,
                                                 GMainContext     *main_context);

/**
 * clawt_embedded_runtime_get_app:
 * @self: a #ClawtEmbeddedRuntime
 *
 * The #LcApp backing this agent, or %NULL while it is stopped.
 *
 * Returns: (transfer none) (nullable): the app
 */
LcApp *clawt_embedded_runtime_get_app(ClawtEmbeddedRuntime *self);

G_END_DECLS
