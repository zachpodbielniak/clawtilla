/*
 * refuses-plugin.c - A plugin that declines to start
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A plugin whose activate() fails must disable itself and nothing else:
 * one stale third-party plugin should never keep the daemon down.
 */

#include <clawtilla.h>

G_MODULE_EXPORT const guint clawt_plugin_abi_version =
    CLAWT_PLUGIN_ABI_VERSION;

#define REFUSES_TYPE_PLUGIN (refuses_plugin_get_type())

G_DECLARE_FINAL_TYPE(RefusesPlugin, refuses_plugin, REFUSES, PLUGIN,
                     ClawtPlugin)

struct _RefusesPlugin {
    ClawtPlugin parent_instance;
};

G_DEFINE_FINAL_TYPE(RefusesPlugin, refuses_plugin, CLAWT_TYPE_PLUGIN)

static const gchar *
refuses_plugin_get_name(ClawtPlugin *plugin)
{
    (void)plugin;

    return "Refuses";
}

static gboolean
refuses_plugin_activate(ClawtPlugin *plugin, GError **error)
{
    (void)plugin;

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "this plugin needs something it cannot find");

    return FALSE;
}

static void
refuses_plugin_class_init(RefusesPluginClass *klass)
{
    ClawtPluginClass *plugin_class = CLAWT_PLUGIN_CLASS(klass);

    plugin_class->get_name = refuses_plugin_get_name;
    plugin_class->activate = refuses_plugin_activate;
}

static void
refuses_plugin_init(RefusesPlugin *self)
{
    (void)self;
}

G_MODULE_EXPORT GType
clawt_plugin_register(void)
{
    return REFUSES_TYPE_PLUGIN;
}
