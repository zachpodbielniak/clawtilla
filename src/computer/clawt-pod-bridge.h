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
 * @module_dir: (nullable): the one directory to load modules from, or %NULL
 *   to search the default path
 *
 * Naming @module_dir means exactly that directory is used: a caller that
 * says where the modules are gets a legible failure rather than a quiet
 * success from somewhere else.
 *
 * The default path is, best first: every entry of the
 * =CLAWT_POD_MODULE_DIR= environment variable (colon-separated),
 * =pod-modules/= and =modules/= beside the running binary, then the
 * compiled-in install location.  The binary-relative entries are what
 * make an uninstalled clawtilla work against the modules the build just
 * produced.
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
 * clawt_pod_bridge_load_module_for:
 * @self: a #ClawtPodBridge
 * @module_name: e.g. "container" or "vm_virtmanager"
 * @connection_uri: (nullable): the backend to talk to, or %NULL for the
 *   module's own default
 * @error: (out) (optional): return location for a #GError
 *
 * Loads a module instance bound to one connection.
 *
 * Instances are cached per (module, connection): a module carries its
 * connection, so two agents pointed at different podman sockets need two
 * of them, and sharing one meant the second agent silently talked to the
 * first one's daemon. A module with no connection-uri property ignores
 * @connection_uri.
 *
 * Returns: %TRUE if it is available
 */
gboolean clawt_pod_bridge_load_module_for(ClawtPodBridge  *self,
                                          const gchar     *module_name,
                                          const gchar     *connection_uri,
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
 * clawt_pod_bridge_has_module_for:
 * @self: a #ClawtPodBridge
 * @module_name: a module name
 * @connection_uri: (nullable): the connection it was loaded for
 *
 * Returns: %TRUE if that instance is loaded and usable
 */
gboolean clawt_pod_bridge_has_module_for(ClawtPodBridge *self,
                                         const gchar    *module_name,
                                         const gchar    *connection_uri);

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
 * clawt_pod_bridge_call_for:
 * @self: a #ClawtPodBridge
 * @module_name: which module
 * @connection_uri: (nullable): which instance, as passed to
 *   clawt_pod_bridge_load_module_for()
 * @action: the handler to invoke, e.g. "start"
 * @params: (transfer none) (element-type utf8 utf8): named arguments
 * @error: (out) (optional): return location for a #GError
 *
 * Invokes a handler on the instance bound to @connection_uri.
 *
 * Returns: (transfer full) (nullable) (element-type utf8 utf8): the result,
 *   or %NULL on failure
 */
GHashTable *clawt_pod_bridge_call_for(ClawtPodBridge  *self,
                                      const gchar     *module_name,
                                      const gchar     *connection_uri,
                                      const gchar     *action,
                                      GHashTable      *params,
                                      GError         **error);

/**
 * clawt_pod_bridge_get_module_dir:
 * @self: a #ClawtPodBridge
 *
 * Returns: (transfer none) (nullable): the first directory searched
 */
const gchar *clawt_pod_bridge_get_module_dir(ClawtPodBridge *self);

/**
 * clawt_pod_bridge_get_search_path:
 * @self: a #ClawtPodBridge
 *
 * Every directory a module is looked for in, best first.
 *
 * Returns: (transfer none) (array zero-terminated=1): the search path
 */
const gchar * const *clawt_pod_bridge_get_search_path(ClawtPodBridge *self);

G_END_DECLS
