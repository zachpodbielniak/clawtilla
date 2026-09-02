/*
 * daemon-integration.c - The client surface: integration.*
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
clawt_daemon_handle_integration(
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

    if (g_strcmp0(kind, "integration.types") == 0) {
        const ClawtIntegrationInfo *info;
        gsize n_integrations = 0;
        gsize i;

        info = clawt_integration_list(&n_integrations);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "types");
        json_builder_begin_array(builder);

        for (i = 0; i < n_integrations; i++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, info[i].id);
            json_builder_set_member_name(builder, "kind");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_INTEGRATION_KIND,
                                            (gint)info[i].kind));
            json_builder_set_member_name(builder, "summary");
            json_builder_add_string_value(builder, info[i].summary);
            json_builder_set_member_name(builder, "one_per_agent");
            json_builder_add_boolean_value(builder, info[i].one_per_agent);
            json_builder_set_member_name(builder, "one_per_fleet");
            json_builder_add_boolean_value(builder, info[i].one_per_fleet);

            clawt_daemon_add_key_array(builder, "required_keys",
                                       info[i].required_keys);
            clawt_daemon_add_key_array(builder, "credential_keys",
                                       info[i].credential_keys);
            clawt_daemon_add_key_array(builder, "identity_keys",
                                       info[i].identity_keys);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.list") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        GPtrArray *instances = clawt_config_get_integrations(self->config);
        g_autoptr(GPtrArray) warnings = NULL;
        guint i;

        if (agent_id != NULL && agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        json_builder_begin_object(builder);

        /*
         * The shared instances, whatever was asked for.  A settings page
         * shows all of them; an agent inspector shows which of them reach
         * that agent, which is the `covers` flag rather than a filter --
         * the dialog needs the ones it could turn on as well as the ones
         * that are on.
         */
        json_builder_set_member_name(builder, "integrations");
        json_builder_begin_array(builder);

        for (i = 0; instances != NULL && i < instances->len; i++) {
            ClawtIntegrationConfig *instance =
                g_ptr_array_index(instances, i);

            clawt_daemon_add_integration_object(builder, self->config,
                                                instance, agent_id);
        }

        json_builder_end_array(builder);

        /*
         * And what one agent actually has, inline blocks included.  A
         * client cannot work this out from the list above, because an
         * agent's own `integrations:` block is not an instance and never
         * appears there.
         */
        if (agent_config != NULL) {
            g_autoptr(GPtrArray) bindings =
                clawt_integration_resolve_for_agent(self->config,
                                                    agent_config);

            json_builder_set_member_name(builder, "bindings");
            json_builder_begin_array(builder);

            for (i = 0; i < bindings->len; i++)
                clawt_daemon_add_binding_object(
                    builder, g_ptr_array_index(bindings, i));

            json_builder_end_array(builder);
        }

        clawt_integration_validate_fleet(self->config, &warnings);

        json_builder_set_member_name(builder, "warnings");
        json_builder_begin_array(builder);

        for (i = 0; warnings != NULL && i < warnings->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(warnings, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.add") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *type_id = clawt_ipc_payload_string(payload, "type");
        ClawtIntegrationConfig *instance;

        instance = clawt_config_add_integration(self->config, name, type_id,
                                                &error);

        if (instance == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (clawt_integration_find(type_id) == NULL) {
            /*
             * Rolled back rather than left as a shadow.  A shadow agent
             * earns its keep because the config was already on disk when
             * we met it; here somebody has just typed a type that does
             * not exist, and the honest answer is to say so and change
             * nothing.
             */
            clawt_config_remove_integration(self->config, name);

            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "there is no integration type called "
                                       "that");
        }

        if (!clawt_daemon_apply_integration_fields(instance, payload,
                                                   &error)) {
            clawt_config_remove_integration(self->config, name);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_config_save(self->config, &error)) {
            clawt_config_remove_integration(self->config, name);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.update") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;

        if (instance == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        if (!clawt_daemon_apply_integration_fields(instance, payload, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "integration.remove") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");

        if (name == NULL ||
            !clawt_config_remove_integration(self->config, name))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The credential file it wrote is deliberately left where it is.
         * Removing an integration is a config change, and taking a token
         * off disk as a side effect of it is the kind of helpfulness that
         * is only noticed when it was wrong.
         */
        clawt_event_bus_emit(self->bus, "integration.changed", name);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "integration.health") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autoptr(GPtrArray) bindings = NULL;
        HealthRun *run;
        guint i;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        bindings = clawt_integration_resolve_for_agent(self->config,
                                                       agent_config);

        run = g_new0(HealthRun, 1);
        run->pending = clawt_ipc_server_defer(self->ipc_server, request);
        run->checks = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_integration_binding_unref);
        run->results = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_daemon_health_result_free);
        run->timeout = (guint)clawt_ipc_payload_int(payload, "timeout", 10);

        if (run->pending == NULL) {
            clawt_daemon_health_run_free(run);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        for (i = 0; i < bindings->len; i++) {
            ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

            if (name != NULL &&
                g_strcmp0(clawt_integration_binding_get_name(binding),
                          name) != 0 &&
                g_strcmp0(clawt_integration_binding_get_info(binding)->id,
                          name) != 0)
                continue;

            g_ptr_array_add(run->checks,
                            clawt_integration_binding_ref(binding));
        }

        clawt_daemon_health_run_start(run);

        /*
         * NULL, not a frame: the answer goes out from health_run_finish()
         * when the last check comes back.  A handler that waits here
         * would hold the daemon's main context for the whole timeout,
         * which is exactly the ten seconds in which nothing else is
         * routed.
         */
        return NULL;
    }

    if (g_strcmp0(kind, "integration.notify_test") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        ClawtIpcPending *pending;

        if (self->notifier == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this daemon has no notifier");

        pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (pending == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");

        /*
         * A notifier is the one thing in a fleet you cannot tell is
         * working by looking at it: it is correct precisely when nothing
         * happens. This is the button that makes something happen.
         */
        clawt_notifier_test_async(self->notifier, name, NULL,
                                  clawt_daemon_on_notify_tested, pending);

        return NULL;
    }

    if (g_strcmp0(kind, "integration.matrix_login") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        const gchar *homeserver = clawt_ipc_payload_string(payload,
                                                           "homeserver");
        const gchar *user = clawt_ipc_payload_string(payload, "user");
        const gchar *password = clawt_ipc_payload_string(payload, "password");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;
        MatrixLogin *login;

        if (instance == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        if (homeserver == NULL)
            homeserver = clawt_integration_config_get_string(instance,
                                                             agent_id,
                                                             "homeserver");

        if (homeserver == NULL || user == NULL || password == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a homeserver, a user and a password "
                                       "are all needed");

        /*
         * The agent id becomes part of the token's filename, so it is
         * checked here rather than folded there.  An id holding a
         * separator would have put a live Matrix access token outside
         * the 0700 secrets directory -- on top of whatever it named --
         * and then written that path into clawtilla.yaml, so the
         * traversal survived a restart.  clawt_connector_token_path()
         * had already learned this about instance names.
         *
         * An id that names no agent is refused in the same breath: a
         * per-agent override for an agent that does not exist reaches
         * nobody, so writing its credential to disk is pure cost.
         */
        if (agent_id != NULL) {
            if (!clawt_is_valid_id(agent_id))
                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           "that is not an agent id");

            if (clawt_agent_manager_get(self->agents, agent_id) == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "there is no agent called that");
        }

        login = g_new0(MatrixLogin, 1);
        login->daemon = self;
        login->pending = clawt_ipc_server_defer(self->ipc_server, request);
        login->name = g_strdup(name);
        login->agent_id = g_strdup(agent_id);
        login->homeserver = g_strdup(homeserver);

        if (login->pending == NULL) {
            clawt_daemon_matrix_login_free(login);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        {
            g_autofree gchar *device = g_strdup_printf(
                "clawtilla (%s)", agent_id != NULL ? agent_id : name);

            clawt_matrix_login_async(homeserver, user, password, device, NULL,
                                     clawt_daemon_on_matrix_login, login);
        }

        return NULL;
    }

    if (g_strcmp0(kind, "integration.matrix_rooms") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;
        g_autoptr(ClawtSecretRef) ref = NULL;
        g_autofree gchar *token = NULL;
        g_autofree gchar *secrets_dir = NULL;
        const gchar *homeserver;
        ClawtIpcPending *pending;

        if (instance == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        homeserver = clawt_integration_config_get_string(instance, agent_id,
                                                         "homeserver");
        ref = clawt_integration_config_get_secret(instance, agent_id,
                                                  "access_token");

        if (homeserver == NULL || ref == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "sign in first: there is no "
                                       "homeserver and token to list with");

        secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");
        token = clawt_secret_ref_resolve(
            ref, secrets_dir,
            (guint)clawt_config_get_int(self->config,
                                        "secrets.command_timeout_seconds"),
            &error);

        if (token == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_SECRET,
                                       error->message);

        pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (pending == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");

        clawt_matrix_rooms_async(homeserver, token, NULL,
                                 clawt_daemon_on_matrix_rooms, pending);

        return NULL;
    }

    *handled = FALSE;
    return NULL;
}
