/*
 * clawt-container-computer.h - A podman container per agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The middle ground between no computer and the real host: a real
 * filesystem and a real shell, with a kernel boundary around them, and
 * nothing visible but what was mounted in.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "computer/clawt-computer.h"
#include "computer/clawt-pod-bridge.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_CONTAINER_COMPUTER (clawt_container_computer_get_type())

G_DECLARE_FINAL_TYPE(ClawtContainerComputer, clawt_container_computer,
                     CLAWT, CONTAINER_COMPUTER, ClawtComputer)

/**
 * clawt_container_computer_new:
 * @agent_id: the agent this belongs to
 * @bridge: (transfer none): the podomation bridge
 * @image: the image to create from
 *
 * Returns: (transfer full): a new #ClawtContainerComputer
 */
ClawtComputer *clawt_container_computer_new(const gchar    *agent_id,
                                            ClawtPodBridge *bridge,
                                            const gchar    *image);

void clawt_container_computer_set_name(ClawtContainerComputer *self,
                                       const gchar            *name);
void clawt_container_computer_set_network(ClawtContainerComputer *self,
                                          const gchar            *network);

/**
 * clawt_container_computer_set_keep:
 * @self: a #ClawtContainerComputer
 * @keep: whether to leave the container behind when the agent stops
 *
 * Useful when the agent installs things it should not have to reinstall
 * every time it starts.
 */
void clawt_container_computer_set_keep(ClawtContainerComputer *self,
                                       gboolean                keep);

/**
 * clawt_container_computer_build_mount_json:
 * @mounts: (element-type ClawtMount): the mounts
 *
 * Renders the mount list as the JSON podomation's container module expects.
 *
 * Exposed so it can be tested without podman running: getting SELinux
 * relabelling or read-only flags wrong is silent until an agent cannot read
 * a directory it should, and that is worth a unit test rather than an
 * integration one.
 *
 * Returns: (transfer full): the JSON array
 */
gchar *clawt_container_computer_build_mount_json(GPtrArray *mounts);

G_END_DECLS
