/*
 * clawt-exchange.h - The shared drop-box agents pass files through
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
#include "computer/clawt-mount.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_EXCHANGE (clawt_exchange_get_type())

G_DECLARE_FINAL_TYPE(ClawtExchange, clawt_exchange, CLAWT, EXCHANGE, GObject)

/**
 * CLAWT_EXCHANGE_MOUNT_POINT:
 *
 * Where the exchange appears inside every computer.
 */
#define CLAWT_EXCHANGE_MOUNT_POINT "/mnt/clawtilla/exchange"

/**
 * clawt_exchange_new:
 * @root: the exchange directory
 * @max_bytes: size cap for the whole exchange, or 0 for none
 *
 * Returns: (transfer full): a new #ClawtExchange
 */
ClawtExchange *clawt_exchange_new(const gchar *root, gint64 max_bytes);

/**
 * clawt_exchange_prepare:
 * @self: a #ClawtExchange
 * @agent_id: the agent
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the shared area and this agent's own drop-box.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_exchange_prepare(ClawtExchange  *self,
                                const gchar    *agent_id,
                                GError        **error);

/**
 * clawt_exchange_get_root:
 * @self: a #ClawtExchange
 *
 * Returns: (transfer none): the exchange directory on the host
 */
const gchar *clawt_exchange_get_root(ClawtExchange *self);

/**
 * clawt_exchange_get_mounts:
 * @self: a #ClawtExchange
 * @agent_id: (nullable): whose computer this is
 *
 * The mounts that make the exchange visible inside a computer.
 *
 * Three of them, not one: the whole exchange read-only, then `shared/`
 * and the agent's own directory read-write on top.  A single read-write
 * mount of the root made the per-agent write rule unenforceable --
 * anything in the computer could simply write into another agent's
 * directory, which is precisely what the rule exists to stop.
 *
 * Returns: (transfer full) (element-type ClawtMount): the mounts
 */
GPtrArray *clawt_exchange_get_mounts(ClawtExchange *self,
                                     const gchar   *agent_id);

/**
 * clawt_exchange_resolve:
 * @self: a #ClawtExchange
 * @agent_id: the agent asking
 * @relative: a path within the exchange, as the agent wrote it
 * @for_writing: whether the agent intends to write
 * @error: (out) (optional): return location for a #GError
 *
 * Turns a path an agent supplied into a real one, or refuses.
 *
 * An agent may write to `shared/` and to its own directory, and read
 * everywhere.  The path is canonicalised before it is checked, so `..`
 * and a symlink pointing outward are both caught by the same test rather
 * than by two that can disagree.
 *
 * Returns: (transfer full) (nullable): the host path, or %NULL if refused
 */
gchar *clawt_exchange_resolve(ClawtExchange  *self,
                              const gchar    *agent_id,
                              const gchar    *relative,
                              gboolean        for_writing,
                              GError        **error);

/**
 * clawt_exchange_list:
 * @self: a #ClawtExchange
 * @relative: (nullable): a directory within the exchange, or %NULL for the top
 *
 * Returns: (transfer full) (element-type utf8): entry names, or %NULL if
 *   the directory could not be read
 */
GPtrArray *clawt_exchange_list(ClawtExchange *self, const gchar *relative);

/**
 * clawt_exchange_get_size:
 * @self: a #ClawtExchange
 *
 * Returns: how many bytes the exchange currently holds
 */
gint64 clawt_exchange_get_size(ClawtExchange *self);

/**
 * clawt_exchange_sweep:
 * @self: a #ClawtExchange
 * @max_age_days: delete files older than this, or 0 to only enforce size
 *
 * Removes old files, then oldest-first until the size cap is met.
 *
 * Returns: how many files were removed
 */
guint clawt_exchange_sweep(ClawtExchange *self, gint max_age_days);

G_END_DECLS
