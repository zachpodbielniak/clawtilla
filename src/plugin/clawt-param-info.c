/*
 * clawt-param-info.c - Describing a tool's parameters once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "plugin/clawt-param-info.h"

G_DEFINE_BOXED_TYPE(ClawtParamInfo, clawt_param_info,
                    clawt_param_info_copy, clawt_param_info_free)

ClawtParamInfo *
clawt_param_info_copy(ClawtParamInfo *self)
{
    ClawtParamInfo *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtParamInfo, 1);
    *copy = *self;

    return copy;
}

void
clawt_param_info_free(ClawtParamInfo *self)
{
    /*
     * The strings are static literals from the tool tables, not owned
     * copies, so only the struct itself is freed.
     */
    g_free(self);
}

JsonNode *
clawt_param_info_to_schema(const ClawtParamInfo *params, gsize n_params)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    gsize i;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");

    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    for (i = 0; i < n_params; i++) {
        json_builder_set_member_name(builder, params[i].name);
        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder,
                                      params[i].type_name != NULL
                                      ? params[i].type_name : "string");

        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder,
                                      params[i].description != NULL
                                      ? params[i].description : "");

        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);

    /*
     * "required" is emitted even when empty.  A model reading a schema with
     * the key missing has to guess whether nothing is required or the
     * schema is incomplete, and guesses wrong in both directions.
     */
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);

    for (i = 0; i < n_params; i++) {
        if (params[i].required)
            json_builder_add_string_value(builder, params[i].name);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}
