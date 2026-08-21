/*
 * clawt-integration-provider.h - Adding a way for agents to reach the world
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

G_BEGIN_DECLS

#define CLAWT_TYPE_INTEGRATION_PROVIDER \
    (clawt_integration_provider_get_type())

G_DECLARE_INTERFACE(ClawtIntegrationProvider, clawt_integration_provider,
                    CLAWT, INTEGRATION_PROVIDER, GObject)

/**
 * ClawtIntegrationProviderInterface:
 * @get_id: the `integrations.<id>` key this provider answers to
 * @validate: check an agent's settings for this integration
 * @render_config: the YAML to splice into the agent's libreclaw config
 *
 * Implemented by a plugin that adds an integration type.
 */
struct _ClawtIntegrationProviderInterface {
    GTypeInterface parent_iface;

    const gchar *(*get_id)        (ClawtIntegrationProvider *self);
    gboolean     (*validate)      (ClawtIntegrationProvider  *self,
                                   ClawtAgentConfig          *config,
                                   GError                   **error);
    gchar       *(*render_config) (ClawtIntegrationProvider  *self,
                                   ClawtAgentConfig          *config,
                                   const gchar               *state_dir,
                                   GError                   **error);
};

const gchar *clawt_integration_provider_get_id(
    ClawtIntegrationProvider *self);

gboolean clawt_integration_provider_validate(
    ClawtIntegrationProvider  *self,
    ClawtAgentConfig          *config,
    GError                   **error);

/**
 * clawt_integration_provider_render_config:
 * @self: a #ClawtIntegrationProvider
 * @config: the agent's configuration
 * @state_dir: where this agent's credentials live
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): YAML for the agent's `channels:`
 *   block, indented for two-space nesting
 */
gchar *clawt_integration_provider_render_config(
    ClawtIntegrationProvider  *self,
    ClawtAgentConfig          *config,
    const gchar               *state_dir,
    GError                   **error);

G_END_DECLS
