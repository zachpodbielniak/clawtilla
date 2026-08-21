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

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMount, clawt_mount_free)

G_END_DECLS
