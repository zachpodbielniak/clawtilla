/*
 * clawt-pod-bridge.h - Talking to podomation's modules
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * podomation already knows how to drive podman and libvirt, and its modules
 * are loadable, introspectable and tested.  Reimplementing either here
 * would be a second copy of somebody else's protocol handling to keep
 * correct.
 *
 * Its interface is GVariant in, GVariant out, keyed by event name.  This
 * turns that into typed C, and turns its parameter metadata into
 * #ClawtParamInfo so the same descriptions can become MCP tool schemas
 * without being written twice.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_POD_BRIDGE (clawt_pod_bridge_get_type())

G_DECLARE_FINAL_TYPE(ClawtPodBridge, clawt_pod_bridge,
                     CLAWT, POD_BRIDGE, GObject)

/**
 * clawt_pod_bridge_new:
 * @module_dir: (nullable): where podomation's modules are, or %NULL for the
 *   compiled-in location
 *
 * Returns: (transfer full): a new #ClawtPodBridge
 */
ClawtPodBridge *clawt_pod_bridge_new(const gchar *module_dir);

/**
 * clawt_pod_bridge_load_module:
 * @self: a #ClawtPodBridge
 * @module_name: e.g. "container" or "vm_virtmanager"
 * @error: (out) (optional): return location for a #GError
 *
 * Loads one podomation module.
 *
 * Returns: %TRUE if it is available
 */
gboolean clawt_pod_bridge_load_module(ClawtPodBridge  *self,
                                      const gchar     *module_name,
                                      GError         **error);

/**
 * clawt_pod_bridge_has_module:
 * @self: a #ClawtPodBridge
 * @module_name: a module name
 *
 * Returns: %TRUE if the module is loaded and usable
 */
gboolean clawt_pod_bridge_has_module(ClawtPodBridge *self,
                                     const gchar    *module_name);

/**
 * clawt_pod_bridge_call:
 * @self: a #ClawtPodBridge
 * @module_name: which module
 * @action: the handler to invoke, e.g. "start"
 * @params: (transfer none) (element-type utf8 utf8): named arguments
 * @error: (out) (optional): return location for a #GError
 *
 * Invokes a module handler.
 *
 * Returns: (transfer full) (nullable) (element-type utf8 utf8): the result,
 *   or %NULL on failure
 */
GHashTable *clawt_pod_bridge_call(ClawtPodBridge  *self,
                                  const gchar     *module_name,
                                  const gchar     *action,
                                  GHashTable      *params,
                                  GError         **error);

/**
 * clawt_pod_bridge_get_module_dir:
 * @self: a #ClawtPodBridge
 *
 * Returns: (transfer none): where modules are being loaded from
 */
const gchar *clawt_pod_bridge_get_module_dir(ClawtPodBridge *self);

G_END_DECLS
