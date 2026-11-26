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
#include "computer/clawt-container-limits.h"
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
/**
 * clawt_container_computer_set_connection:
 * @self: a #ClawtContainerComputer
 * @connection: (nullable): a podman connection -- a URI, a socket path, or
 *   "unix" for the local default
 *
 * Chooses which podman the agent's container lives in.
 *
 * The default is resolved at construction: the rootless socket under
 * XDG_RUNTIME_DIR when there is one, otherwise the system socket. %NULL,
 * an empty string and "unix" all keep that default.
 */
void clawt_container_computer_set_connection(ClawtContainerComputer *self,
                                             const gchar            *connection);

/**
 * clawt_container_computer_get_connection:
 * @self: a #ClawtContainerComputer
 *
 * Returns: (transfer none) (nullable): the connection URI in use
 */
const gchar *clawt_container_computer_get_connection(ClawtContainerComputer *self);

/**
 * clawt_container_computer_set_command:
 * @self: a #ClawtContainerComputer
 * @command: (nullable): a JSON array of arguments, or a plain string that
 *   is split on spaces
 *
 * Sets what the container runs.
 *
 * Defaults to something long-lived, because a container computer exists
 * to be exec'd into and a plain base image's entrypoint exits at once.
 * Set it when the image has an entrypoint of its own worth running.
 */
void clawt_container_computer_set_command(ClawtContainerComputer *self,
                                          const gchar            *command);

/**
 * clawt_container_computer_get_command:
 * @self: a #ClawtContainerComputer
 *
 * Returns: (transfer none) (nullable): the command, as a JSON array
 */
const gchar *clawt_container_computer_get_command(ClawtContainerComputer *self);

void clawt_container_computer_set_network(ClawtContainerComputer *self,
                                          const gchar            *network);

/**
 * clawt_container_computer_set_env:
 * @self: a #ClawtContainerComputer
 * @env_json: (nullable): a JSON object of variable names to values, or
 *   %NULL for none
 *
 * The environment the container is created with.  Read at the next
 * provision, so it reaches a container created after the call and never
 * one that already exists.  The values travel to podman's API and no
 * further: nothing here logs them.
 */
void clawt_container_computer_set_env(ClawtContainerComputer *self,
                                      const gchar            *env_json);

/**
 * clawt_container_computer_set_ports:
 * @self: a #ClawtContainerComputer
 * @ports_json: (nullable): a JSON array of libpod port mappings
 *   (`host_ip`, `host_port`, `container_port`, `protocol`), or %NULL
 *
 * The ports the container publishes, read at the next provision.
 */
void clawt_container_computer_set_ports(ClawtContainerComputer *self,
                                        const gchar            *ports_json);

/**
 * clawt_container_computer_get_container_id:
 * @self: a #ClawtContainerComputer
 *
 * Returns: (transfer none) (nullable): podman's id for the container this
 *   object created, or %NULL before a create or after a removal
 */
const gchar *clawt_container_computer_get_container_id(
    ClawtContainerComputer *self);

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
 * clawt_container_computer_set_reuse:
 * @self: a #ClawtContainerComputer
 * @reuse: whether a container already under this name may be adopted
 *
 * The default, %TRUE, is what makes keep mean "across daemon restarts"
 * too: the first start of a new object finds a container left under its
 * name, compares it with the config (see
 * clawt_container_computer_compare()), and adopts it unless something
 * podman cannot change in place differs.
 *
 * %FALSE replaces a container found under the name at every first start
 * of an object, saying so in the log.  For a machine whose whole spec is
 * rebuilt each time it starts -- the broker's, whose environment and
 * ports are not compared -- so a change to them is never left behind in
 * an adopted container.  A container that is not kept is never adopted,
 * whatever this says.
 */
void clawt_container_computer_set_reuse(ClawtContainerComputer *self,
                                        gboolean                reuse);

/**
 * clawt_container_computer_compare:
 * @self: a #ClawtContainerComputer
 * @inspected: what `podman container inspect` printed for the container:
 *   a JSON array of one object, or the object
 * @reason: (out) (transfer full) (nullable): why the container must be
 *   recreated, or %NULL when it can be kept
 * @error: return location for a #GError
 *
 * Whether an existing container still matches what this object would
 * create, judged only on what podman cannot change in place: the image
 * (by name, so a re-pulled tag is the same image), the bind mounts
 * (added, removed, or with a different source or mode), the SELinux
 * level when one is wanted, and the command.  Limits are not compared --
 * they are applied in place at every start.
 *
 * @reason reads "image changed (…)", "mounts changed (+/added, -/gone,
 * ~/different)", "SELinux level changed (…)" or "command changed (…)",
 * joined with "; " when several hold.  Anything @inspected does not say
 * counts as unchanged.
 *
 * Returns: %TRUE when the comparison was made; %FALSE with @error set when
 *   @inspected could not be read
 */
gboolean clawt_container_computer_compare(ClawtContainerComputer  *self,
                                          const gchar             *inspected,
                                          gchar                  **reason,
                                          GError                 **error);

/**
 * clawt_container_computer_get_last_recreate:
 * @self: a #ClawtContainerComputer
 *
 * The last time this object removed a kept container it had adopted and
 * made it again, as one sentence: the container, when, and why.  The
 * daemon reports it on the Fleet page (the warnings of team.list) and as
 * computer_recreated in agent.list, so somebody whose agent lost what it
 * installed can see what did it.
 *
 * Returns: (transfer none) (nullable): the sentence, or %NULL when this
 *   object has not recreated a kept container
 */
const gchar *
clawt_container_computer_get_last_recreate(ClawtContainerComputer *self);

/**
 * clawt_container_computer_set_limits:
 * @self: a #ClawtContainerComputer
 * @limits: (nullable): the process and memory limits, or %NULL for none
 *
 * Copied. Applied with `podman update` before the container's first
 * start and again at every start whose limits differ from what podman
 * reports, so a changed limit reaches the existing container without a
 * rebuild. (podomation's create request carries no limits, which is why
 * the first application is an update too; the container never runs
 * without them.)
 */
void clawt_container_computer_set_limits(ClawtContainerComputer     *self,
                                         const ClawtContainerLimits *limits);

/**
 * clawt_container_computer_get_limits:
 * @self: a #ClawtContainerComputer
 *
 * Returns: (transfer none) (nullable): the limits, or %NULL for none
 */
const ClawtContainerLimits *
clawt_container_computer_get_limits(ClawtContainerComputer *self);

/**
 * clawt_container_computer_apply_limits:
 * @self: a #ClawtContainerComputer
 * @error: return location for a #GError
 *
 * Gives the existing container its limits now, with `podman update` and
 * only for what differs; see clawt_container_limits_apply(). Run by every
 * start, and by the daemon when a limit is changed under a running one.
 * Blocking: call it from a worker thread.
 *
 * Returns: %TRUE if the container has its limits, or has none to have
 */
gboolean clawt_container_computer_apply_limits(ClawtContainerComputer  *self,
                                               GError                 **error);

/**
 * clawt_container_computer_get_name:
 * @self: a #ClawtContainerComputer
 *
 * Returns: (transfer none): the container's name in podman
 */
const gchar *clawt_container_computer_get_name(ClawtContainerComputer *self);

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

/**
 * clawt_container_computer_set_selinux_level:
 * @self: a #ClawtContainerComputer
 * @level: (nullable): the MCS level, such as "s0:c100,c200", or %NULL for
 *   podman's random pair
 *
 * The level the container is created at.  It only takes effect when the
 * container is created: an existing container keeps the level it was
 * made with until it is recreated.  A level that is not a valid MCS level
 * is ignored with a warning rather than passed on, so podman is never
 * asked for something it will refuse at create time.
 */
void clawt_container_computer_set_selinux_level(ClawtContainerComputer *self,
                                                const gchar            *level);

/**
 * clawt_container_computer_get_selinux_level:
 * @self: a #ClawtContainerComputer
 *
 * Returns: (transfer none) (nullable): the level the container is, or
 *   will be, created at; %NULL for podman's random pair
 */
const gchar *
clawt_container_computer_get_selinux_level(ClawtContainerComputer *self);

/**
 * clawt_container_computer_build_create_params:
 * @self: a #ClawtContainerComputer
 *
 * The parameters podomation's container `create` is called with: image,
 * name, mounts, command, and -- only when one is set -- selinux_level,
 * plus env and ports for the broker's containers.
 *
 * Exposed so what reaches podman can be asserted without podman: a level
 * that silently goes missing gives the container a random pair, and the
 * first sign is a teammate unable to read a file.
 *
 * Returns: (transfer full) (element-type utf8 utf8): the parameters
 */
GHashTable *
clawt_container_computer_build_create_params(ClawtContainerComputer *self);

G_END_DECLS
