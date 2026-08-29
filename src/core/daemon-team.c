/*
 * daemon-team.c - The client surface: team.*
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
clawt_daemon_handle_team(
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

    /* ── teams ── */
    if (g_strcmp0(kind, "team.list") == 0) {
        g_autoptr(GPtrArray) teams = clawt_config_get_teams(self->config);
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        g_auto(GStrv) warnings = NULL;
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "teams");
        json_builder_begin_array(builder);

        for (i = 0; i < teams->len; i++) {
            ClawtTeamSpec *team = g_ptr_array_index(teams, i);
            guint running = 0;
            guint total = 0;
            const gchar *lead = NULL;
            guint j;

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "id", team->id);
            clawt_daemon_add_string_member(
                builder, "name",
                team->name != NULL ? team->name : team->id);
            clawt_daemon_add_string_member(builder, "description",
                                           team->description);
            clawt_daemon_add_string_member(builder, "color", team->color);

            json_builder_set_member_name(builder, "order");
            json_builder_add_int_value(builder, team->order);

            json_builder_set_member_name(builder, "members");
            json_builder_begin_array(builder);

            for (j = 0; agents != NULL && j < agents->len; j++) {
                ClawtAgent *agent = g_ptr_array_index(agents, j);
                ClawtAgentConfig *config = clawt_agent_get_config(agent);

                if (g_strcmp0(clawt_agent_config_get_string(config, "team"),
                              team->id) != 0)
                    continue;

                json_builder_add_string_value(builder,
                                              clawt_agent_get_id(agent));
                total++;

                if (clawt_agent_get_state(agent) ==
                    CLAWT_AGENT_STATE_RUNNING)
                    running++;

                if (clawt_team_role_of(config) == CLAWT_TEAM_LEAD)
                    lead = clawt_agent_get_id(agent);
            }

            json_builder_end_array(builder);

            clawt_daemon_add_string_member(builder, "lead", lead);

            /*
             * Counted here rather than in each client. Three clients
             * counting the same thing is three chances to disagree about
             * what "active" means.
             */
            json_builder_set_member_name(builder, "running");
            json_builder_add_int_value(builder, running);
            json_builder_set_member_name(builder, "total");
            json_builder_add_int_value(builder, total);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        /*
         * What only the whole fleet can show: two leads on one team, an
         * agent naming a team nobody declared. Reported rather than
         * enforced, because a fleet is edited by hand and half-built
         * states are ordinary.
         */
        clawt_team_validate_fleet(self->config, &warnings);

        json_builder_set_member_name(builder, "warnings");
        json_builder_begin_array(builder);

        for (i = 0; warnings != NULL && warnings[i] != NULL; i++)
            json_builder_add_string_value(builder, warnings[i]);

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "team.create") == 0) {
        const gchar *team_id = clawt_ipc_payload_string(payload, "id");

        if (team_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a team needs an id");

        if (!clawt_config_add_team(self->config, team_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        {
            static const gchar *const fields[] = {
                "name", "description", "color", NULL
            };
            gsize i;

            for (i = 0; fields[i] != NULL; i++) {
                const gchar *value = clawt_ipc_payload_string(payload,
                                                              fields[i]);

                if (value != NULL)
                    clawt_config_set_team_string(self->config, team_id,
                                                 fields[i], value);
            }
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "team.changed", team_id);

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "id", team_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "team.set") == 0) {
        const gchar *team_id = clawt_ipc_payload_string(payload, "team");
        const gchar *key = clawt_ipc_payload_string(payload, "key");
        const gchar *value = clawt_ipc_payload_string(payload, "value");

        if (team_id == NULL || key == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "team and key are both required");

        if (!clawt_config_set_team_string(self->config, team_id, key, value))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_FOUND,
                g_strcmp0(key, "id") == 0
                    ? "a team's id cannot be changed: everything refers to "
                      "it by that. Create the new one, move the agents, "
                      "remove the old."
                    : "no such team");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The agents' own files describe their team, so they are
         * rewritten here for the same reason agent.set rewrites them:
         * a description that changed and did not reach the prompt is a
         * second answer to what the team is for.
         */
        {
            g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "team.changed", team_id);

            json_builder_begin_object(builder);
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "team.remove") == 0) {
        const gchar *team_id = clawt_ipc_payload_string(payload, "team");

        if (team_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which team?");

        if (!clawt_config_remove_team(self->config, team_id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such team");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The agents that were on it are left naming a team that is no
         * longer declared, which is a state they are allowed to be in --
         * and saying how many is more use than silently reassigning
         * them somewhere nobody chose.
         */
        {
            GPtrArray *agents;
            g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();
            guint orphaned = 0;
            guint i;

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "team.changed", team_id);

            agents = clawt_agent_manager_list(self->agents);

            for (i = 0; agents != NULL && i < agents->len; i++) {
                ClawtAgent *agent = g_ptr_array_index(agents, i);

                if (g_strcmp0(clawt_agent_config_get_string(
                                  clawt_agent_get_config(agent), "team"),
                              team_id) == 0)
                    orphaned++;
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "orphaned");
            json_builder_add_int_value(builder, orphaned);
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
