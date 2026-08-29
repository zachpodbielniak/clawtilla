/*
 * daemon-connector.c - The client surface: connector.*
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
clawt_daemon_handle_connector(
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

    if (g_strcmp0(kind, "connector.catalog") == 0) {
        GPtrArray *catalog = clawt_daemon_catalog(self);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "connectors");
        json_builder_begin_array(builder);

        for (i = 0; catalog != NULL && i < catalog->len; i++) {
            const ClawtConnectorInfo *info = g_ptr_array_index(catalog, i);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "id", info->id);
            clawt_daemon_add_string_member(builder, "name", info->name);
            clawt_daemon_add_string_member(builder, "summary", info->summary);
            clawt_daemon_add_string_member(builder, "category",
                                           info->category);
            clawt_daemon_add_string_member(
                builder, "auth",
                clawt_enum_to_nick(CLAWT_TYPE_CONNECTOR_AUTH,
                                   (gint)info->auth));
            clawt_daemon_add_string_member(builder, "scopes", info->scopes);
            clawt_daemon_add_string_member(builder, "client_id_help",
                                           info->client_id_help);
            clawt_daemon_add_string_member(builder, "docs_url",
                                           info->docs_url);
            clawt_daemon_add_string_member(builder, "default_instance",
                                           info->default_instance);

            /*
             * Whether a server is known matters as much as the auth
             * does: a connector with neither this nor a `command` in
             * the integration authenticates perfectly and hands the
             * agent nothing.
             */
            json_builder_set_member_name(builder, "has_server");
            json_builder_add_boolean_value(builder,
                                           info->server_command != NULL ||
                                           info->server_url != NULL);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.list") == 0) {
        GPtrArray *integrations = clawt_config_get_integrations(self->config);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "connectors");
        json_builder_begin_array(builder);

        for (i = 0; integrations != NULL && i < integrations->len; i++) {
            ClawtIntegrationConfig *instance =
                g_ptr_array_index(integrations, i);
            const gchar *token_file;
            g_autoptr(ClawtOauthToken) token = NULL;

            if (g_strcmp0(clawt_integration_config_get_type_id(instance),
                          "connector") != 0)
                continue;

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(
                builder, "name",
                clawt_integration_config_get_name(instance));
            clawt_daemon_add_string_member(
                builder, "provider",
                clawt_integration_config_get_string(instance, NULL,
                                                    "provider"));
            clawt_daemon_add_string_member(
                builder, "account",
                clawt_integration_config_get_string(instance, NULL,
                                                    "account"));
            clawt_daemon_add_string_member(
                builder, "scope",
                clawt_enum_to_nick(
                    CLAWT_TYPE_SCOPE,
                    (gint)clawt_integration_config_get_scope(instance)));

            json_builder_set_member_name(builder, "enabled");
            json_builder_add_boolean_value(
                builder, clawt_integration_config_get_enabled(instance));

            token_file = clawt_integration_config_get_string(instance, NULL,
                                                             "token_file");

            if (token_file != NULL)
                token = clawt_oauth_token_load(token_file, NULL);

            /*
             * Everything about the credential except the credential.
             * Whether it exists, when it stops working and whether it
             * can renew itself are the three things somebody looking at
             * this list needs; the value is the one thing that must
             * never come back over IPC.
             */
            json_builder_set_member_name(builder, "connected");
            json_builder_add_boolean_value(builder, token != NULL);

            json_builder_set_member_name(builder, "expires_at");
            json_builder_add_int_value(builder,
                                       token != NULL ? token->expires_at : 0);

            json_builder_set_member_name(builder, "renewable");
            json_builder_add_boolean_value(
                builder, token != NULL && token->refresh_token != NULL);

            if (token != NULL)
                clawt_daemon_add_string_member(builder, "granted_scopes",
                                               token->scopes);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.begin") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autofree gchar *auth_url = NULL;
        g_autofree gchar *token_url = NULL;
        const gchar *client_id;
        const gchar *instance_url;
        const gchar *scopes;
        ConnectorFlow *flow;

        clawt_daemon_sweep_connector_flows(self);

        binding = clawt_daemon_connector_binding(self, name, &connector,
                                                 &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (connector->auth == CLAWT_CONNECTOR_AUTH_API_KEY ||
            connector->auth == CLAWT_CONNECTOR_AUTH_NONE)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this connector takes a key rather "
                                       "than an authorization; use "
                                       "`clawtilla connector key`");

        client_id = clawt_integration_binding_get_string(binding, "client_id");

        if (client_id == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_CONFIG_INVALID,
                connector->client_id_help != NULL
                ? connector->client_id_help
                : "this connector needs a client_id you registered with "
                  "the provider");

        instance_url = clawt_integration_binding_get_string(binding,
                                                            "instance");
        auth_url = clawt_connector_resolve_url(connector, connector->auth_url,
                                               instance_url);
        token_url = clawt_connector_resolve_url(connector,
                                                connector->token_url,
                                                instance_url);

        if (auth_url == NULL || token_url == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "this connector has no authorization "
                                       "endpoints");

        scopes = clawt_integration_binding_get_string(binding, "scopes");

        if (scopes == NULL)
            scopes = connector->scopes;

        flow = g_new0(ConnectorFlow, 1);
        flow->daemon = self;
        flow->id = g_uuid_string_random();
        flow->name = g_strdup(name);
        flow->token_url = g_steal_pointer(&token_url);
        flow->client_id = g_strdup(client_id);
        flow->client_secret =
            clawt_daemon_connector_client_secret(self, binding);

        g_hash_table_insert(self->connector_flows, g_strdup(flow->id), flow);

        if (connector->auth == CLAWT_CONNECTOR_AUTH_DEVICE) {
            BeginWait *begin = g_new0(BeginWait, 1);

            begin->flow = flow;
            begin->pending = clawt_ipc_server_defer(self->ipc_server, request);

            if (begin->pending == NULL) {
                g_free(begin);
                g_hash_table_remove(self->connector_flows, flow->id);

                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "this request cannot be answered "
                                           "later");
            }

            /*
             * Deferred because the codes come from the provider, and
             * there is nothing to show anybody until they do.  The poll
             * that follows is *not* deferred onto this request -- it
             * takes as long as a person takes, which is what
             * connector.await is for.
             */
            clawt_oauth_device_begin_async(
                auth_url, client_id, scopes, NULL,
                clawt_daemon_on_connector_begun, begin);
            return NULL;
        }

        /* The authorization-code flow, for providers with no device grant. */
        {
            g_autofree gchar *state = clawt_oauth_pkce_verifier();
            g_autofree gchar *challenge = NULL;
            g_autofree gchar *url = NULL;
            gint64 port = clawt_config_get_int(self->config,
                                               "connectors.redirect_port");

            flow->verifier = clawt_oauth_pkce_verifier();

            if (flow->verifier == NULL || state == NULL) {
                g_hash_table_remove(self->connector_flows, flow->id);

                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "this machine has no usable "
                                           "randomness, and a guessable "
                                           "verifier is no protection at "
                                           "all");
            }

            flow->redirect_uri =
                g_strdup_printf("http://127.0.0.1:%d/callback", (gint)port);

            challenge = clawt_oauth_pkce_challenge(flow->verifier);
            url = clawt_oauth_authorize_url(auth_url, client_id,
                                            flow->redirect_uri, scopes, state,
                                            challenge);

            /*
             * The listener goes up before the URL is handed out.  A
             * person who is quick would otherwise be redirected to a
             * port nothing is listening on, and the browser would show
             * a connection refused for an authorization that in fact
             * succeeded.
             */
            clawt_oauth_await_redirect_async(
                (guint)port, state, 600, NULL,
                clawt_daemon_on_connector_redirected, flow);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "flow", flow->id);
            clawt_daemon_add_string_member(builder, "method", "pkce");
            clawt_daemon_add_string_member(builder, "authorize_url", url);
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }
    }

    if (g_strcmp0(kind, "connector.await") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "flow");
        ConnectorFlow *flow = (id != NULL)
            ? g_hash_table_lookup(self->connector_flows, id) : NULL;

        if (flow == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no connection attempt with "
                                       "that id");

        /*
         * A flow that finished before anybody asked keeps its answer.
         * Somebody who started an authorization, walked away and came
         * back should not find that the result was delivered to nobody.
         */
        if (flow->settled) {
            gboolean ok = flow->ok;
            g_autofree gchar *message = g_strdup(flow->message);
            g_autofree gchar *flow_name = g_strdup(flow->name);

            g_hash_table_remove(self->connector_flows, id);

            if (!ok)
                return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                           message != NULL ? message
                                           : "the flow did not complete");

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "connected");
            json_builder_add_boolean_value(builder, TRUE);
            clawt_daemon_add_string_member(builder, "name", flow_name);
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        flow->waiter = clawt_ipc_server_defer(self->ipc_server, request);

        if (flow->waiter == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");

        return NULL;
    }

    if (g_strcmp0(kind, "connector.key") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *key = clawt_ipc_payload_string(payload, "key");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autofree gchar *secrets_dir = NULL;
        g_autofree gchar *path = NULL;

        binding = clawt_daemon_connector_binding(self, name, &connector,
                                                 &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (key == NULL || *key == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "no key was given");

        /*
         * Accepted for any connector, not only the api_key ones.  A
         * personal access token is a perfectly good credential for
         * GitHub or GitLab, and taking one here means somebody who
         * wants an agent reading their repositories does not first have
         * to go and register an OAuth application.
         *
         * It is stored in the same shape as a negotiated token, so
         * everything downstream -- the relay, the health check, the
         * list -- has one thing to read rather than two.
         */
        token = g_new0(ClawtOauthToken, 1);
        token->access_token = g_strdup(key);

        if (!clawt_daemon_store_connector_token(self, name, token, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");
        path = clawt_connector_token_path(secrets_dir, name);

        /*
         * The path, never the value.  Handing the key back to the client
         * that sent it would put a live credential into the memory of
         * every client that asked.
         */
        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "token_file", path);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.refresh") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autofree gchar *token_url = NULL;
        const gchar *token_file;
        const gchar *client_id;
        RefreshJob *job;

        binding = clawt_daemon_connector_binding(self, name, &connector,
                                                 &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        token_file = clawt_integration_binding_get_string(binding,
                                                          "token_file");
        token = (token_file != NULL) ? clawt_oauth_token_load(token_file, NULL)
                                     : NULL;

        if (token == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                       "it is not connected yet");

        if (token->refresh_token == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "the provider issued nothing to renew "
                                       "with; connect again instead");

        client_id = clawt_integration_binding_get_string(binding, "client_id");
        token_url = clawt_connector_resolve_url(
            connector, connector->token_url,
            clawt_integration_binding_get_string(binding, "instance"));

        if (client_id == NULL || token_url == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "there is nowhere to renew it");

        job = g_new0(RefreshJob, 1);
        job->daemon = self;
        job->name = g_strdup(name);
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            clawt_daemon_refresh_job_free(job);

            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        {
            g_autofree gchar *secret =
                clawt_daemon_connector_client_secret(self, binding);

            clawt_oauth_refresh_async(
                token_url, client_id, secret, token->refresh_token, NULL,
                clawt_daemon_on_connector_refreshed, job);
        }

        return NULL;
    }

    if (g_strcmp0(kind, "connector.revoke") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autofree gchar *revoke_url = NULL;
        const gchar *token_file;

        binding = clawt_daemon_connector_binding(self, name, &connector,
                                                 &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        token_file = clawt_integration_binding_get_string(binding,
                                                          "token_file");
        token = (token_file != NULL) ? clawt_oauth_token_load(token_file, NULL)
                                     : NULL;

        revoke_url = clawt_connector_resolve_url(
            connector, connector->revoke_url,
            clawt_integration_binding_get_string(binding, "instance"));

        /*
         * The local copy goes whatever the provider says.  Somebody who
         * asked to revoke wants the fleet to stop using it now, and a
         * provider that is unreachable must not leave an agent holding
         * a working credential until the network comes back.
         */
        if (!clawt_daemon_forget_connector_token(self, name, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        if (token != NULL && revoke_url != NULL) {
            RevokeJob *job = g_new0(RevokeJob, 1);

            job->pending = clawt_ipc_server_defer(self->ipc_server, request);

            if (job->pending != NULL) {
                job->name = g_strdup(name);

                clawt_oauth_revoke_async(
                    revoke_url, clawt_integration_binding_get_string(
                                    binding, "client_id"),
                    NULL, token->access_token, NULL,
                    clawt_daemon_on_connector_revoked, job);

                return NULL;
            }

            g_free(job);
        }

        /*
         * Says plainly when the provider was not told.  A person who
         * believes a token is dead and finds it working months later
         * has been misled by this reply, and the fix -- their settings
         * page -- is somewhere only they can go.
         */
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "forgotten");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_set_member_name(builder, "told_provider");
        json_builder_add_boolean_value(builder, FALSE);

        if (token != NULL && revoke_url == NULL)
            clawt_daemon_add_string_member(
                builder, "note",
                "this provider offers no revocation endpoint; "
                "the credential is gone from here but remains valid until "
                "you withdraw it in their settings");

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
