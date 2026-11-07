/*
 * daemon-search.c - The libresearch server each agent may search through
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent's `libresearch` integration names a server; the clawtilla_web_*
 * tools reach it through the client built here.  The tools ask this one
 * hook both whether to offer themselves and what to call, so "offered"
 * and "works" cannot disagree about which agents have a server.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include <ai-glib.h>

/*
 * The agent's libresearch binding, or NULL.  An integration is
 * one_per_agent, so the first is the only one.
 */
static ClawtIntegrationBinding *
libresearch_binding(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(GPtrArray) bindings = NULL;
    guint i;

    if (agent == NULL || clawt_agent_config_is_shadow(agent))
        return NULL;

    bindings = clawt_integration_resolve_for_agent(self->config, agent);
    for (i = 0; bindings != NULL && i < bindings->len; i++) {
        ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

        if (g_strcmp0(clawt_integration_binding_get_info(binding)->id, "libresearch") == 0)
            return clawt_integration_binding_ref(binding);
    }

    return NULL;
}

static AiLibresearchSearch *
search_for_agent(const gchar *agent_id, gpointer user_data, GError **error)
{
    ClawtDaemon *self = user_data;
    ClawtIntegrationBinding *binding = NULL;
    g_autoptr(ClawtSecretRef) ref = NULL;
    g_autofree gchar *key = NULL;
    g_autofree gchar *token = NULL;
    const gchar *url;
    AiLibresearchSearch *client;

    binding = libresearch_binding(self, agent_id);
    if (binding == NULL)
        return NULL;

    url = clawt_integration_binding_get_string(binding, "url");
    ref = clawt_integration_binding_get_secret(binding, "token");
    key = g_strdup_printf("%s\x1f%s\x1f%s", agent_id, url != NULL ? url : "",
                          ref != NULL ? clawt_secret_ref_get_locator(ref) : "");

    client = g_hash_table_lookup(self->search_clients, key);
    if (client != NULL) {
        clawt_integration_binding_unref(binding);
        return g_object_ref(client);
    }

    if (ref != NULL) {
        g_autofree gchar *secrets_dir =
            clawt_config_get_path_value(self->config, "secrets.dir");

        token = clawt_secret_ref_resolve(
            ref, secrets_dir,
            (guint)clawt_config_get_int(self->config, "secrets.command_timeout_seconds"),
            error);
        if (token == NULL) {
            g_prefix_error(error, "the libresearch token for integration '%s': ",
                           clawt_integration_binding_get_name(binding));
            clawt_integration_binding_unref(binding);
            return NULL;
        }
    }

    client = ai_libresearch_search_new(url, token);
    g_hash_table_insert(self->search_clients, g_steal_pointer(&key), g_object_ref(client));
    clawt_integration_binding_unref(binding);

    return client;
}

void
clawt_daemon_search_setup(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->search_clients == NULL)
        self->search_clients = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, g_object_unref);

    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_search_func(self->mcp_tools, search_for_agent, self, NULL);
}

void
clawt_daemon_search_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    /* The tools lose the hook before the clients it hands out go. */
    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_search_func(self->mcp_tools, NULL, NULL, NULL);
    g_clear_pointer(&self->search_clients, g_hash_table_unref);
}
