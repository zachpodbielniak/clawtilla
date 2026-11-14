/*
 * daemon-connector.c - The client surface: connector.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"


/* ── Signing in where the server says ────────────────────────────── */

/*
 * Starts the authorization-code flow against endpoints already known --
 * from the catalogue, or from discovery -- and builds the reply that
 * hands the person the URL.
 *
 * @flow is already in the table; on failure the caller takes it out.
 */
static JsonNode *
start_pkce(ClawtDaemon   *self,
           ConnectorFlow *flow,
           const gchar   *auth_url,
           const gchar   *scopes,
           const gchar   *auth_server,
           GError       **error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autofree gchar *state = clawt_oauth_pkce_verifier();
    g_autofree gchar *challenge = NULL;
    g_autofree gchar *url = NULL;
    gint64 port = clawt_config_get_int(self->config,
                                       "connectors.redirect_port");

    flow->verifier = clawt_oauth_pkce_verifier();

    if (flow->verifier == NULL || state == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "this machine has no usable randomness, and a "
                            "guessable verifier is no protection at all");
        return NULL;
    }

    if (flow->redirect_uri == NULL)
        flow->redirect_uri =
            g_strdup_printf("http://127.0.0.1:%d/callback", (gint)port);

    challenge = clawt_oauth_pkce_challenge(flow->verifier);
    url = clawt_oauth_authorize_url(auth_url, flow->client_id,
                                    flow->redirect_uri, scopes, state,
                                    challenge, flow->resource);

    /*
     * The listener goes up before the URL is handed out.  A person who
     * is quick would otherwise be redirected to a port nothing is
     * listening on, and the browser would show a connection refused for
     * an authorization that in fact succeeded.
     */
    clawt_oauth_await_redirect_async(
        (guint)port, state, 600, NULL,
        clawt_daemon_on_connector_redirected, flow);

    json_builder_begin_object(builder);
    clawt_daemon_add_string_member(builder, "flow", flow->id);
    clawt_daemon_add_string_member(builder, "method", "pkce");
    clawt_daemon_add_string_member(builder, "authorize_url", url);

    /*
     * Named so the person can see where they are about to sign in
     * before they do it -- a discovered server is the remote server's
     * claim, and the host in the address bar is the check on it.
     */
    if (auth_server != NULL)
        clawt_daemon_add_string_member(builder, "auth_server", auth_server);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * One sign-in that has to learn where to sign in first: discovery, then
 * registration when nobody configured a client id, then the flow.  Every
 * step is on the network, so the request is deferred and each step is a
 * callback; nothing here waits on the daemon's loop.
 *
 * `connector.discover` runs the first step alone and answers with what it
 * found.
 */
typedef struct {
    ClawtDaemon          *daemon;
    ClawtIpcPending      *pending;
    gchar                *name;
    gchar                *mcp_url;
    gchar                *scopes;
    gchar                *client_id;
    gchar                *client_secret;
    ClawtMcpAuthMetadata *metadata;
    ClawtMcpAuthClient   *registered;
    gboolean              discover_only;
} SignIn;

static void
sign_in_free(SignIn *sign_in)
{
    g_free(sign_in->name);
    g_free(sign_in->mcp_url);
    g_free(sign_in->scopes);
    g_free(sign_in->client_id);

    if (sign_in->client_secret != NULL)
        memset(sign_in->client_secret, 0, strlen(sign_in->client_secret));

    g_free(sign_in->client_secret);
    clawt_mcp_auth_metadata_free(sign_in->metadata);
    clawt_mcp_auth_client_free(sign_in->registered);
    g_free(sign_in);
}

static void
sign_in_fail(SignIn *sign_in, gint code, const gchar *message)
{
    clawt_ipc_pending_respond(
        sign_in->pending,
        clawt_ipc_error_new(clawt_ipc_pending_get_request(sign_in->pending),
                            code, message));
    sign_in_free(sign_in);
}

static void sign_in_continue(SignIn *sign_in);

/*
 * What `connector.discover` answers.  Finding no authorization server is
 * an answer, not a failure: most MCP servers take a key, and a client
 * that asked after adding one should be told "no sign-in here" without a
 * red toast for something that is not wrong.
 */
static void
discover_answer(SignIn *sign_in, const gchar *reason)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    ClawtMcpAuthMetadata *m = sign_in->metadata;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sign_in");
    json_builder_add_boolean_value(builder, m != NULL);

    if (m != NULL) {
        clawt_daemon_add_string_member(builder, "auth_server", m->issuer);
        clawt_daemon_add_string_member(builder, "source", m->source);
        json_builder_set_member_name(builder, "registration");
        json_builder_add_boolean_value(builder,
                                       m->registration_endpoint != NULL);
    }

    if (reason != NULL)
        clawt_daemon_add_string_member(builder, "reason", reason);

    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        sign_in->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(sign_in->pending),
                               json_builder_get_root(builder)));

    if (m != NULL)
        clawt_event_bus_emit(sign_in->daemon->bus, "integration.changed",
                             sign_in->name);

    sign_in_free(sign_in);
}

static gboolean
sign_in_save(SignIn *sign_in, GError **error)
{
    g_autofree gchar *path =
        clawt_daemon_connector_auth_path(sign_in->daemon, sign_in->name);

    return clawt_mcp_auth_save(path, sign_in->metadata, sign_in->registered,
                               error);
}

static void
on_sign_in_discovered(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    SignIn *sign_in = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GError) save_error = NULL;
    ClawtMcpAuthMetadata *found;

    found = clawt_mcp_auth_discover_finish(result, &error);

    if (found == NULL) {
        if (sign_in->discover_only) {
            discover_answer(sign_in, error->message);
            return;
        }

        sign_in_fail(sign_in, CLAWT_ERROR_AUTH, error->message);
        return;
    }

    /*
     * A client registered with one authorization server is a stranger to
     * any other, so a server that moved takes its registration with it.
     */
    if (sign_in->metadata != NULL &&
        g_strcmp0(sign_in->metadata->issuer, found->issuer) != 0)
        g_clear_pointer(&sign_in->registered, clawt_mcp_auth_client_free);

    clawt_mcp_auth_metadata_free(sign_in->metadata);
    sign_in->metadata = found;

    if (!sign_in_save(sign_in, &save_error))
        g_warning("connector '%s': could not keep what discovery found: %s",
                  sign_in->name, save_error->message);

    if (sign_in->discover_only) {
        discover_answer(sign_in, NULL);
        return;
    }

    sign_in_continue(sign_in);
}

static void
on_sign_in_registered(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    SignIn *sign_in = user_data;
    g_autoptr(GError) error = NULL;
    ClawtMcpAuthClient *client;

    client = clawt_mcp_auth_register_finish(result, &error);

    if (client == NULL) {
        sign_in_fail(sign_in, CLAWT_ERROR_AUTH, error->message);
        return;
    }

    clawt_mcp_auth_client_free(sign_in->registered);
    sign_in->registered = client;

    /*
     * Kept before it is used.  A registration is not free to repeat --
     * some servers rate-limit it, and every one leaves a client behind
     * on the server -- so losing it to a flow that is then abandoned
     * would register again next time.
     */
    if (!sign_in_save(sign_in, &error)) {
        sign_in_fail(sign_in, CLAWT_ERROR_FAILED, error->message);
        return;
    }

    sign_in_continue(sign_in);
}

static void
sign_in_continue(SignIn *sign_in)
{
    ClawtDaemon *self = sign_in->daemon;
    const gchar *client_id = sign_in->client_id;
    ClawtMcpAuthPlan plan;

    if (client_id == NULL && sign_in->registered != NULL)
        client_id = sign_in->registered->client_id;

    plan = clawt_mcp_auth_plan(TRUE, FALSE, TRUE, sign_in->metadata,
                               client_id);

    /* No default: a step added to the plan must be decided here. */
    switch (plan) {
    case CLAWT_MCP_AUTH_PLAN_NONE:
    case CLAWT_MCP_AUTH_PLAN_CONFIGURED:
        sign_in_fail(sign_in, CLAWT_ERROR_CONFIG_INVALID,
                     "this connector has no authorization endpoints");
        return;

    case CLAWT_MCP_AUTH_PLAN_DISCOVER:
        {
            const gchar *hint = g_hash_table_lookup(
                self->connector_sign_in_wanted, sign_in->name);

            clawt_mcp_auth_discover_async(
                sign_in->mcp_url,
                (hint != NULL && *hint != '\0') ? hint : NULL, NULL,
                on_sign_in_discovered, sign_in);
        }
        return;

    case CLAWT_MCP_AUTH_PLAN_REGISTER:
        {
            g_autofree gchar *redirect = g_strdup_printf(
                "http://127.0.0.1:%d/callback",
                (gint)clawt_config_get_int(self->config,
                                           "connectors.redirect_port"));

            clawt_mcp_auth_register_async(
                sign_in->metadata, redirect,
                clawt_config_get_string(self->config,
                                        "connectors.client_name"),
                NULL, on_sign_in_registered, sign_in);
        }
        return;

    case CLAWT_MCP_AUTH_PLAN_NO_CLIENT:
        {
            g_autofree gchar *message = g_strdup_printf(
                "%s offers no dynamic client registration, so clawtilla "
                "cannot register itself there; register a client with it "
                "and set client_id on the connector",
                sign_in->metadata->issuer);

            sign_in_fail(sign_in, CLAWT_ERROR_CONFIG_INVALID, message);
        }
        return;

    case CLAWT_MCP_AUTH_PLAN_SIGN_IN:
        break;
    }

    {
        ConnectorFlow *flow = g_new0(ConnectorFlow, 1);
        g_autoptr(GError) error = NULL;
        JsonNode *payload;

        flow->daemon = self;
        flow->id = g_uuid_string_random();
        flow->name = g_strdup(sign_in->name);
        flow->token_url = g_strdup(sign_in->metadata->token_endpoint);
        flow->client_id = g_strdup(client_id);
        flow->client_secret = g_strdup(
            sign_in->client_id != NULL ? sign_in->client_secret
            : sign_in->registered != NULL ? sign_in->registered->client_secret
                                          : NULL);
        flow->resource = g_strdup(sign_in->metadata->resource);
        flow->basic_auth = flow->client_secret != NULL &&
                           clawt_mcp_auth_uses_basic(
                               sign_in->metadata,
                               sign_in->client_id != NULL
                               ? NULL : sign_in->registered);

        g_hash_table_insert(self->connector_flows, g_strdup(flow->id), flow);

        payload = start_pkce(self, flow,
                             sign_in->metadata->authorization_endpoint,
                             sign_in->scopes != NULL ? sign_in->scopes
                                                     : sign_in->metadata->scopes,
                             sign_in->metadata->issuer, &error);

        if (payload == NULL) {
            g_hash_table_remove(self->connector_flows, flow->id);
            sign_in_fail(sign_in, error->code, error->message);
            return;
        }

        clawt_ipc_pending_respond(
            sign_in->pending,
            clawt_ipc_response_new(
                clawt_ipc_pending_get_request(sign_in->pending), payload));
        sign_in_free(sign_in);
    }
}

/*
 * Builds the sign-in for a connector discovery applies to, or says why
 * not.  The request is deferred onto it; the caller returns NULL.
 */
static JsonNode *
begin_discovered(ClawtDaemon              *self,
                 JsonNode                 *request,
                 const gchar              *name,
                 ClawtIntegrationBinding  *binding,
                 const gchar              *mcp_url,
                 gboolean                  discover_only)
{
    SignIn *sign_in;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    if (!clawt_config_get_boolean(self->config, "connectors.discovery"))
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                   "connectors.discovery is off, and this "
                                   "connector names no authorization "
                                   "endpoints of its own");

    sign_in = g_new0(SignIn, 1);
    sign_in->daemon = self;
    sign_in->name = g_strdup(name);
    sign_in->mcp_url = g_strdup(mcp_url);
    sign_in->discover_only = discover_only;
    sign_in->scopes =
        g_strdup(clawt_integration_binding_get_string(binding, "scopes"));
    sign_in->client_id =
        g_strdup(clawt_integration_binding_get_string(binding, "client_id"));

    if (sign_in->client_id != NULL)
        sign_in->client_secret =
            clawt_daemon_connector_client_secret(self, binding);

    path = clawt_daemon_connector_auth_path(self, name);

    if (!clawt_mcp_auth_load(path, &sign_in->metadata, &sign_in->registered,
                             &error))
        g_warning("connector '%s': %s; discovering again", name,
                  error->message);

    sign_in->pending = clawt_ipc_server_defer(self->ipc_server, request);

    if (sign_in->pending == NULL) {
        sign_in_free(sign_in);
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                   "this request cannot be answered later");
    }

    /*
     * Asked to look again: what was found before is not the answer, but
     * it stays on the struct until the new answer replaces it, so a
     * server that moved can be noticed and its registration dropped.
     */
    if (discover_only) {
        const gchar *hint = g_hash_table_lookup(self->connector_sign_in_wanted,
                                                name);

        clawt_mcp_auth_discover_async(
            mcp_url, (hint != NULL && *hint != '\0') ? hint : NULL, NULL,
            on_sign_in_discovered, sign_in);
        return NULL;
    }

    sign_in_continue(sign_in);
    return NULL;
}

/*
 * The two thresholds, as members of whatever object is open.  Read
 * through the config getters, so the schema's defaults are what an
 * untouched fleet reports.
 */
static void
add_directory_thresholds(ClawtDaemon *self, JsonBuilder *builder)
{
    json_builder_set_member_name(builder, "max_result_bytes");
    json_builder_add_int_value(
        builder, clawt_config_get_int(self->config,
                                      "connectors.max_result_bytes"));
    json_builder_set_member_name(builder, "directory_min_tools");
    json_builder_add_int_value(
        builder, clawt_config_get_int(self->config,
                                      "connectors.directory_min_tools"));
    json_builder_set_member_name(builder, "directory_min_chars");
    json_builder_add_int_value(
        builder, clawt_config_get_int(self->config,
                                      "connectors.directory_min_chars"));
}

/*
 * The result budget and the apps switch, per connector: the connector's
 * own value (-1 when it sets none), what that resolves to with the
 * fleet's, and whether its apps are shown in chat.
 */
static void
add_budget_members(ClawtDaemon            *self,
                   JsonBuilder            *builder,
                   ClawtIntegrationConfig *instance)
{
    gint64 own = clawt_integration_config_get_int(instance, NULL,
                                                  "max_result_bytes");
    gint64 effective = clawt_mcp_result_budget_resolve(
        own, clawt_config_get_int(self->config, "connectors.max_result_bytes"));
    g_autofree gchar *label = clawt_mcp_result_budget_label(effective);

    json_builder_set_member_name(builder, "max_result_bytes");
    json_builder_add_int_value(builder, own);
    json_builder_set_member_name(builder, "effective_max_result_bytes");
    json_builder_add_int_value(builder, effective);
    clawt_daemon_add_string_member(builder, "max_result_label", label);
    json_builder_set_member_name(builder, "show_apps");
    json_builder_add_boolean_value(
        builder, clawt_integration_config_get_boolean(instance, NULL,
                                                      "show_apps"));
}

/*
 * The connector an agent holds by @name, or NULL -- so a relay's report
 * about a connector the agent was never given is not believed.
 */
static ClawtIntegrationBinding *
agent_connector(ClawtDaemon *self, const gchar *agent_id, const gchar *name)
{
    ClawtAgentConfig *agent = (agent_id != NULL)
        ? clawt_config_get_agent(self->config, agent_id) : NULL;
    g_autoptr(GPtrArray) bindings = NULL;
    guint i;

    if (agent == NULL || name == NULL)
        return NULL;

    bindings = clawt_integration_resolve_for_agent(self->config, agent);

    for (i = 0; bindings != NULL && i < bindings->len; i++) {
        ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

        if (g_strcmp0(clawt_integration_binding_get_name(binding), name) == 0 &&
            g_strcmp0(clawt_integration_binding_get_info(binding)->id,
                      "connector") == 0)
            return clawt_integration_binding_ref(binding);
    }

    return NULL;
}

/*
 * How big a connector's server is, as a relay last saw it, and whether
 * an agent whose CLI does not search tools itself gets it as a
 * directory.  -1 tools means no relay has reported it yet, which is a
 * different thing from a server with none.
 */
static void
add_directory_members(ClawtDaemon            *self,
                      JsonBuilder            *builder,
                      ClawtIntegrationConfig *instance)
{
    g_autofree gchar *path = clawt_tool_census_path(self->state_dir);
    g_autoptr(ClawtToolCensus) census = clawt_tool_census_load(
        path, clawt_integration_config_get_name(instance));
    const gchar *nick =
        clawt_integration_config_get_string(instance, NULL, "directory");
    ClawtToolDirectoryMode mode = CLAWT_TOOL_DIRECTORY_AUTO;

    /* A spelling no mode has is reported as auto, which is what it does. */
    (void)clawt_tool_directory_mode_from_nick(nick, &mode);

    clawt_daemon_add_string_member(
        builder, "directory",
        clawt_enum_to_nick(CLAWT_TYPE_TOOL_DIRECTORY_MODE, (gint)mode));

    json_builder_set_member_name(builder, "tool_count");
    json_builder_add_int_value(builder, census != NULL ? census->tools : -1);
    json_builder_set_member_name(builder, "tool_chars");
    json_builder_add_int_value(builder, census != NULL ? census->chars : 0);

    if (census != NULL && census->areas != NULL)
        clawt_daemon_add_string_member(builder, "tool_areas", census->areas);

    json_builder_set_member_name(builder, "presented_as_directory");
    json_builder_add_boolean_value(
        builder,
        clawt_mcp_directory_decide(
            mode, FALSE,
            census != NULL ? (guint64)census->tools : 0,
            census != NULL ? (guint64)census->chars : 0,
            (guint64)MAX(clawt_config_get_int(
                self->config, "connectors.directory_min_tools"), 0),
            (guint64)MAX(clawt_config_get_int(
                self->config, "connectors.directory_min_chars"), 0)));
}

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

    /*
     * The integration broker's connectors are made from the config, not
     * read from it: an edit here would be saved nowhere and undone by the
     * next reload, so it is refused with the settings that do change them.
     */
    if (g_strcmp0(kind, "connector.begin") == 0 ||
        g_strcmp0(kind, "connector.discover") == 0 ||
        g_strcmp0(kind, "connector.key") == 0 ||
        g_strcmp0(kind, "connector.revoke") == 0) {
        JsonNode *refused = clawt_daemon_broker_refuse_managed(
            self, request, clawt_ipc_payload_string(payload, "name"));

        if (refused != NULL)
            return refused;
    }

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

            /*
             * Whether to offer a sign-in, and where it would happen.
             *
             * Offered when discovery found an authorization server or
             * the server itself answered 401 -- "discovery says so" --
             * and never merely because a connector exists: a key-based
             * one has nothing to sign in to, and a button that leads to
             * an error teaches somebody the button is broken.
             */
            {
                const ClawtConnectorInfo *connector = NULL;
                g_autoptr(ClawtIntegrationBinding) binding =
                    clawt_daemon_connector_binding(
                        self, clawt_integration_config_get_name(instance),
                        &connector, NULL);
                g_autofree gchar *remote = NULL;
                g_autofree gchar *path = NULL;
                g_autoptr(ClawtMcpAuthMetadata) metadata = NULL;
                gboolean wanted = g_hash_table_contains(
                    self->connector_sign_in_wanted,
                    clawt_integration_config_get_name(instance));
                gboolean offered = FALSE;

                if (binding != NULL) {
                    ClawtMcpAuthPlan plan;

                    remote = clawt_daemon_connector_remote_url(binding,
                                                               connector);
                    path = clawt_daemon_connector_auth_path(
                        self, clawt_integration_config_get_name(instance));
                    clawt_mcp_auth_load(path, &metadata, NULL, NULL);

                    plan = clawt_mcp_auth_plan(
                        connector->auth == CLAWT_CONNECTOR_AUTH_DEVICE ||
                        connector->auth == CLAWT_CONNECTOR_AUTH_PKCE,
                        connector->auth_url != NULL &&
                        connector->token_url != NULL,
                        remote != NULL, metadata, NULL);

                    offered = plan != CLAWT_MCP_AUTH_PLAN_NONE &&
                              (metadata != NULL || wanted);
                }

                json_builder_set_member_name(builder, "remote");
                json_builder_add_boolean_value(builder, remote != NULL);
                json_builder_set_member_name(builder, "sign_in");
                json_builder_add_boolean_value(builder, offered);
                json_builder_set_member_name(builder, "needs_sign_in");
                json_builder_add_boolean_value(builder, wanted);

                if (metadata != NULL && metadata->issuer != NULL)
                    clawt_daemon_add_string_member(builder, "auth_server",
                                                   metadata->issuer);
            }
            add_directory_members(self, builder, instance);
            add_budget_members(self, builder, instance);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        add_directory_thresholds(self, builder);
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

        /*
         * A catalogue entry that names its endpoints is a decision
         * somebody made, and wins.  One that names none, for a server
         * reached by URL, is asked of the server: RFC 9728, RFC 8414 and
         * -- with no client id configured -- RFC 7591.
         */
        if (connector->auth_url == NULL || connector->token_url == NULL) {
            g_autofree gchar *remote =
                clawt_daemon_connector_remote_url(binding, connector);

            if (remote != NULL)
                return begin_discovered(self, request, name, binding, remote,
                                        FALSE);
        }

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
            JsonNode *started = start_pkce(self, flow, auth_url, scopes, NULL,
                                           &error);

            if (started == NULL) {
                g_hash_table_remove(self->connector_flows, flow->id);

                return clawt_ipc_error_new(request, error->code,
                                           error->message);
            }

            return clawt_ipc_response_new(request, started);
        }
    }

    if (g_strcmp0(kind, "connector.discover") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autofree gchar *remote = NULL;

        binding = clawt_daemon_connector_binding(self, name, &connector,
                                                 &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        remote = clawt_daemon_connector_remote_url(binding, connector);

        /*
         * Answered, not refused: a connector with no URL, or one whose
         * catalogue entry names its endpoints, simply has nothing to
         * discover, and saying so is the whole answer.
         */
        if (remote == NULL ||
            (connector->auth_url != NULL && connector->token_url != NULL) ||
            connector->auth == CLAWT_CONNECTOR_AUTH_API_KEY ||
            connector->auth == CLAWT_CONNECTOR_AUTH_NONE) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "sign_in");
            json_builder_add_boolean_value(builder, FALSE);
            clawt_daemon_add_string_member(
                builder, "reason",
                remote == NULL
                ? "this connector's server is not reached by URL"
                : "this connector says how it signs in; nothing to discover");
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        return begin_discovered(self, request, name, binding, remote, TRUE);
    }

    /*
     * A relay's report that its server answered 401.
     *
     * Recorded rather than acted on: the daemon goes to the network only
     * when somebody asks it to sign in, and the 401's own
     * `resource_metadata` is kept for when they do.  Answered at once --
     * the relay is in the middle of a tool call.
     */
    if (g_strcmp0(kind, "connector.unauthorized") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *challenge = clawt_ipc_payload_string(payload,
                                                          "challenge");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;
        g_autofree gchar *hint = NULL;
        gboolean fresh;

        if (instance == NULL ||
            g_strcmp0(clawt_integration_config_get_type_id(instance),
                      "connector") != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no connector called that");

        hint = clawt_mcp_auth_parse_challenge(challenge);
        fresh = !g_hash_table_contains(self->connector_sign_in_wanted, name);

        g_hash_table_replace(self->connector_sign_in_wanted, g_strdup(name),
                             hint != NULL ? g_steal_pointer(&hint)
                                          : g_strdup(""));

        /* Once per episode, not once per refused call. */
        if (fresh) {
            g_message("connector '%s': its server refused a request (401); "
                      "it needs a sign-in", name);
            clawt_event_bus_emit(self->bus, "integration.changed", name);
        }

        json_builder_begin_object(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
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
        g_autofree gchar *client_id = NULL;
        g_autofree gchar *secret = NULL;
        g_autofree gchar *resource = NULL;
        g_autofree gchar *revoke_url = NULL;
        gboolean basic = FALSE;
        const gchar *token_file;
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

        clawt_daemon_connector_endpoints(self, binding, connector,
                                         &token_url, &client_id, &secret,
                                         &resource, &revoke_url, &basic);

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

        clawt_oauth_refresh_async(
            token_url, client_id, secret, token->refresh_token, resource,
            basic, NULL, clawt_daemon_on_connector_refreshed, job);

        return NULL;
    }

    if (g_strcmp0(kind, "connector.revoke") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autofree gchar *revoke_url = NULL;
        g_autofree gchar *token_url = NULL;
        g_autofree gchar *client_id = NULL;
        g_autofree gchar *secret = NULL;
        g_autofree gchar *resource = NULL;
        gboolean basic = FALSE;
        const gchar *token_file;

        binding = clawt_daemon_connector_binding(self, name, &connector,
                                                 &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        token_file = clawt_integration_binding_get_string(binding,
                                                          "token_file");
        token = (token_file != NULL) ? clawt_oauth_token_load(token_file, NULL)
                                     : NULL;

        clawt_daemon_connector_endpoints(self, binding, connector,
                                         &token_url, &client_id, &secret,
                                         &resource, &revoke_url, &basic);

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
                    revoke_url, client_id, NULL, token->access_token, NULL,
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

    if (g_strcmp0(kind, "connector.settings") == 0) {
        const gchar *keys[] = { "directory_min_tools",
                                "directory_min_chars", "max_result_bytes",
                                NULL };
        gboolean changed = FALSE;
        gsize k;

        /*
         * Both checked before either is written, so a refused value
         * leaves the other where it was rather than half an edit saved.
         */
        for (k = 0; keys[k] != NULL; k++) {
            if (json_object_has_member(payload, keys[k]) &&
                clawt_ipc_payload_int(payload, keys[k], 0) < 0)
                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           "a threshold or a budget cannot "
                                           "be below zero");
        }

        for (k = 0; keys[k] != NULL; k++) {
            g_autofree gchar *key = NULL;

            if (!json_object_has_member(payload, keys[k]))
                continue;

            key = g_strdup_printf("connectors.%s", keys[k]);
            clawt_config_set_int(self->config, key,
                                 clawt_ipc_payload_int(payload, keys[k], 0));
            changed = TRUE;
        }

        if (changed && !clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        add_directory_thresholds(self, builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.tools_seen") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;
        g_autofree gchar *path = NULL;
        ClawtToolCensus census = { 0 };

        /*
         * Only for a connector that exists: the file is keyed by name,
         * and a relay for something since removed must not bring its
         * row back.
         */
        if (instance == NULL ||
            g_strcmp0(clawt_integration_config_get_type_id(instance),
                      "connector") != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no connector called that");

        census.name = (gchar *)name;
        census.tools = MAX(clawt_ipc_payload_int(payload, "tools", 0), 0);
        census.chars = MAX(clawt_ipc_payload_int(payload, "chars", 0), 0);
        census.areas = (gchar *)clawt_ipc_payload_string(payload, "areas");
        census.seen_at = g_get_real_time() / G_USEC_PER_SEC;

        path = clawt_tool_census_path(self->state_dir);

        if (!clawt_tool_census_store(path, &census, &error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "connector.result_trimmed") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        g_autoptr(ClawtIntegrationBinding) binding =
            agent_connector(self, agent_id, name);
        g_autoptr(ClawtTurnStep) step = NULL;
        g_autofree gchar *notice = NULL;

        if (binding == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that agent holds no such connector");

        /*
         * Where the turn's tool calls are, so somebody reading the
         * conversation knows the agent saw part of a result.  A relay
         * with no room to name -- one started by hand -- has nowhere to
         * put it, and the agent was told in the result either way.
         */
        if (room_id == NULL || *room_id == '\0')
            return clawt_ipc_response_new(request, NULL);

        notice = clawt_mcp_result_notice(
            name, clawt_ipc_payload_string(payload, "tool"),
            (gsize)MAX(clawt_ipc_payload_int(payload, "original_bytes", 0), 0),
            (gsize)MAX(clawt_ipc_payload_int(payload, "kept_bytes", 0), 0),
            clawt_mcp_result_id_is_valid(
                clawt_ipc_payload_string(payload, "result_id"))
                ? clawt_ipc_payload_string(payload, "result_id") : NULL);
        step = clawt_turn_step_new(CLAWT_STEP_STATUS, agent_id, room_id,
                                   notice, NULL, NULL, FALSE);
        clawt_daemon_note_step(self, step);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "connector.app") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        JsonObject *app = (json_object_has_member(payload, "app") &&
                           JSON_NODE_HOLDS_OBJECT(
                               json_object_get_member(payload, "app")))
            ? json_object_get_object_member(payload, "app") : NULL;
        const gchar *name = (app != NULL)
            ? json_object_get_string_member_with_default(app, "connector",
                                                         NULL)
            : NULL;
        g_autoptr(ClawtIntegrationBinding) binding =
            agent_connector(self, agent_id, name);
        g_autoptr(JsonObject) record = NULL;
        g_autoptr(ClawtTurnStep) step = NULL;
        g_autofree gchar *id = NULL;
        g_autofree gchar *dir = NULL;
        g_autofree gchar *url = NULL;
        g_autofree gchar *text = NULL;
        const gchar *title;

        if (app == NULL || binding == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that agent holds no such connector");

        /*
         * Asked again here, not taken from the relay: the switch is the
         * person's, and a relay started before it was turned off must
         * not put an app in the conversation after.
         */
        if (!clawt_integration_binding_get_boolean(binding, "show_apps"))
            return clawt_ipc_error_new(request, CLAWT_ERROR_PERMISSION_DENIED,
                                       "this connector does not show its "
                                       "apps in chat");

        id = clawt_mcp_app_new_id();

        if (id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "could not make an id for the app");

        record = json_object_ref(app);
        json_object_set_string_member(record, "id", id);
        json_object_set_string_member(record, "agent", agent_id);

        if (room_id != NULL)
            json_object_set_string_member(record, "room", room_id);

        json_object_set_int_member(record, "created_at",
                                   g_get_real_time() / G_USEC_PER_SEC);

        dir = clawt_mcp_app_store_dir(self->config, agent_id);

        if (!clawt_mcp_app_save(dir, id, record, &error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       error->message);

        if (room_id != NULL && *room_id != '\0') {
            title = json_object_get_string_member_with_default(record, "title",
                                                               NULL);

            if (title == NULL)
                title = json_object_get_string_member_with_default(record,
                                                                   "tool",
                                                                   "an app");

            text = g_strdup_printf("App: %s from %s", title, name);
            url = clawt_mcp_app_web_url(
                clawt_config_get_string(self->config,
                                        "connectors.apps_web_url"),
                agent_id, id);
            step = clawt_turn_step_new(
                CLAWT_STEP_STATUS, agent_id, room_id, text,
                json_object_get_string_member_with_default(record, "tool",
                                                           NULL),
                NULL, FALSE);
            clawt_turn_step_set_app(step, id, url, title);
            clawt_daemon_note_step(self, step);
        }

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "id", id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.app_get") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *app_id = clawt_ipc_payload_string(payload, "app");
        g_autofree gchar *dir = NULL;
        g_autofree gchar *document = NULL;
        g_autofree gchar *url = NULL;
        g_autoptr(JsonObject) record = NULL;
        const gchar *html;
        const gchar *csp;
        const gchar *members[] = { "connector", "tool", "title",
                                   "resource_uri", "room", "blocked", NULL };
        guint m;

        if (agent_id == NULL ||
            clawt_config_get_agent(self->config, agent_id) == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        /* An app is looked up under its own agent and nowhere else. */
        dir = clawt_mcp_app_store_dir(self->config, agent_id);
        record = clawt_mcp_app_load(dir, app_id, &error);

        if (record == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       error->message);

        html = json_object_get_string_member_with_default(record, "html",
                                                          NULL);
        csp = json_object_get_string_member_with_default(record, "csp", NULL);

        /*
         * The document is built here from the kept view and the kept
         * policy, so a client sets it verbatim and no client builds a
         * policy of its own.  A record with no policy gets the strictest
         * one rather than none.
         */
        if (html != NULL) {
            g_autofree gchar *strict = (csp == NULL)
                ? clawt_mcp_app_build_csp(NULL, FALSE, NULL) : NULL;

            document = clawt_mcp_app_build_document(
                html, csp != NULL ? csp : strict);
        }

        url = clawt_mcp_app_web_url(
            clawt_config_get_string(self->config, "connectors.apps_web_url"),
            agent_id, app_id);

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "id", app_id);
        clawt_daemon_add_string_member(builder, "agent", agent_id);

        for (m = 0; members[m] != NULL; m++)
            clawt_daemon_add_string_member(
                builder, members[m],
                json_object_get_string_member_with_default(record, members[m],
                                                           NULL));

        clawt_daemon_add_string_member(builder, "document", document);
        clawt_daemon_add_string_member(builder, "csp", csp);
        clawt_daemon_add_string_member(builder, "sandbox",
                                       CLAWT_MCP_APP_IFRAME_SANDBOX);
        clawt_daemon_add_string_member(builder, "url", url);
        json_builder_set_member_name(builder, "created_at");
        json_builder_add_int_value(
            builder, json_object_get_int_member_with_default(record,
                                                             "created_at",
                                                             0));

        if (json_object_has_member(record, "refused")) {
            json_builder_set_member_name(builder, "refused");
            json_builder_add_value(builder, json_node_copy(
                json_object_get_member(record, "refused")));
        }

        if (json_object_has_member(record, "input")) {
            json_builder_set_member_name(builder, "input");
            json_builder_add_value(builder, json_node_copy(
                json_object_get_member(record, "input")));
        }

        if (json_object_has_member(record, "result")) {
            json_builder_set_member_name(builder, "result");
            json_builder_add_value(builder, json_node_copy(
                json_object_get_member(record, "result")));
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.registry_refresh") == 0) {
        g_autofree gchar *cache_path = NULL;
        const gchar *base_url;
        RegistryRefreshJob *job;

        if (!clawt_config_get_boolean(self->config,
                                      "connectors.registry_enabled"))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "connectors.registry_enabled is off; "
                                       "turn it on before asking for a "
                                       "refresh");

        if (self->registry_refreshing)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "a refresh is already in progress");

        base_url = clawt_config_get_string(self->config,
                                           "connectors.registry_url");
        cache_path = clawt_connector_registry_cache_path(self->state_dir);

        job = g_new0(RegistryRefreshJob, 1);
        job->daemon = self;
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            g_free(job);

            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        /*
         * The fetch itself never runs on this handler's stack -- it is
         * the same clawt_connector_registry_refresh_async() the periodic
         * sweep uses, which is what makes the "no IPC handler waits on
         * the network" rule true of this verb rather than merely stated
         * of it.
         */
        self->registry_refreshing = TRUE;
        clawt_connector_registry_refresh_async(
            base_url, cache_path, NULL,
            clawt_daemon_on_registry_refresh_requested, job);

        return NULL;
    }

    *handled = FALSE;
    return NULL;
}

/* ── Moving credentials into a directory each ────────────────────── */

void
clawt_daemon_connector_migrate_tokens(ClawtDaemon *self)
{
    g_autofree gchar *secrets_dir = NULL;
    GPtrArray *integrations;
    gboolean changed = FALSE;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");
    integrations = clawt_config_get_integrations(self->config);

    if (secrets_dir == NULL)
        return;

    /*
     * Before any agent starts, because a confined agent's box is built
     * from where the credential is *then*: a flat file bound into a box
     * goes on showing the inode it was, so the first renewal after the
     * box started would have been invisible inside it, and a credential
     * forgotten while it ran would have stayed readable there.
     */
    for (i = 0; integrations != NULL && i < integrations->len; i++) {
        ClawtIntegrationConfig *instance = g_ptr_array_index(integrations, i);
        const gchar *name = clawt_integration_config_get_name(instance);
        const gchar *configured;
        g_autofree gchar *legacy = NULL;
        g_autofree gchar *current = NULL;
        g_autofree gchar *dir = NULL;
        g_autoptr(GError) error = NULL;

        if (g_strcmp0(clawt_integration_config_get_type_id(instance),
                      "connector") != 0 || name == NULL)
            continue;

        configured = clawt_integration_config_get_string(instance, NULL,
                                                         "token_file");
        legacy = clawt_connector_legacy_token_path(secrets_dir, name);

        /* A path somebody chose is theirs; only the daemon's own moves. */
        if (configured == NULL || g_strcmp0(configured, legacy) != 0)
            continue;

        current = clawt_connector_token_path(secrets_dir, name);
        dir = clawt_connector_token_dir(secrets_dir, name);

        if (!clawt_ensure_dir(dir, 0700, &error)) {
            g_warning("connector '%s': cannot make %s, so its credential "
                      "stays at %s: %s", name, dir, legacy, error->message);
            continue;
        }

        /*
         * The file may already have moved -- a previous start whose
         * config save failed -- in which case only the path is left to
         * fix.  A flat file beside a newer one is the older of the two.
         */
        if (g_file_test(legacy, G_FILE_TEST_EXISTS)) {
            if (g_file_test(current, G_FILE_TEST_EXISTS)) {
                g_unlink(legacy);
            } else if (g_rename(legacy, current) != 0) {
                g_warning("connector '%s': cannot move %s to %s: %s", name,
                          legacy, current, g_strerror(errno));
                continue;
            }
        } else if (!g_file_test(current, G_FILE_TEST_EXISTS)) {
            continue;
        }

        clawt_integration_config_set_string(instance, NULL, "token_file",
                                            current);
        changed = TRUE;
    }

    if (changed) {
        g_autoptr(GError) error = NULL;

        if (!clawt_config_save(self->config, &error))
            g_warning("moved connector credentials into %s/connectors, but "
                      "the configuration naming them could not be saved: "
                      "%s; this daemon uses the new paths, and the next "
                      "start tries again", secrets_dir, error->message);
    }
}
