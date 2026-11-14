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
 * clawt_computer_factory_create_shared:
 * @computer_id: the entry under `computers:`
 * @users: (element-type ClawtAgentConfig): every agent that uses it; the
 *   first is read for the machine's settings, which all of them share
 * @default_mounts: (nullable) (element-type ClawtMount): the fleet folders
 * @bridge: (nullable): the pod bridge, for container and VM backends
 * @error: return location for a #GError
 *
 * Builds one machine for several agents: named `clawt-c.<id>`, carrying
 * each agent's workspace at /mnt/clawtilla/workspaces/<agent> and the
 * fleet folders that cover any of them.
 *
 * Returns: (transfer full) (nullable): the computer
 */
ClawtComputer *clawt_computer_factory_create_shared(const gchar       *computer_id,
                                                    GPtrArray         *users,
                                                    GPtrArray         *default_mounts,
                                                    ClawtPodBridge    *bridge,
                                                    GError           **error);

/**
 * clawt_computer_factory_limits:
 * @agent_config: (transfer none): the agent's configuration
 *
 * computer.container.pids_limit, memory and memory_swap, as one value.
 * Read for the container and distrobox backends alike, both of which are
 * a podman container underneath. Not validated here; see
 * clawt_container_limits_validate().
 *
 * Returns: (transfer full): the limits; empty when none are set
 */
ClawtContainerLimits *clawt_computer_factory_limits(
    ClawtAgentConfig *agent_config);

/**
 * clawt_computer_factory_refresh:
 * @agent_config: (transfer none): the configuration @computer was built
 *   from, as it is now
 * @computer: a computer this factory built
 * @error: return location for a #GError
 *
 * Re-reads the settings a computer can take without being rebuilt: when
 * it starts (computer.start), its podman limits, and its wake command.
 *
 * A computer is kept across agent restarts and is not rebuilt while its
 * machine runs, so without this a changed limit would wait for a rebuild
 * it does not need -- the point of applying limits with `podman update`
 * is that it does not. The next start applies what this sets.
 *
 * Returns: %FALSE if the new limits are ones podman would refuse
 */
gboolean clawt_computer_factory_refresh(ClawtAgentConfig  *agent_config,
                                        ClawtComputer     *computer,
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
