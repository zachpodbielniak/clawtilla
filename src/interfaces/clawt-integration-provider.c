/*
 * clawt-integration-provider.c - Adding a way for agents to reach the world
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "interfaces/clawt-integration-provider.h"

G_DEFINE_INTERFACE(ClawtIntegrationProvider, clawt_integration_provider,
                   G_TYPE_OBJECT)

static void
clawt_integration_provider_default_init(
    ClawtIntegrationProviderInterface *iface)
{
    (void)iface;
}

const gchar *
clawt_integration_provider_get_id(ClawtIntegrationProvider *self)
{
    ClawtIntegrationProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_INTEGRATION_PROVIDER(self), NULL);

    iface = CLAWT_INTEGRATION_PROVIDER_GET_IFACE(self);

    return (iface->get_id != NULL) ? iface->get_id(self) : NULL;
}

gboolean
clawt_integration_provider_validate(ClawtIntegrationProvider *self,
                                    ClawtAgentConfig *config, GError **error)
{
    ClawtIntegrationProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_INTEGRATION_PROVIDER(self), FALSE);

    iface = CLAWT_INTEGRATION_PROVIDER_GET_IFACE(self);

    if (iface->validate == NULL)
        return TRUE;

    return iface->validate(self, config, error);
}

gchar *
clawt_integration_provider_render_config(ClawtIntegrationProvider *self,
                                         ClawtAgentConfig *config,
                                         const gchar *state_dir,
                                         GError **error)
{
    ClawtIntegrationProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_INTEGRATION_PROVIDER(self), NULL);

    iface = CLAWT_INTEGRATION_PROVIDER_GET_IFACE(self);

    if (iface->render_config == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "the '%s' integration cannot be rendered",
                    clawt_integration_provider_get_id(self));
        return NULL;
    }

    return iface->render_config(self, config, state_dir, error);
}
