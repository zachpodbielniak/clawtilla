/*
 * clawt-mount.h - A host path shared into an agent's computer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One mount, described once and translated per backend: bind mounts for a
 * container, virtiofs shares for a VM, and for a host computer the
 * confinement allowlist itself.  Describing it once is the point -- the
 * three backends spell the same idea very differently, and a user should
 * not have to know which.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MOUNT (clawt_mount_get_type())

GType clawt_mount_get_type(void) G_GNUC_CONST;

/**
 * clawt_mount_new:
 * @source: (nullable): host path, or %NULL for tmpfs
 * @target: absolute path inside the computer
 *
 * Returns: (transfer full): a new #ClawtMount
 */
ClawtMount *clawt_mount_new(const gchar *source,
                            const gchar *target);

ClawtMount *clawt_mount_copy(ClawtMount *self);
void        clawt_mount_free(ClawtMount *self);

const gchar    *clawt_mount_get_source(ClawtMount *self);
const gchar    *clawt_mount_get_target(ClawtMount *self);
ClawtMountMode  clawt_mount_get_mode(ClawtMount *self);
ClawtMountType  clawt_mount_get_mount_type(ClawtMount *self);
ClawtRelabel    clawt_mount_get_relabel(ClawtMount *self);
const gchar    *clawt_mount_get_size(ClawtMount *self);
gboolean        clawt_mount_get_create(ClawtMount *self);
gboolean        clawt_mount_get_required(ClawtMount *self);

void clawt_mount_set_mode(ClawtMount *self, ClawtMountMode mode);
void clawt_mount_set_mount_type(ClawtMount *self, ClawtMountType type);
void clawt_mount_set_relabel(ClawtMount *self, ClawtRelabel relabel);
void clawt_mount_set_size(ClawtMount *self, const gchar *size);
void clawt_mount_set_create(ClawtMount *self, gboolean create);
void clawt_mount_set_required(ClawtMount *self, gboolean required);

/**
 * clawt_mount_validate:
 * @self: a #ClawtMount
 * @error: (out) (optional): return location for a #GError
 *
 * Checks one mount in isolation: that the target is absolute, that a
 * non-tmpfs mount has a source, that the source exists or may be created,
 * and that neither path tries to escape through "..".
 *
 * Returns: %TRUE if the mount is usable
 */
gboolean clawt_mount_validate(ClawtMount  *self,
                              GError     **error);

/**
 * clawt_mount_set_forbidden_sources:
 * @paths: (array zero-terminated=1) (nullable): directories no mount may
 *   expose, or %NULL to clear
 *
 * Directories that must never be mounted into any computer.
 *
 * The daemon's state directory holds every agent's link token and
 * resolved credentials, so an agent that could read it could read every
 * other agent's mail and impersonate any of them.  The refusal was
 * documented from the start and simply never implemented; validation
 * checked a mount's shape and never what it pointed at.
 *
 * Set once by the daemon at startup, because a mount is validated in
 * several places and each of them needs the same answer.
 */
void clawt_mount_set_forbidden_sources(const gchar * const *paths);

/**
 * CLAWT_MOUNT_TAG_MAX:
 *
 * How long a virtiofs tag may be, in bytes.
 *
 * qemu's limit, and it refuses the *device* rather than truncating:
 * "tag property must be 36 bytes or less", at which point the domain
 * does not start at all.
 */
#define CLAWT_MOUNT_TAG_MAX (36)

/**
 * clawt_mount_tag:
 * @target: the path the share appears at inside the guest
 *
 * The name the guest mounts the share by.
 *
 * A `<filesystem>` device is addressed by tag, and the tag used to be
 * the target path itself -- which works until the path is 37 bytes.
 * `/mnt/clawtilla/exchange/ubuntu-tester` is, and the domain then fails
 * to start with an error about a property nobody set by hand.
 *
 * Pure, and stable for ever: the tag is written into the guest's fstab
 * at first boot and into the domain XML on every provision, so a tag
 * that changed would leave the guest mounting a device that no longer
 * exists.  Both callers go through here for the same reason -- two
 * spellings of it would differ exactly once and the share would be
 * silently missing.
 *
 * Returns: (transfer full): a tag of at most %CLAWT_MOUNT_TAG_MAX bytes
 */
gchar *clawt_mount_tag(const gchar *target);

/**
 * clawt_mount_resolved_source:
 * @self: a #ClawtMount
 *
 * The source with "~" and XDG variables expanded, and symlinks resolved.
 *
 * Resolved rather than literal because the resolved path is what actually
 * gets mounted, and what has to be checked against the paths an agent must
 * never reach.
 *
 * Returns: (transfer full) (nullable): the real path, or %NULL
 */
gchar *clawt_mount_resolved_source(ClawtMount *self);

/**
 * clawt_mount_get_scope:
 * @self: a #ClawtMount
 *
 * Who this mount is for, when it is one of the fleet's shared folders.
 *
 * %CLAWT_SCOPE_ALL unless the entry said otherwise, because a folder
 * put in `defaults.mounts` is by definition a default -- and a mount an
 * agent declared for itself is already agent-scoped, so this is
 * meaningless there and left at ALL.
 *
 * Returns: the scope
 */
ClawtScope clawt_mount_get_scope(ClawtMount *self);

/**
 * clawt_mount_set_scope:
 * @self: a #ClawtMount
 * @scope: a #ClawtScope
 */
void clawt_mount_set_scope(ClawtMount *self, ClawtScope scope);

/**
 * clawt_mount_get_agents:
 * @self: a #ClawtMount
 *
 * The agent ids named by a `selected` scope.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the ids
 */
const gchar * const *clawt_mount_get_agents(ClawtMount *self);

/**
 * clawt_mount_set_agents:
 * @self: a #ClawtMount
 * @agents: (nullable) (array zero-terminated=1): agent ids
 */
void clawt_mount_set_agents(ClawtMount *self, const gchar * const *agents);

/**
 * clawt_mount_get_teams:
 * @self: a #ClawtMount
 *
 * The team ids named by a `selected` scope.
 *
 * Naming a team is how a shared folder covers a group without being
 * rewritten every time somebody joins it, which is the whole reason
 * teams exist.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the ids
 */
const gchar * const *clawt_mount_get_teams(ClawtMount *self);

/**
 * clawt_mount_set_teams:
 * @self: a #ClawtMount
 * @teams: (nullable) (array zero-terminated=1): team ids
 */
void clawt_mount_set_teams(ClawtMount *self, const gchar * const *teams);

/**
 * clawt_mount_covers:
 * @self: a #ClawtMount
 * @agent_id: the agent being asked about
 * @team: (nullable): the team that agent is on
 *
 * Whether this shared folder applies to that agent.
 *
 * Goes through clawt_scope_covers(), the same rule integrations use --
 * so "who gets this" has one answer in the tree rather than two that
 * differ on the case nobody tested.
 *
 * Returns: %TRUE if it applies
 */
gboolean clawt_mount_covers(ClawtMount  *self,
                            const gchar *agent_id,
                            const gchar *team);

/**
 * clawt_mount_merge_defaults:
 * @defaults: (element-type ClawtMount) (nullable): the fleet's shared
 *   folders, already filtered to the ones covering this agent
 * @own: (element-type ClawtMount) (nullable): what this agent declared
 *
 * The mounts an agent actually gets, with its own winning.
 *
 * A default that an agent has overridden by target must not also be
 * applied, or the two would both be mounted at one path -- which
 * validation refuses, so an agent that customised one shared folder
 * would stop starting entirely. Keyed on the target because the target
 * is what has to be unique; two sources cannot occupy one path inside
 * the computer.
 *
 * Defaults come first so the order in the rendered config reads
 * fleet-then-agent, which is the order somebody debugging one would
 * expect to find them in.
 *
 * A pure function, so the override rule can be asserted without a
 * container: the failure it prevents is an agent that will not start,
 * and reproducing that needs podman.
 *
 * Returns: (transfer full) (element-type ClawtMount): the effective
 *   list, holding copies
 */
GPtrArray *clawt_mount_merge_defaults(GPtrArray *defaults, GPtrArray *own);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMount, clawt_mount_free)

G_END_DECLS
