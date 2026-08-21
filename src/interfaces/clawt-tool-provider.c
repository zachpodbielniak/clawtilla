/*
 * clawt-tool-provider.c - Adding tools agents can call
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "interfaces/clawt-tool-provider.h"

G_DEFINE_INTERFACE(ClawtToolProvider, clawt_tool_provider, G_TYPE_OBJECT)

static void
clawt_tool_provider_default_init(ClawtToolProviderInterface *iface)
{
    (void)iface;
}

GStrv
clawt_tool_provider_list_tools(ClawtToolProvider *self)
{
    ClawtToolProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_TOOL_PROVIDER(self), NULL);

    iface = CLAWT_TOOL_PROVIDER_GET_IFACE(self);

    if (iface->list_tools == NULL)
        return NULL;

    return iface->list_tools(self);
}

const ClawtParamInfo *
clawt_tool_provider_get_params(ClawtToolProvider *self,
                               const gchar *tool_name, gsize *n_params)
{
    ClawtToolProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_TOOL_PROVIDER(self), NULL);
    g_return_val_if_fail(n_params != NULL, NULL);

    *n_params = 0;
    iface = CLAWT_TOOL_PROVIDER_GET_IFACE(self);

    if (iface->get_params == NULL)
        return NULL;

    return iface->get_params(self, tool_name, n_params);
}

gchar *
clawt_tool_provider_call(ClawtToolProvider *self, const gchar *agent_id,
                         const gchar *tool_name, JsonObject *arguments,
                         GError **error)
{
    ClawtToolProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_TOOL_PROVIDER(self), NULL);

    iface = CLAWT_TOOL_PROVIDER_GET_IFACE(self);

    if (iface->call == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "this provider lists %s but cannot run it", tool_name);
        return NULL;
    }

    return iface->call(self, agent_id, tool_name, arguments, error);
}
