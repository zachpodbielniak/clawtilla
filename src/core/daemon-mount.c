/*
 * daemon-mount.c - The client surface: agent.mount.* and defaults.mount.*
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
clawt_daemon_handle_mount(
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

    if (g_strcmp0(kind, "agent.mount.add") == 0 ||
        g_strcmp0(kind, "agent.mount.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *target = clawt_ipc_payload_string(payload, "target");
        ClawtAgentConfig *agent_config;

        /*
         * Shared folders, settable.
         *
         * computer.mounts has always been read and applied -- bind
         * mounts for a container, virtiofs devices for a VM -- and no
         * client could write one, so the only way to share a folder
         * with an agent was to edit the YAML by hand.
         */
        if (agent_id == NULL || target == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and target are both required");

        agent_config = clawt_config_get_agent(self->config, agent_id);

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (g_strcmp0(kind, "agent.mount.remove") == 0) {
            if (!clawt_agent_config_remove_mount(agent_config, target))
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "that agent has no such mount");
        } else {
            g_autoptr(ClawtMount) mount =
                clawt_daemon_mount_from_payload(self->config, payload, target,
                                                &error);

            if (mount == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            if (!clawt_agent_config_add_mount(agent_config, mount))
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "could not add the mount");
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_agent_manager_load(self->agents, NULL);

        {
            g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "target");
            json_builder_add_string_value(builder, target);
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * The fleet's shared folders.
     *
     * Same three verbs as the per-agent family and the same parser, so
     * a folder added here behaves exactly like one added to an agent.
     * Named `defaults.*` rather than reusing `agent.mount.*` with the
     * agent omitted, because a frame whose name says "agent" and whose
     * meaning changes when a field is missing is one every client has
     * to be told about.
     */
    if (g_strcmp0(kind, "defaults.mount.add") == 0 ||
        g_strcmp0(kind, "defaults.mount.remove") == 0) {
        const gchar *target = clawt_ipc_payload_string(payload, "target");

        if (target == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "target is required");

        if (g_strcmp0(kind, "defaults.mount.remove") == 0) {
            if (!clawt_config_remove_default_mount(self->config, target))
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "there is no default shared "
                                           "folder at that path");
        } else {
            g_autoptr(ClawtMount) mount =
                clawt_daemon_mount_from_payload(self->config, payload, target,
                                                &error);

            if (mount == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            if (!clawt_config_add_default_mount(self->config, mount))
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "could not add the shared folder");
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Every agent is re-rendered, because this changed what all of
         * them get. Skipping it would leave the fleet's config.yaml
         * files describing the mounts they had before -- the "saved but
         * nothing rewrote what it produces" gap agent.set had, one
         * level up and affecting every agent at once.
         */
        clawt_agent_manager_load(self->agents, NULL);

        {
            g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "config.changed", NULL);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "target");
            json_builder_add_string_value(builder, target);

            /*
             * A mount reaches a running agent's computer when it is
             * next built, which is at its next start -- the container
             * or the domain already exists and its devices were decided
             * when it was created. Said here rather than discovered.
             */
            json_builder_set_member_name(builder, "restart_required");
            json_builder_add_boolean_value(builder, TRUE);
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "defaults.mount.list") == 0) {
        g_autoptr(GPtrArray) mounts =
            clawt_config_get_default_mounts(self->config);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "mounts");
        json_builder_begin_array(builder);

        for (i = 0; mounts != NULL && i < mounts->len; i++) {
            ClawtMount *mount = g_ptr_array_index(mounts, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "source");
            json_builder_add_string_value(builder,
                                          clawt_mount_get_source(mount));
            json_builder_set_member_name(builder, "target");
            json_builder_add_string_value(builder,
                                          clawt_mount_get_target(mount));
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_MOUNT_MODE,
                                            clawt_mount_get_mode(mount)));
            json_builder_set_member_name(builder, "type");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_MOUNT_TYPE,
                                            clawt_mount_get_mount_type(mount)));
            json_builder_set_member_name(builder, "relabel");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_RELABEL,
                                            clawt_mount_get_relabel(mount)));
            json_builder_set_member_name(builder, "required");
            json_builder_add_boolean_value(builder,
                                           clawt_mount_get_required(mount));
            json_builder_set_member_name(builder, "scope");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_SCOPE,
                                            clawt_mount_get_scope(mount)));
            clawt_daemon_add_string_array(builder, "agents",
                                          clawt_mount_get_agents(mount));
            clawt_daemon_add_string_array(builder, "teams",
                                          clawt_mount_get_teams(mount));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        /*
         * What only the whole fleet can show: a folder scoped to an
         * agent or a team that is not there, and so shared with nobody.
         * Reported here beside the list rather than refused, the same
         * way team.list reports what it can see -- and reported at all
         * because the alternative is silence, which is what made a team
         * id written under agents: cost somebody an agent with no
         * source tree and nothing to say why.
         */
        {
            g_auto(GStrv) warnings = NULL;
            guint w;

            clawt_mount_validate_fleet(self->config, &warnings);

            json_builder_set_member_name(builder, "warnings");
            json_builder_begin_array(builder);

            for (w = 0; warnings != NULL && warnings[w] != NULL; w++)
                json_builder_add_string_value(builder, warnings[w]);

            json_builder_end_array(builder);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.mount.list") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autoptr(GPtrArray) mounts = NULL;
        guint inherited = 0;
        guint i;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        /*
         * The mounts this agent actually gets, not only the ones it
         * declared.
         *
         * Reporting its own list alone would show an empty "Shared
         * folders" for an agent that has two, because the fleet's
         * defaults reach it without appearing anywhere on its page --
         * two answers to "what do I have", with the wrong one on the
         * screen somebody is looking at.
         *
         * Each entry says where it came from, so a client can show a
         * default as a default rather than as something removable here.
         */
        {
            g_autoptr(GPtrArray) own =
                clawt_agent_config_get_mounts(agent_config);
            ClawtComputerType type = (ClawtComputerType)
                clawt_agent_config_get_enum(agent_config, "computer.type");

            if (clawt_computer_type_takes_mounts(type) &&
                clawt_agent_config_get_boolean(agent_config,
                                               "computer.default_mounts")) {
                g_autoptr(GPtrArray) defaults =
                    clawt_config_get_default_mounts(self->config);
                g_autoptr(GPtrArray) mine = g_ptr_array_new_with_free_func(
                    (GDestroyNotify)clawt_mount_free);
                const gchar *team =
                    clawt_agent_config_get_string(agent_config, "team");
                guint d;

                /*
                 * Narrowed by scope first, exactly as the factory does.
                 * Reporting a folder this agent does not get would be
                 * the same "two answers to what do I have" this list
                 * was widened to fix, arrived at from the other side.
                 */
                for (d = 0; d < defaults->len; d++) {
                    ClawtMount *candidate = g_ptr_array_index(defaults, d);

                    if (clawt_mount_covers(candidate, agent_id, team))
                        g_ptr_array_add(mine, clawt_mount_copy(candidate));
                }

                mounts = clawt_mount_merge_defaults(mine, own);

                /*
                 * merge_defaults drops a default the agent overrode, so
                 * the count of inherited entries is however many
                 * survived -- not the number configured.
                 */
                inherited = mounts->len - own->len;
            } else {
                mounts = g_ptr_array_ref(own);
            }
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "mounts");
        json_builder_begin_array(builder);

        for (i = 0; i < mounts->len; i++) {
            ClawtMount *mount = g_ptr_array_index(mounts, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "inherited");
            json_builder_add_boolean_value(builder, i < inherited);
            json_builder_set_member_name(builder, "source");
            json_builder_add_string_value(builder,
                                          clawt_mount_get_source(mount));
            json_builder_set_member_name(builder, "target");
            json_builder_add_string_value(builder,
                                          clawt_mount_get_target(mount));
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_MOUNT_MODE,
                                            clawt_mount_get_mode(mount)));
            json_builder_set_member_name(builder, "type");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_MOUNT_TYPE,
                                            clawt_mount_get_mount_type(mount)));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
