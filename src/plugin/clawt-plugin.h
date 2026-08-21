/*
 * clawt-plugin.h - What a plugin is
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
#include <gmodule.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * CLAWT_PLUGIN_ABI_VERSION:
 *
 * The plugin ABI this build speaks.
 *
 * A plugin exports `clawt_plugin_abi_version` and the manager checks it
 * before instantiating anything.  Loading a plugin built against a
 * different vtable layout does not fail cleanly -- it calls through
 * function pointers at the wrong offsets -- so the check happens before
 * the first call rather than after the first crash.
 */
#define CLAWT_PLUGIN_ABI_VERSION 1

/**
 * CLAWT_PLUGIN_REGISTER_SYMBOL:
 *
 * The entry point every plugin exports.
 */
#define CLAWT_PLUGIN_REGISTER_SYMBOL "clawt_plugin_register"

/**
 * CLAWT_PLUGIN_ABI_SYMBOL:
 *
 * The ABI version symbol every plugin exports.
 */
#define CLAWT_PLUGIN_ABI_SYMBOL "clawt_plugin_abi_version"

#define CLAWT_TYPE_PLUGIN (clawt_plugin_get_type())

G_DECLARE_DERIVABLE_TYPE(ClawtPlugin, clawt_plugin, CLAWT, PLUGIN, GObject)

/**
 * ClawtPluginClass:
 * @get_name: a human-readable name
 * @get_version: the plugin's own version
 * @get_description: one line saying what it is for
 * @configure: hand it its `plugins.<id>` settings
 * @activate: start doing whatever it does
 * @deactivate: stop
 * @create_instance: build an object of a named kind, for providers
 *
 * The vtable a plugin implements.
 */
struct _ClawtPluginClass {
    GObjectClass parent_class;

    const gchar *(*get_name)        (ClawtPlugin *self);
    const gchar *(*get_version)     (ClawtPlugin *self);
    const gchar *(*get_description) (ClawtPlugin *self);

    gboolean     (*configure)       (ClawtPlugin  *self,
                                     GHashTable   *settings,
                                     GError      **error);
    gboolean     (*activate)        (ClawtPlugin  *self,
                                     GError      **error);
    void         (*deactivate)      (ClawtPlugin  *self);

    GObject     *(*create_instance) (ClawtPlugin  *self,
                                     const gchar  *kind,
                                     GHashTable   *params,
                                     GError      **error);

    /*< private >*/
    gpointer _padding[8];
};

const gchar *clawt_plugin_get_name(ClawtPlugin *self);
const gchar *clawt_plugin_get_version(ClawtPlugin *self);
const gchar *clawt_plugin_get_description(ClawtPlugin *self);

gboolean clawt_plugin_configure(ClawtPlugin  *self,
                                GHashTable   *settings,
                                GError      **error);
gboolean clawt_plugin_activate(ClawtPlugin *self, GError **error);
void     clawt_plugin_deactivate(ClawtPlugin *self);
gboolean clawt_plugin_is_active(ClawtPlugin *self);

/**
 * clawt_plugin_create_instance:
 * @self: a #ClawtPlugin
 * @kind: what to build, e.g. a computer backend name
 * @params: (element-type utf8 utf8) (nullable): construction parameters
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the new object, or %NULL
 */
GObject *clawt_plugin_create_instance(ClawtPlugin  *self,
                                      const gchar  *kind,
                                      GHashTable   *params,
                                      GError      **error);

/**
 * clawt_plugin_get_id:
 * @self: a #ClawtPlugin
 *
 * The id taken from the plugin's filename, which is what
 * `plugins.<id>` and `plugins.disabled` refer to.
 *
 * Returns: (transfer none) (nullable): the id
 */
const gchar *clawt_plugin_get_id(ClawtPlugin *self);

/**
 * clawt_plugin_set_id:
 * @self: a #ClawtPlugin
 * @id: the plugin's id
 *
 * For the manager, which knows the filename the plugin came from.
 */
void clawt_plugin_set_id(ClawtPlugin *self, const gchar *id);

/**
 * clawt_plugin_get_services:
 * @self: a #ClawtPlugin
 *
 * The service locator: how a plugin reaches the daemon's components at
 * activate time, without every plugin needing a different constructor.
 *
 * Returns: (transfer none) (nullable) (element-type utf8 GObject): the
 *   services, keyed by name
 */
GHashTable *clawt_plugin_get_services(ClawtPlugin *self);

void clawt_plugin_set_services(ClawtPlugin *self, GHashTable *services);

G_END_DECLS
