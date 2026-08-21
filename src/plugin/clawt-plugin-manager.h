/*
 * clawt-plugin-manager.h - Finding, loading and running plugins
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
#include "config/clawt-config.h"
#include "core/clawt-event-bus.h"
#include "plugin/clawt-plugin.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_PLUGIN_MANAGER (clawt_plugin_manager_get_type())

G_DECLARE_FINAL_TYPE(ClawtPluginManager, clawt_plugin_manager, CLAWT,
                     PLUGIN_MANAGER, GObject)

/**
 * clawt_plugin_manager_new:
 * @config: (transfer none) (nullable): the fleet configuration
 *
 * Returns: (transfer full): a new #ClawtPluginManager
 */
ClawtPluginManager *clawt_plugin_manager_new(ClawtConfig *config);

/**
 * clawt_plugin_manager_add_service:
 * @self: a #ClawtPluginManager
 * @name: what to call it, e.g. "agents"
 * @service: (transfer none): the object
 *
 * Adds a component plugins can reach at activate time.
 *
 * A locator rather than a constructor argument per component, so adding a
 * new daemon component does not change every plugin's signature.
 */
void clawt_plugin_manager_add_service(ClawtPluginManager *self,
                                      const gchar        *name,
                                      GObject            *service);

/**
 * clawt_plugin_manager_load_all:
 * @self: a #ClawtPluginManager
 *
 * Searches every plugin directory and loads what it finds.
 *
 * Search order: $CLAWT_PLUGIN_PATH, then `plugins.dirs`, then
 * $XDG_CONFIG_HOME/clawtilla/plugins, then the compiled-in system
 * directory.  A plugin that fails to load disables itself and nothing
 * else.
 *
 * Returns: how many plugins were activated
 */
guint clawt_plugin_manager_load_all(ClawtPluginManager *self);

/**
 * clawt_plugin_manager_load_file:
 * @self: a #ClawtPluginManager
 * @path: the .so to load
 * @error: (out) (optional): return location for a #GError
 *
 * Loads one plugin.
 *
 * Returns: (transfer none) (nullable): the plugin, or %NULL
 */
ClawtPlugin *clawt_plugin_manager_load_file(ClawtPluginManager  *self,
                                            const gchar         *path,
                                            GError             **error);

/**
 * clawt_plugin_manager_list:
 * @self: a #ClawtPluginManager
 *
 * Returns: (transfer container) (element-type ClawtPlugin): the loaded
 *   plugins
 */
GPtrArray *clawt_plugin_manager_list(ClawtPluginManager *self);

/**
 * clawt_plugin_manager_get:
 * @self: a #ClawtPluginManager
 * @plugin_id: the id, from the filename
 *
 * Returns: (transfer none) (nullable): the plugin, or %NULL
 */
ClawtPlugin *clawt_plugin_manager_get(ClawtPluginManager *self,
                                      const gchar        *plugin_id);

/**
 * clawt_plugin_manager_attach_bus:
 * @self: a #ClawtPluginManager
 * @bus: (transfer none): the event bus
 *
 * Feeds fleet events to every plugin that implements #ClawtEventHandler.
 */
void clawt_plugin_manager_attach_bus(ClawtPluginManager *self,
                                     ClawtEventBus      *bus);

/**
 * clawt_plugin_manager_find_computer_provider:
 * @self: a #ClawtPluginManager
 * @type_name: a `computer.type` value the core does not recognise
 *
 * Returns: (transfer none) (nullable): a provider for it, or %NULL
 */
GObject *clawt_plugin_manager_find_computer_provider(
    ClawtPluginManager *self,
    const gchar        *type_name);

/**
 * clawt_plugin_manager_tool_providers:
 * @self: a #ClawtPluginManager
 *
 * Returns: (transfer container) (element-type GObject): every loaded
 *   plugin that provides tools
 */
GPtrArray *clawt_plugin_manager_tool_providers(ClawtPluginManager *self);

/**
 * clawt_plugin_manager_unload_all:
 * @self: a #ClawtPluginManager
 *
 * Deactivates every plugin.
 *
 * The modules themselves are never dlclose()d: a plugin's GTypes stay
 * registered for the life of the process, and unmapping the code they
 * point at turns the next type lookup into a jump to freed memory.
 */
void clawt_plugin_manager_unload_all(ClawtPluginManager *self);

G_END_DECLS
