/*
 * clawt-tool-provider.h - Adding tools agents can call
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
#include <json-glib/json-glib.h>

#include "clawt-types.h"
#include "plugin/clawt-param-info.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TOOL_PROVIDER (clawt_tool_provider_get_type())

G_DECLARE_INTERFACE(ClawtToolProvider, clawt_tool_provider, CLAWT,
                    TOOL_PROVIDER, GObject)

/**
 * ClawtToolProviderInterface:
 * @list_tools: the tools this provider offers
 * @get_params: the parameters one tool takes
 * @call: run a tool
 *
 * Implemented by a plugin that adds tools to what agents can do.
 */
struct _ClawtToolProviderInterface {
    GTypeInterface parent_iface;

    GStrv                 (*list_tools) (ClawtToolProvider *self);
    const ClawtParamInfo *(*get_params) (ClawtToolProvider *self,
                                         const gchar       *tool_name,
                                         gsize             *n_params);
    gchar                *(*call)       (ClawtToolProvider  *self,
                                         const gchar        *agent_id,
                                         const gchar        *tool_name,
                                         JsonObject         *arguments,
                                         GError            **error);
};

/**
 * clawt_tool_provider_list_tools:
 * @self: a #ClawtToolProvider
 *
 * Returns: (transfer full) (array zero-terminated=1): the tool names
 */
GStrv clawt_tool_provider_list_tools(ClawtToolProvider *self);

/**
 * clawt_tool_provider_get_params:
 * @self: a #ClawtToolProvider
 * @tool_name: which tool
 * @n_params: (out): how many parameters
 *
 * The schema is generated from this rather than hand-written, so a
 * provider cannot describe its tool one way and accept another.
 *
 * Returns: (transfer none) (array length=n_params): the parameters
 */
const ClawtParamInfo *clawt_tool_provider_get_params(
    ClawtToolProvider *self,
    const gchar       *tool_name,
    gsize             *n_params);

/**
 * clawt_tool_provider_call:
 * @self: a #ClawtToolProvider
 * @agent_id: who is calling
 * @tool_name: which tool
 * @arguments: (nullable): what they passed
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): what to tell the agent
 */
gchar *clawt_tool_provider_call(ClawtToolProvider  *self,
                                const gchar        *agent_id,
                                const gchar        *tool_name,
                                JsonObject         *arguments,
                                GError            **error);

G_END_DECLS
