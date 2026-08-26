/*
 * clawt-distrobox-computer.h - A distrobox per agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Between the container and the host rather than beside the container.
 * A distrobox *is* a podman container, but one deliberately wired into
 * the machine around it -- the same uid, the host's sockets, its
 * binaries reachable through distrobox-host-exec. That makes it a
 * comfortable place to build things and a poor place to put something
 * that should be contained.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "computer/clawt-computer.h"
#include "computer/clawt-pod-bridge.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_DISTROBOX_COMPUTER (clawt_distrobox_computer_get_type())

G_DECLARE_FINAL_TYPE(ClawtDistroboxComputer, clawt_distrobox_computer,
                     CLAWT, DISTROBOX_COMPUTER, ClawtComputer)

/**
 * clawt_distrobox_computer_new:
 * @agent_id: the agent this belongs to
 * @bridge: (transfer none): the podomation bridge
 * @image: (nullable): the image to create from, or %NULL for
 *   distrobox's own default
 *
 * Returns: (transfer full): a new #ClawtDistroboxComputer
 */
ClawtComputer *clawt_distrobox_computer_new(const gchar    *agent_id,
                                            ClawtPodBridge *bridge,
                                            const gchar    *image);

void clawt_distrobox_computer_set_name(ClawtDistroboxComputer *self,
                                       const gchar            *name);

/**
 * clawt_distrobox_computer_set_home:
 * @self: a #ClawtDistroboxComputer
 * @home: (nullable): the box's HOME, or %NULL to share the operator's
 *
 * Where the agent's home directory is.
 *
 * This is the whole confinement question for a distrobox, so it is a
 * setting of its own rather than something to be remembered as an entry
 * in `flags`. distrobox mounts the invoking user's real home when told
 * nothing, which is the tool working exactly as designed and is also a
 * box that can read that user's ssh keys and shell configuration.
 *
 * clawtilla therefore gives an agent a home of its own by default and
 * makes sharing the operator's an explicit choice -- the same shape as
 * the host computer defaulting to `confine: workspace` rather than to
 * `none`.
 */
void clawt_distrobox_computer_set_home(ClawtDistroboxComputer *self,
                                       const gchar            *home);

/**
 * clawt_distrobox_computer_set_share_home:
 * @self: a #ClawtDistroboxComputer
 * @share: whether the box gets the operator's own home directory
 *
 * Said out loud, because it cannot be arrived at by accident.
 *
 * A box that shares the operator's home can read their ssh keys, their
 * shell configuration and anything else they can -- so it is reached by
 * asking for it rather than by leaving a field empty.
 */
void clawt_distrobox_computer_set_share_home(ClawtDistroboxComputer *self,
                                             gboolean                share);

/**
 * clawt_distrobox_computer_resolve_home:
 * @self: a #ClawtDistroboxComputer
 *
 * What `--home` will be, or %NULL when none is passed -- which is
 * distrobox's default, and therefore the operator's own home.
 *
 * A pure function of the settings, so the one decision that matters can
 * be asserted on without distrobox installed. The two outcomes look
 * identical from inside the box and are completely different in what
 * they touch, which is exactly the sort of thing that should not be
 * checked by running it and looking.
 *
 * Returns: (transfer full) (nullable): the home directory
 */
gchar *clawt_distrobox_computer_resolve_home(ClawtDistroboxComputer *self);

/**
 * clawt_distrobox_computer_set_packages:
 * @self: a #ClawtDistroboxComputer
 * @packages: (nullable): whitespace-separated package names
 *
 * Installed once, when the box is first created. Changing this later
 * does not reach an existing box -- distrobox runs its setup at
 * creation, the same way a VM's cloud-init reads its seed once.
 */
void clawt_distrobox_computer_set_packages(ClawtDistroboxComputer *self,
                                           const gchar            *packages);

void clawt_distrobox_computer_set_flags(ClawtDistroboxComputer *self,
                                        const gchar            *flags);

/**
 * clawt_distrobox_computer_set_init:
 * @self: a #ClawtDistroboxComputer
 * @init: whether to run an init system inside the box
 *
 * Needed by anything that expects systemd -- a service, a user unit, a
 * timer. Off by default because it makes the box slower to start and
 * most agents never ask for one.
 */
void clawt_distrobox_computer_set_init(ClawtDistroboxComputer *self,
                                       gboolean                init);

/**
 * clawt_distrobox_computer_set_keep:
 * @self: a #ClawtDistroboxComputer
 * @keep: whether to leave the box behind when the agent stops
 *
 * On by default, unlike the plain container.
 *
 * A distrobox is expensive to make -- it pulls an image and then runs a
 * package install inside it -- and it is the kind of computer somebody
 * chose precisely so that what an agent installs survives. Throwing it
 * away at every stop would mean paying that cost on every start.
 */
void clawt_distrobox_computer_set_keep(ClawtDistroboxComputer *self,
                                       gboolean                keep);

/**
 * clawt_distrobox_computer_build_volume_args:
 * @mounts: (element-type ClawtMount): the mounts
 *
 * Renders the mount list as the whitespace-separated `--volume`
 * arguments distrobox takes.
 *
 * Exposed so it can be asserted on without podman or distrobox
 * installed: a read-only mount silently arriving read-write is the kind
 * of thing nobody notices until an agent overwrites something, and that
 * is worth a unit test rather than an integration one.
 *
 * Returns: (transfer full): the arguments, empty when there are none
 */
gchar *clawt_distrobox_computer_build_volume_args(GPtrArray *mounts);

G_END_DECLS
