/*
 * daemon-config.c - The client surface: config.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

JsonNode *
clawt_daemon_handle_config(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /* ── config ── */
    if (g_strcmp0(kind, "config.show") == 0) {
        g_autofree gchar *text = clawt_config_to_string(self->config);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "yaml");
        json_builder_add_string_value(builder, text);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "config.render") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *config = (agent_id != NULL)
                                   ? clawt_config_get_agent(self->config,
                                                            agent_id)
                                   : NULL;
        g_autofree gchar *rendered = NULL;
        g_autofree gchar *state_dir = NULL;

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        state_dir = clawt_config_agent_state_dir(self->config, agent_id);
        rendered = clawt_config_render_agent(self->config, config,
                                             self->link_socket, state_dir,
                                             &error);

        if (rendered == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "yaml");
        json_builder_add_string_value(builder, rendered);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "config.validate") == 0) {
        GPtrArray *warnings;
        guint i;

        clawt_config_validate(self->config, &error);
        warnings = clawt_config_get_warnings(self->config);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "valid");
        json_builder_add_boolean_value(builder, error == NULL);

        if (error != NULL) {
            json_builder_set_member_name(builder, "error");
            json_builder_add_string_value(builder, error->message);
        }

        json_builder_set_member_name(builder, "warnings");
        json_builder_begin_array(builder);

        for (i = 0; warnings != NULL && i < warnings->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(warnings, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
