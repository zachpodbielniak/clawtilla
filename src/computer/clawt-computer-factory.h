/*
 * clawt-computer-factory.h - Building a computer from configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One place that turns an agent's `computer:` block into the right backend
 * with the right confinement, mounts and desktop.  Spreading that across
 * the daemon would mean each caller reproducing the defaulting rules, and
 * disagreeing about them.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "computer/clawt-computer.h"
#include "computer/clawt-desktop.h"
#include "computer/clawt-pod-bridge.h"

G_BEGIN_DECLS

/**
 * clawt_computer_factory_create:
 * @agent_config: (transfer none): the agent's configuration
 * @bridge: (transfer none) (nullable): the podomation bridge
 * @default_mounts: (element-type ClawtMount) (nullable) (transfer none):
 *   the mounts every computer gets, before the agent's own
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the computer an agent's configuration describes.
 *
 * Returns: (transfer full) (nullable): the computer, or %NULL if the
 *   configuration cannot be satisfied
 */
ClawtComputer *clawt_computer_factory_create(ClawtAgentConfig  *agent_config,
                                             GPtrArray         *default_mounts,
                                             ClawtPodBridge    *bridge,
                                             GError           **error);

/**
 * clawt_computer_factory_create_desktop:
 * @agent_config: (transfer none): the agent's configuration
 *
 * Builds the desktop add-on, if the agent asked for one.
 *
 * Returns: (transfer full) (nullable): the desktop, or %NULL
 */
ClawtDesktop *clawt_computer_factory_create_desktop(
    ClawtAgentConfig *agent_config);

G_END_DECLS
