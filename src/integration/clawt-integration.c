/*
 * clawt-integration.c - How an agent reaches the world
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-integration.h"

#include <gio/gio.h>

#include <string.h>

static const gchar *const matrix_required[] = {
    "homeserver", "user_id", NULL
};

static const gchar *const matrix_credentials[] = {
    "access_token", NULL
};

static const gchar *const email_required[] = {
    "imap_host", "smtp_host", "username", NULL
};

static const gchar *const email_credentials[] = {
    "password", NULL
};

static const gchar *const webhook_required[] = {
    "port", NULL
};

static const ClawtIntegrationInfo integrations[] = {
    { "matrix",
      "Chat over Matrix, including bridged Discord and Signal rooms.",
      matrix_required, matrix_credentials, "matrix" },

    { "email",
      "Receive over IMAP and reply over SMTP.",
      email_required, email_credentials, "email" },

    { "webhook",
      "Accept HTTP posts from other services.",
      webhook_required, NULL, "webhook" },

    { "local",
      "Read from the terminal. Only for an agent you run by hand: it owns "
      "stdin and stdout, so two agents with it would fight over them.",
      NULL, NULL, "local" },

    { "cmacs",
      "Talk to an Emacs session in-process.",
      NULL, NULL, "cmacs" }
};

const ClawtIntegrationInfo *
clawt_integration_list(gsize *n_integrations)
{
    g_return_val_if_fail(n_integrations != NULL, NULL);

    *n_integrations = G_N_ELEMENTS(integrations);

    return integrations;
}

const ClawtIntegrationInfo *
clawt_integration_find(const gchar *id)
{
    gsize i;

    if (id == NULL)
        return NULL;

    for (i = 0; i < G_N_ELEMENTS(integrations); i++) {
        if (g_strcmp0(integrations[i].id, id) == 0)
            return &integrations[i];
    }

    return NULL;
}

gboolean
clawt_integration_is_enabled(ClawtAgentConfig *agent, const gchar *id)
{
    g_autofree gchar *key = NULL;

    g_return_val_if_fail(agent != NULL, FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    /*
     * `local` and `cmacs` are plain booleans rather than blocks, because
     * they have nothing to configure.  Both spellings are accepted so
     * neither form is a mistake.
     */
    key = g_strdup_printf("integrations.%s", id);

    if (clawt_agent_config_has_key(agent, key) &&
        clawt_agent_config_get_boolean(agent, key))
        return TRUE;

    g_free(key);
    key = g_strdup_printf("integrations.%s.enabled", id);

    return clawt_agent_config_get_boolean(agent, key);
}

GStrv
clawt_integration_enabled_for(ClawtAgentConfig *agent)
{
    g_autoptr(GPtrArray) out = NULL;
    gsize i;

    g_return_val_if_fail(agent != NULL, NULL);

    out = g_ptr_array_new();

    for (i = 0; i < G_N_ELEMENTS(integrations); i++) {
        if (clawt_integration_is_enabled(agent, integrations[i].id))
            g_ptr_array_add(out, g_strdup(integrations[i].id));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

gboolean
clawt_integration_validate(ClawtAgentConfig *agent, GError **error)
{
    gsize i;

    g_return_val_if_fail(agent != NULL, FALSE);

    for (i = 0; i < G_N_ELEMENTS(integrations); i++) {
        const ClawtIntegrationInfo *info = &integrations[i];
        gsize j;

        if (!clawt_integration_is_enabled(agent, info->id))
            continue;

        for (j = 0; info->required_keys != NULL &&
                    info->required_keys[j] != NULL; j++) {
            g_autofree gchar *key = g_strdup_printf(
                "integrations.%s.%s", info->id, info->required_keys[j]);

            if (clawt_agent_config_has_key(agent, key))
                continue;

            /*
             * Named in full, because the failure this prevents is silent:
             * an agent whose Matrix block is missing its homeserver starts
             * cleanly and simply never receives anything.
             */
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "%s: integrations.%s is enabled but %s is not set",
                        clawt_agent_config_get_id(agent), info->id, key);
            return FALSE;
        }

        for (j = 0; info->credential_keys != NULL &&
                    info->credential_keys[j] != NULL; j++) {
            g_autofree gchar *key = g_strdup_printf(
                "integrations.%s.%s", info->id, info->credential_keys[j]);
            g_autoptr(ClawtSecretRef) ref = NULL;

            ref = clawt_agent_config_get_secret(agent, key);

            if (ref == NULL) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "%s: integrations.%s needs %s -- a secret "
                            "reference such as {env: NAME}, {file: PATH} "
                            "or {command: \"...\"}",
                            clawt_agent_config_get_id(agent), info->id, key);
                return FALSE;
            }
        }
    }

    return TRUE;
}

/*
 * Opens a TCP connection and closes it again.
 *
 * The point is to catch the failures people actually hit -- a typo in a
 * hostname, a port nothing is listening on, a firewall -- before an agent
 * starts and quietly does nothing.
 */
static gboolean
can_reach(const gchar *host, guint16 port, guint timeout_seconds,
          GError **error)
{
    g_autoptr(GSocketClient) client = NULL;
    g_autoptr(GSocketConnection) connection = NULL;

    if (host == NULL || port == 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no host or port to check");
        return FALSE;
    }

    client = g_socket_client_new();
    g_socket_client_set_timeout(client,
                                timeout_seconds > 0 ? timeout_seconds : 10);

    connection = g_socket_client_connect_to_host(client, host, port, NULL,
                                                 error);

    if (connection == NULL) {
        g_prefix_error(error, "could not reach %s:%u: ", host, port);
        return FALSE;
    }

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    return TRUE;
}

gboolean
clawt_integration_health_check(ClawtAgentConfig *agent, const gchar *id,
                               guint timeout_seconds, GError **error)
{
    const ClawtIntegrationInfo *info;

    g_return_val_if_fail(agent != NULL, FALSE);

    info = clawt_integration_find(id);

    if (info == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no integration called '%s'", id);
        return FALSE;
    }

    if (!clawt_integration_is_enabled(agent, id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "%s does not have the %s integration enabled",
                    clawt_agent_config_get_id(agent), id);
        return FALSE;
    }

    if (g_strcmp0(id, "matrix") == 0) {
        const gchar *homeserver = clawt_agent_config_get_string(
            agent, "integrations.matrix.homeserver");
        g_autoptr(GUri) uri = NULL;

        if (homeserver == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "no homeserver is configured");
            return FALSE;
        }

        uri = g_uri_parse(homeserver, G_URI_FLAGS_NONE, error);

        if (uri == NULL)
            return FALSE;

        return can_reach(g_uri_get_host(uri),
                         (guint16)(g_uri_get_port(uri) > 0
                                   ? g_uri_get_port(uri)
                                   : (g_strcmp0(g_uri_get_scheme(uri),
                                                "http") == 0 ? 80 : 443)),
                         timeout_seconds, error);
    }

    if (g_strcmp0(id, "email") == 0) {
        /*
         * Both directions are checked.  An agent that can read mail but
         * cannot send looks like it is ignoring people.
         */
        if (!can_reach(clawt_agent_config_get_string(
                           agent, "integrations.email.imap_host"),
                       (guint16)clawt_agent_config_get_int(
                           agent, "integrations.email.imap_port"),
                       timeout_seconds, error))
            return FALSE;

        return can_reach(clawt_agent_config_get_string(
                             agent, "integrations.email.smtp_host"),
                         (guint16)clawt_agent_config_get_int(
                             agent, "integrations.email.smtp_port"),
                         timeout_seconds, error);
    }

    if (g_strcmp0(id, "webhook") == 0) {
        guint16 port = (guint16)clawt_agent_config_get_int(
            agent, "integrations.webhook.port");
        g_autoptr(GSocketListener) listener = g_socket_listener_new();
        g_autoptr(GError) local = NULL;

        /*
         * Checked by trying to bind rather than by connecting: for a
         * listener, the failure that matters is the port already being
         * taken.  Binding and immediately closing is the only way to find
         * that out without starting the agent.
         */
        if (!g_socket_listener_add_inet_port(listener, port, NULL, &local)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "port %u is not available: %s", port, local->message);
            return FALSE;
        }

        g_socket_listener_close(listener);

        return TRUE;
    }

    /* local and cmacs need nothing outside the process. */
    return TRUE;
}
