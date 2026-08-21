/*
 * clawt-param-info.h - Describing a tool's parameters once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A tool's parameters are described once and rendered wherever they are
 * needed: as an MCP JSON schema for agents, as ai-glib tool definitions for
 * the agent designer, and as documentation.
 *
 * The alternative -- writing the schema by hand next to the implementation
 * -- means the description and the code drift, and a tool whose schema
 * lies about its arguments is worse than one with no schema at all.
 * podomation's modules already carry this shape, so a plugin's actions can
 * become agent tools without being described twice.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_PARAM_INFO (clawt_param_info_get_type())

GType clawt_param_info_get_type(void) G_GNUC_CONST;

/**
 * ClawtParamInfo:
 * @name: the parameter's name
 * @type_name: "string", "integer", "boolean" or "number"
 * @description: what it is for
 * @required: whether the tool cannot run without it
 */
struct _ClawtParamInfo {
    const gchar *name;
    const gchar *type_name;
    const gchar *description;
    gboolean     required;
};

ClawtParamInfo *clawt_param_info_copy(ClawtParamInfo *self);
void            clawt_param_info_free(ClawtParamInfo *self);

/**
 * clawt_param_info_to_schema:
 * @params: (array length=n_params): the parameters
 * @n_params: how many
 *
 * Renders parameters as a JSON Schema object, as MCP expects.
 *
 * Returns: (transfer full): the schema
 */
JsonNode *clawt_param_info_to_schema(const ClawtParamInfo *params,
                                     gsize                 n_params);

G_END_DECLS
