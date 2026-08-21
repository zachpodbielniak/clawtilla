/*
 * clawt-plugin.c - What a plugin is
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "plugin/clawt-plugin.h"

typedef struct {
    gchar      *id;
    GHashTable *services;   /* unowned; the daemon outlives its plugins */
    gboolean    active;
} ClawtPluginPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(ClawtPlugin, clawt_plugin, G_TYPE_OBJECT)

#define PRIV(self) \
    ((ClawtPluginPrivate *)clawt_plugin_get_instance_private(self))

const gchar *
clawt_plugin_get_name(ClawtPlugin *self)
{
    ClawtPluginClass *klass;

    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), NULL);

    klass = CLAWT_PLUGIN_GET_CLASS(self);

    if (klass->get_name != NULL)
        return klass->get_name(self);

    /* Falls back to the id rather than NULL: a plugin without a name is
     * still worth listing, and "(null)" in a plugin list helps nobody. */
    return PRIV(self)->id;
}

const gchar *
clawt_plugin_get_version(ClawtPlugin *self)
{
    ClawtPluginClass *klass;

    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), NULL);

    klass = CLAWT_PLUGIN_GET_CLASS(self);

    return (klass->get_version != NULL) ? klass->get_version(self)
                                        : "unknown";
}

const gchar *
clawt_plugin_get_description(ClawtPlugin *self)
{
    ClawtPluginClass *klass;

    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), NULL);

    klass = CLAWT_PLUGIN_GET_CLASS(self);

    return (klass->get_description != NULL) ? klass->get_description(self)
                                            : NULL;
}

gboolean
clawt_plugin_configure(ClawtPlugin *self, GHashTable *settings,
                       GError **error)
{
    ClawtPluginClass *klass;

    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), FALSE);

    klass = CLAWT_PLUGIN_GET_CLASS(self);

    if (klass->configure == NULL)
        return TRUE;

    return klass->configure(self, settings, error);
}

gboolean
clawt_plugin_activate(ClawtPlugin *self, GError **error)
{
    ClawtPluginClass *klass;
    ClawtPluginPrivate *priv;

    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), FALSE);

    priv = PRIV(self);

    if (priv->active)
        return TRUE;

    klass = CLAWT_PLUGIN_GET_CLASS(self);

    if (klass->activate != NULL && !klass->activate(self, error))
        return FALSE;

    priv->active = TRUE;

    return TRUE;
}

void
clawt_plugin_deactivate(ClawtPlugin *self)
{
    ClawtPluginClass *klass;
    ClawtPluginPrivate *priv;

    g_return_if_fail(CLAWT_IS_PLUGIN(self));

    priv = PRIV(self);

    if (!priv->active)
        return;

    klass = CLAWT_PLUGIN_GET_CLASS(self);

    if (klass->deactivate != NULL)
        klass->deactivate(self);

    priv->active = FALSE;
}

gboolean
clawt_plugin_is_active(ClawtPlugin *self)
{
    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), FALSE);

    return PRIV(self)->active;
}

GObject *
clawt_plugin_create_instance(ClawtPlugin *self, const gchar *kind,
                             GHashTable *params, GError **error)
{
    ClawtPluginClass *klass;

    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), NULL);

    klass = CLAWT_PLUGIN_GET_CLASS(self);

    if (klass->create_instance == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "the %s plugin does not build things",
                    clawt_plugin_get_name(self));
        return NULL;
    }

    return klass->create_instance(self, kind, params, error);
}

const gchar *
clawt_plugin_get_id(ClawtPlugin *self)
{
    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), NULL);

    return PRIV(self)->id;
}

void
clawt_plugin_set_id(ClawtPlugin *self, const gchar *id)
{
    ClawtPluginPrivate *priv;

    g_return_if_fail(CLAWT_IS_PLUGIN(self));

    priv = PRIV(self);
    g_free(priv->id);
    priv->id = g_strdup(id);
}

GHashTable *
clawt_plugin_get_services(ClawtPlugin *self)
{
    g_return_val_if_fail(CLAWT_IS_PLUGIN(self), NULL);

    return PRIV(self)->services;
}

void
clawt_plugin_set_services(ClawtPlugin *self, GHashTable *services)
{
    g_return_if_fail(CLAWT_IS_PLUGIN(self));

    PRIV(self)->services = services;
}

static void
clawt_plugin_finalize(GObject *object)
{
    ClawtPluginPrivate *priv = PRIV(CLAWT_PLUGIN(object));

    g_free(priv->id);

    G_OBJECT_CLASS(clawt_plugin_parent_class)->finalize(object);
}

static void
clawt_plugin_class_init(ClawtPluginClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_plugin_finalize;
}

static void
clawt_plugin_init(ClawtPlugin *self)
{
    PRIV(self)->active = FALSE;
}
