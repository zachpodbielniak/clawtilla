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

            /*
             * The team's container limits as written, short keys ("cpus",
             * "memory"), only those it sets: what a client's team editor
             * shows and what members without their own value inherit.
             */
            {
                guint k;

                json_builder_set_member_name(builder, "container_limits");
                json_builder_begin_object(builder);

                for (k = 0; k < CLAWT_CONTAINER_LIMIT_KEYS; k++) {
                    const gchar *key = clawt_container_limit_key_nth(k);
                    const gchar *value = clawt_config_get_team_container_limit(
                        self->config, team->id, key);

                    if (value == NULL)
                        continue;

                    json_builder_set_member_name(
                        builder, key + strlen("computer.container."));
                    json_builder_add_string_value(builder, value);
                }

                json_builder_end_object(builder);
            }

            /* as written; clawt_team_tag_lookup() says which are known */
            json_builder_set_member_name(builder, "tags");
            json_builder_begin_array(builder);
            for (j = 0; team->tags != NULL && team->tags[j] != NULL; j++)
                json_builder_add_string_value(builder, team->tags[j]);
            json_builder_end_array(builder);

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

        /*
         * And the SELinux trap, which is a fleet-shaped problem too: two
         * containers on one shared folder at different levels.  Here
         * because this is the list the Fleet page shows, and with the
         * stored team pairs, so a team and an explicit level that turn
         * out to be the same pair are not reported as different.
         */
        {
            g_auto(GStrv) labelling = clawt_daemon_selinux_warnings(self);

            for (i = 0; labelling != NULL && labelling[i] != NULL; i++)
                json_builder_add_string_value(builder, labelling[i]);
        }

        /*
         * And a kept container that was removed and made again because
         * it no longer matched its config (VENTURE #21) -- the same list,
         * because whoever finds their agent's packages gone looks here.
         */
        {
            g_auto(GStrv) recreated =
                clawt_daemon_container_recreate_notes(self);
            g_auto(GStrv) limits = clawt_daemon_container_limit_notes(self);

            for (i = 0; recreated != NULL && recreated[i] != NULL; i++)
                json_builder_add_string_value(builder, recreated[i]);

            /*
             * And the container limits: one refused, one waiting for a
             * restart, a machine at its memory limit.
             */
            for (i = 0; limits != NULL && limits[i] != NULL; i++)
                json_builder_add_string_value(builder, limits[i]);
        }

        /*
         * And what the VENTURE mirror needs fixed (VENTURE #30): a token
         * that cannot reach its tickets' organization, a write VENTURE's
         * role gate refused.  The tasks are kept; this is where the
         * operator learns their tickets have stopped moving.
         */
        if (self->venture_mirror != NULL) {
            g_auto(GStrv) mirror =
                clawt_venture_mirror_warnings(self->venture_mirror);

            for (i = 0; mirror != NULL && mirror[i] != NULL; i++)
                json_builder_add_string_value(builder, mirror[i]);
        }

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

        /* tags (0.55.0), refused here when malformed rather than warned later */
        {
            g_auto(GStrv) tags = clawt_ipc_payload_strv(payload, "tags");
            gsize i;

            for (i = 0; tags != NULL && tags[i] != NULL; i++)
                if (!clawt_team_tag_is_valid(tags[i])) {
                    clawt_config_remove_team(self->config, team_id);
                    return clawt_ipc_error_new(request,
                        CLAWT_ERROR_INVALID_ARGUMENT,
                        "a tag is lowercase letters, digits and hyphens, "
                        "with at most one vendor: prefix");
                }
            if (tags != NULL && tags[0] != NULL)
                clawt_config_set_team_string_list(self->config, team_id, "tags",
                                                  (const gchar *const *)tags);
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

        if (!clawt_broker_check_setting(self->config, NULL, team_id, key,
                                        value, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * A container limit for the whole team, checked as the team's set
         * will be with this value in place -- refused here, naming the
         * key, rather than warned about on every member's next start.
         */
        if (clawt_container_limit_key_is(key)) {
            g_autoptr(ClawtContainerLimits) limits =
                clawt_container_limits_new();
            guint k;

            for (k = 0; k < CLAWT_CONTAINER_LIMIT_KEYS; k++) {
                const gchar *other = clawt_container_limit_key_nth(k);
                const gchar *now = (g_strcmp0(other, key) == 0)
                    ? value
                    : clawt_config_get_team_container_limit(self->config,
                                                            team_id, other);

                if (!clawt_container_limits_set_key(limits, other, now,
                                                    &error))
                    return clawt_ipc_error_new(request,
                                               CLAWT_ERROR_INVALID_ARGUMENT,
                                               error->message);
            }

            if (!clawt_container_limits_validate_level(
                    limits, clawt_host_cpu_count(), &error)) {
                g_autofree gchar *said = g_strdup_printf(
                    "team %s: %s", team_id, error->message);

                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT, said);
            }
        }

        /*
         * Dispatched on what the schema says the key is.  A list written
         * as a scalar is read back as nothing, so `skills` set through
         * here reached no agent; a list arrives as `values` (an array),
         * or as `value` split on commas for a client that sends strings.
         */
        {
            g_autofree gchar *schema_key = g_strdup_printf("teams.%s", key);
            const ClawtSchemaEntry *entry = clawt_config_schema_lookup(schema_key);

            if (entry != NULL && entry->type == CLAWT_SCHEMA_STRING_LIST) {
                g_auto(GStrv) values = clawt_ipc_payload_strv(payload, "values");
                gsize i;

                if (values == NULL && value != NULL && *value != '\0') {
                    values = g_strsplit(value, ",", -1);
                    for (i = 0; values[i] != NULL; i++)
                        g_strstrip(values[i]);
                }
                for (i = 0; g_strcmp0(key, "tags") == 0 &&
                            values != NULL && values[i] != NULL; i++)
                    if (!clawt_team_tag_is_valid(values[i]))
                        return clawt_ipc_error_new(request,
                            CLAWT_ERROR_INVALID_ARGUMENT,
                            "a tag is lowercase letters, digits and hyphens, "
                            "with at most one vendor: prefix");

                if (!clawt_config_set_team_string_list(
                        self->config, team_id, key, (const gchar *const *)values))
                    return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                               "no such team");
                goto team_saved;
            }
        }

        if (!clawt_config_set_team_string(self->config, team_id, key, value))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_FOUND,
                g_strcmp0(key, "id") == 0
                    ? "a team's id cannot be changed: everything refers to "
                      "it by that. Create the new one, move the agents, "
                      "remove the old."
                    : "no such team");

team_saved:
        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /* A team's broker_server decides its members' broker connector. */
        clawt_config_refresh_broker_connectors(self->config);

        /*
         * A team limit reaches its running members now, in place, as an
         * agent's own does through agent.set.
         */
        if (clawt_container_limit_key_is(key))
            clawt_daemon_apply_limits_everywhere(self);

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

    /* ── accounts ── */
    if (g_strcmp0(kind, "account.add") == 0) {
        if (!clawt_config_add_account(self->config,
                                      clawt_ipc_payload_string(payload, "id"),
                                      clawt_ipc_payload_string(payload, "provider"),
                                      clawt_ipc_payload_string(payload, "config_dir"),
                                      clawt_ipc_payload_string(payload, "description"),
                                      &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "account.changed",
                             clawt_ipc_payload_string(payload, "id"));
        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "id",
                                       clawt_ipc_payload_string(payload, "id"));
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "account.remove") == 0) {
        const gchar *account_id = clawt_ipc_payload_string(payload, "id");
        g_autoptr(GPtrArray) refusals = NULL;
        GPtrArray *agents;
        guint using = 0;
        guint i;

        if (account_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which account?");
        if (!clawt_config_remove_account(self->config, account_id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such account");
        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Agents still naming it are revalidated now and become shadows
         * with the reason, rather than starting next time on the default
         * login.  How many is in the reply, because that is the question
         * somebody removing an account is actually holding.
         */
        agents = clawt_agent_manager_list(self->agents);
        for (i = 0; agents != NULL && i < agents->len; i++) {
            ClawtAgent *agent = g_ptr_array_index(agents, i);
            ClawtAgentConfig *config = clawt_agent_get_config(agent);

            if (g_strcmp0(clawt_agent_config_get_string(config, "model.account"),
                          account_id) == 0) {
                using++;
                clawt_agent_config_revalidate(config);
                clawt_agent_revalidate(agent);
            }
        }

        refusals = clawt_daemon_render_refusals_new();
        clawt_daemon_render_all_agents_into(self, refusals);
        clawt_event_bus_emit(self->bus, "account.changed", account_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agents_using");
        json_builder_add_int_value(builder, using);
        clawt_daemon_add_render_refusals(builder, refusals);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "account.list") == 0) {
        g_autoptr(GPtrArray) accounts = clawt_config_get_accounts(self->config);
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *agent = agent_id != NULL
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "accounts");
        json_builder_begin_array(builder);

        for (i = 0; i < accounts->len; i++) {
            ClawtAccountSpec *account = g_ptr_array_index(accounts, i);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "id", account->id);
            clawt_daemon_add_string_member(builder, "provider", account->provider);
            clawt_daemon_add_string_member(builder, "config_dir", account->config_dir);
            clawt_daemon_add_string_member(builder, "description", account->description);

            /*
             * With an agent named, say whether it can log in as this one,
             * and why not -- so a picker offers only what the daemon will
             * accept, decided by the one function that decides it.
             */
            if (agent != NULL) {
                g_autoptr(GError) why = NULL;
                gboolean usable = clawt_account_check(
                    account, clawt_agent_config_get_string(agent, "model.provider"), &why);

                json_builder_set_member_name(builder, "usable");
                json_builder_add_boolean_value(builder, usable);
                clawt_daemon_add_string_member(builder, "reason",
                                               why != NULL ? why->message : NULL);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
