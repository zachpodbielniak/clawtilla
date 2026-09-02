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

/* ── The type table ──────────────────────────────────────────────── */

static const gchar *const matrix_required[] = {
    "homeserver", "user_id", NULL
};

static const gchar *const matrix_credentials[] = {
    "access_token", NULL
};

/*
 * Two agents on one Matrix login receive each other's messages and answer
 * as the same person.  The account is the identity, so both halves of it
 * have to differ.
 */
static const gchar *const matrix_identity[] = {
    "user_id", "access_token", NULL
};

static const gchar *const email_required[] = {
    "imap_host", "smtp_host", "username", NULL
};

static const gchar *const email_credentials[] = {
    "password", NULL
};

static const gchar *const email_identity[] = {
    "username", NULL
};

static const gchar *const webhook_required[] = {
    "port", NULL
};

/* Two agents cannot bind one port; the second to start simply fails. */
static const gchar *const webhook_identity[] = {
    "port", NULL
};

static const gchar *const connector_required[] = {
    "provider", NULL
};

static const ClawtIntegrationInfo integrations[] = {
    { "matrix", CLAWT_INTEGRATION_KIND_CHANNEL,
      "Chat over Matrix, including bridged Discord and Signal rooms.",
      matrix_required, matrix_credentials, matrix_identity,
      "matrix", TRUE, FALSE },

    { "email", CLAWT_INTEGRATION_KIND_CHANNEL,
      "Receive over IMAP and reply over SMTP.",
      email_required, email_credentials, email_identity,
      "email", TRUE, FALSE },

    { "webhook", CLAWT_INTEGRATION_KIND_CHANNEL,
      "Accept HTTP posts from other services.",
      webhook_required, NULL, webhook_identity,
      "webhook", TRUE, FALSE },

    { "local", CLAWT_INTEGRATION_KIND_CHANNEL,
      "Read from the terminal. Only for an agent you run by hand: it owns "
      "stdin and stdout, so two agents with it would fight over them.",
      NULL, NULL, NULL,
      "local", TRUE, TRUE },

    { "cmacs", CLAWT_INTEGRATION_KIND_CHANNEL,
      "Talk to an Emacs session in-process.",
      NULL, NULL, NULL,
      "cmacs", TRUE, FALSE },

    /*
     * The general one.  Anything with an MCP server -- a hosted connector,
     * somebody's stdio binary, a service behind an HTTP endpoint -- reaches
     * the fleet through this rather than through a type of its own, which
     * is what makes "add support for X" a config entry instead of a patch.
     *
     * Not one_per_agent: several tool servers on one agent is the ordinary
     * case, and each gets its own key in the agent's .mcp.json.
     */
    { "mcp", CLAWT_INTEGRATION_KIND_TOOLS,
      "Give agents the tools of any MCP server, by command or by URL.",
      NULL, NULL, NULL,
      NULL, FALSE, FALSE },

    /*
     * The brokered one.
     *
     * Same shape as `mcp` from the agent's side -- tools appear in its
     * .mcp.json and it calls them -- and completely different on this
     * side: the daemon obtains the credential, keeps it, renews it and
     * injects it, so the value never reaches the agent's config, its
     * environment or its transcript.  That is the whole difference
     * between handing somebody a key and unlocking the door for them.
     *
     * Not one_per_agent: an agent with a GitHub account and a calendar
     * has two, and each gets its own key in the agent's .mcp.json.  Not
     * identity-keyed either, because sharing one is the point -- a
     * fleet-wide connector is a single account the whole fleet reads
     * through, which is what somebody asking for "give all of them
     * GitHub" means.
     */
    { "connector", CLAWT_INTEGRATION_KIND_TOOLS,
      "A connected account: clawtilla holds the credential, agents use it.",
      connector_required, NULL, NULL,
      NULL, FALSE, FALSE },

    /*
     * The one that runs in neither direction an agent can see: the
     * daemon telling a person something, about an agent, without the
     * agent being involved or knowing it happened.
     *
     * Not one_per_agent -- a desktop notification while you are at the
     * machine and a phone push while you are not is two instances doing
     * their jobs, not a conflict.
     */
    { "notify", CLAWT_INTEGRATION_KIND_NOTIFY,
      "Tell you when an agent is blocked on you or has broken.",
      NULL, NULL, NULL,
      NULL, FALSE, FALSE }
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

/* ── Bindings ────────────────────────────────────────────────────── */

struct _ClawtIntegrationBinding {
    gint                        ref_count;

    const ClawtIntegrationInfo *info;
    gchar                      *name;
    gchar                      *agent_id;

    /* Exactly one of these is set. */
    ClawtIntegrationConfig     *instance;   /* a shared, named instance */
    ClawtAgentConfig           *agent;      /* an inline block */
};

static ClawtIntegrationBinding *
binding_new(const ClawtIntegrationInfo *info,
            const gchar                *name,
            const gchar                *agent_id)
{
    ClawtIntegrationBinding *self = g_new0(ClawtIntegrationBinding, 1);

    self->ref_count = 1;
    self->info = info;
    self->name = g_strdup(name);
    self->agent_id = g_strdup(agent_id);

    return self;
}

ClawtIntegrationBinding *
clawt_integration_binding_ref(ClawtIntegrationBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);

    return self;
}

void
clawt_integration_binding_unref(ClawtIntegrationBinding *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_clear_pointer(&self->instance, clawt_integration_config_unref);
    g_clear_pointer(&self->agent, clawt_agent_config_unref);
    g_free(self->name);
    g_free(self->agent_id);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtIntegrationBinding, clawt_integration_binding,
                    clawt_integration_binding_ref,
                    clawt_integration_binding_unref)

ClawtIntegrationBinding *
clawt_integration_binding_for_instance(ClawtIntegrationConfig     *instance,
                                       const ClawtIntegrationInfo *info,
                                       const gchar                *agent_id)
{
    ClawtIntegrationBinding *self;

    g_return_val_if_fail(instance != NULL, NULL);
    g_return_val_if_fail(info != NULL, NULL);

    self = binding_new(info, clawt_integration_config_get_name(instance),
                       agent_id);
    self->instance = clawt_integration_config_ref(instance);

    return self;
}

const gchar *
clawt_integration_binding_get_name(ClawtIntegrationBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->name;
}

const ClawtIntegrationInfo *
clawt_integration_binding_get_info(ClawtIntegrationBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->info;
}

const gchar *
clawt_integration_binding_get_agent_id(ClawtIntegrationBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->agent_id;
}

gboolean
clawt_integration_binding_is_shared(ClawtIntegrationBinding *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->instance != NULL;
}

/*
 * The path an inline key lives at.
 *
 * An agent's own block is `integrations.<type>.<key>`; an instance's is
 * just `<key>`, because the instance mapping *is* the integration.
 */
static gchar *
inline_key(ClawtIntegrationBinding *self, const gchar *key)
{
    return g_strdup_printf("integrations.%s.%s", self->info->id, key);
}

const gchar *
clawt_integration_binding_get_string(ClawtIntegrationBinding *self,
                                     const gchar             *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    if (self->instance != NULL)
        return clawt_integration_config_get_string(self->instance,
                                                   self->agent_id, key);

    {
        g_autofree gchar *path = inline_key(self, key);

        return clawt_agent_config_get_string(self->agent, path);
    }
}

gboolean
clawt_integration_binding_get_boolean(ClawtIntegrationBinding *self,
                                      const gchar             *key)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (self->instance != NULL)
        return clawt_integration_config_get_boolean(self->instance,
                                                    self->agent_id, key);

    {
        g_autofree gchar *path = inline_key(self, key);

        return clawt_agent_config_get_boolean(self->agent, path);
    }
}

gint64
clawt_integration_binding_get_int(ClawtIntegrationBinding *self,
                                  const gchar             *key)
{
    g_return_val_if_fail(self != NULL, 0);
    g_return_val_if_fail(key != NULL, 0);

    if (self->instance != NULL)
        return clawt_integration_config_get_int(self->instance,
                                                self->agent_id, key);

    {
        g_autofree gchar *path = inline_key(self, key);

        return clawt_agent_config_get_int(self->agent, path);
    }
}

GStrv
clawt_integration_binding_get_string_list(ClawtIntegrationBinding *self,
                                          const gchar             *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    if (self->instance != NULL)
        return clawt_integration_config_get_string_list(self->instance,
                                                        self->agent_id, key);

    {
        g_autofree gchar *path = inline_key(self, key);

        return clawt_agent_config_get_string_list(self->agent, path);
    }
}

GHashTable *
clawt_integration_binding_get_mapping(ClawtIntegrationBinding *self,
                                      const gchar             *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    if (self->instance != NULL)
        return clawt_integration_config_get_mapping(self->instance,
                                                    self->agent_id, key);

    /*
     * An inline block has no mapping getter of its own, and adding one to
     * ClawtAgentConfig for the single caller here would be a second way to
     * read the same file.  An empty table is right: the only mapping key
     * any type has is `mcp.env`, and `mcp` is only ever a named instance.
     */
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

ClawtSecretRef *
clawt_integration_binding_get_secret(ClawtIntegrationBinding *self,
                                     const gchar             *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    if (self->instance != NULL)
        return clawt_integration_config_get_secret(self->instance,
                                                   self->agent_id, key);

    {
        g_autofree gchar *path = inline_key(self, key);

        return clawt_agent_config_get_secret(self->agent, path);
    }
}

GHashTable *
clawt_integration_binding_resolve_env(ClawtIntegrationBinding  *self,
                                      const gchar              *key,
                                      const gchar              *secrets_dir,
                                      GError                  **error)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    if (self->instance != NULL)
        return clawt_integration_config_resolve_env(self->instance,
                                                    self->agent_id, key,
                                                    secrets_dir, error);

    /* Only `mcp` has an env, and `mcp` is only ever a named instance. */
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

gboolean
clawt_integration_binding_has_key(ClawtIntegrationBinding *self,
                                  const gchar             *key)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    if (self->instance != NULL)
        return clawt_integration_config_has_key(self->instance,
                                                self->agent_id, key);

    {
        g_autofree gchar *path = inline_key(self, key);

        return clawt_agent_config_has_key(self->agent, path);
    }
}

/*
 * How a key is spelled in an error message.
 *
 * Worth the branch: "integrations.matrix.homeserver" and "the 'home'
 * integration's homeserver" are different places in the file, and a
 * message naming the wrong one sends somebody to edit a block that is
 * already correct.
 */
static gchar *
describe_key(ClawtIntegrationBinding *self, const gchar *key)
{
    if (self->instance != NULL)
        return g_strdup_printf("integration '%s': %s", self->name, key);

    return g_strdup_printf("integrations.%s.%s", self->info->id, key);
}

gboolean
clawt_integration_binding_key_is_credential(ClawtIntegrationBinding *self,
                                            const gchar             *key)
{
    gsize i;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    for (i = 0; self->info->credential_keys != NULL &&
                self->info->credential_keys[i] != NULL; i++) {
        if (g_strcmp0(self->info->credential_keys[i], key) == 0)
            return TRUE;
    }

    return FALSE;
}

gchar *
clawt_integration_binding_describe_key(ClawtIntegrationBinding *self,
                                       const gchar             *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    return describe_key(self, key);
}

gboolean
clawt_integration_binding_validate(ClawtIntegrationBinding  *self,
                                   GError                  **error)
{
    gsize i;

    g_return_val_if_fail(self != NULL, FALSE);

    for (i = 0; self->info->required_keys != NULL &&
                self->info->required_keys[i] != NULL; i++) {
        if (clawt_integration_binding_has_key(self,
                                              self->info->required_keys[i]))
            continue;

        {
            g_autofree gchar *where =
                describe_key(self, self->info->required_keys[i]);

            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "%s: %s is not set", self->agent_id, where);
        }

        return FALSE;
    }

    for (i = 0; self->info->credential_keys != NULL &&
                self->info->credential_keys[i] != NULL; i++) {
        g_autoptr(ClawtSecretRef) ref = NULL;

        ref = clawt_integration_binding_get_secret(
            self, self->info->credential_keys[i]);

        if (ref != NULL)
            continue;

        {
            g_autofree gchar *where =
                describe_key(self, self->info->credential_keys[i]);

            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "%s: %s needs a secret reference such as "
                        "{env: NAME}, {file: PATH} or {command: \"...\"}",
                        self->agent_id, where);
        }

        return FALSE;
    }

    /*
     * An MCP server is either a command or a URL.  Neither means there is
     * nothing to start; both means two different servers with no way to
     * tell which was meant, and picking one would be a guess about which
     * of the agent's tools are real.
     */
    if (g_strcmp0(self->info->id, "mcp") == 0 ||
        g_strcmp0(self->info->id, "connector") == 0) {
        gboolean has_command =
            clawt_integration_binding_get_string(self, "command") != NULL;
        gboolean has_url =
            clawt_integration_binding_get_string(self, "url") != NULL;
        /*
         * A connector may name neither, and usually does: the catalogue
         * entry for its provider says which server fronts the service.
         * An `mcp` integration has no catalogue to fall back on, so
         * naming neither leaves nothing to start.
         */
        gboolean from_catalog =
            g_strcmp0(self->info->id, "connector") == 0;

        if (!has_command && !has_url && !from_catalog) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "integration '%s': set either command or url",
                        self->name);
            return FALSE;
        }

        if (has_command && has_url) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "integration '%s': command and url are two "
                        "different servers; set one",
                        self->name);
            return FALSE;
        }
    }

    return TRUE;
}

/* ── Legacy inline blocks ────────────────────────────────────────── */

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
        g_autoptr(ClawtIntegrationBinding) binding = NULL;

        if (!clawt_integration_is_enabled(agent, integrations[i].id))
            continue;

        binding = binding_new(&integrations[i], integrations[i].id,
                              clawt_agent_config_get_id(agent));
        binding->agent = clawt_agent_config_ref(agent);

        if (!clawt_integration_binding_validate(binding, error))
            return FALSE;
    }

    return TRUE;
}

/* ── Resolution ──────────────────────────────────────────────────── */

ClawtIntegrationBinding *
clawt_integration_find_binding(GPtrArray *bindings, const gchar *type_id)
{
    guint i;

    g_return_val_if_fail(bindings != NULL, NULL);
    g_return_val_if_fail(type_id != NULL, NULL);

    for (i = 0; i < bindings->len; i++) {
        ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

        if (g_strcmp0(binding->info->id, type_id) == 0)
            return binding;
    }

    return NULL;
}

GPtrArray *
clawt_integration_resolve_for_agent(ClawtConfig      *config,
                                    ClawtAgentConfig *agent)
{
    GPtrArray *out;
    GPtrArray *instances;
    const gchar *agent_id;
    gsize i;
    guint j;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
    g_return_val_if_fail(agent != NULL, NULL);

    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_integration_binding_unref);
    agent_id = clawt_agent_config_get_id(agent);

    /*
     * The agent's own blocks first.
     *
     * Order is the conflict rule: an inline block is the more specific
     * statement -- somebody wrote it inside this agent -- so when both it
     * and a shared instance claim the one Matrix channel an agent can
     * have, the inline one wins and the instance is reported.
     */
    for (i = 0; i < G_N_ELEMENTS(integrations); i++) {
        ClawtIntegrationBinding *binding;

        if (!clawt_integration_is_enabled(agent, integrations[i].id))
            continue;

        binding = binding_new(&integrations[i], integrations[i].id, agent_id);
        binding->agent = clawt_agent_config_ref(agent);
        g_ptr_array_add(out, binding);
    }

    instances = clawt_config_get_integrations(config);

    for (j = 0; instances != NULL && j < instances->len; j++) {
        ClawtIntegrationConfig *instance = g_ptr_array_index(instances, j);
        const ClawtIntegrationInfo *info;
        ClawtIntegrationBinding *binding;

        if (!clawt_integration_config_covers(instance, agent_id))
            continue;

        info = clawt_integration_find(
            clawt_integration_config_get_type_id(instance));

        if (info == NULL) {
            /*
             * An unknown type disables that instance and nothing else --
             * the same treatment a shadow agent gets, so a config written
             * by a newer clawtilla still loads in an older one.
             */
            g_warning("integration '%s' has an unknown type '%s'; ignored",
                      clawt_integration_config_get_name(instance),
                      clawt_integration_config_get_type_id(instance));
            continue;
        }

        if (info->one_per_agent &&
            clawt_integration_find_binding(out, info->id) != NULL) {
            ClawtIntegrationBinding *held =
                clawt_integration_find_binding(out, info->id);

            /*
             * Named loudly rather than merged.  libreclaw renders one
             * `channels.<type>` block per agent, so a second one has
             * nowhere to go: the account would look configured in the
             * file and receive nothing for ever.
             */
            g_warning("%s already has a %s integration from %s, so '%s' is "
                      "not applied to it",
                      agent_id, info->id,
                      clawt_integration_binding_is_shared(held)
                          ? "another shared integration"
                          : "its own configuration",
                      clawt_integration_config_get_name(instance));
            continue;
        }

        binding = binding_new(info,
                              clawt_integration_config_get_name(instance),
                              agent_id);
        binding->instance = clawt_integration_config_ref(instance);
        g_ptr_array_add(out, binding);
    }

    return out;
}

/* ── Fleet-wide validation ───────────────────────────────────────── */

/*
 * Records one agent's value for an identity key and complains if another
 * agent already had it.
 *
 * Keyed on type, key and value together: two agents sharing a Matrix
 * user_id is a collision, and one of them having the same string as the
 * other's email username is not.
 */
static void
check_identity(GHashTable  *seen,
               GPtrArray   *warnings,
               const gchar *type_id,
               const gchar *key,
               const gchar *value,
               const gchar *agent_id,
               const gchar *name,
               const gchar *note)
{
    g_autofree gchar *slot = NULL;
    const gchar *previous;

    if (value == NULL || *value == '\0')
        return;

    slot = g_strdup_printf("%s\n%s\n%s", type_id, key, value);
    previous = g_hash_table_lookup(seen, slot);

    if (previous == NULL) {
        g_hash_table_insert(seen, g_steal_pointer(&slot), g_strdup(agent_id));
        return;
    }

    if (warnings == NULL)
        return;

    /*
     * @note is what the sharing actually costs, supplied by whoever knew
     * -- a connector entry knows that its service audits by actor, and
     * the generic sentence below does not.  Without it the warning is an
     * instruction with no reason attached, which reads as pedantry and
     * gets turned off.
     */
    if (note != NULL)
        g_ptr_array_add(warnings, g_strdup_printf(
            "integration '%s': %s and %s share one %s -- give each its own "
            "under per_agent. %s",
            name, previous, agent_id, key, note));
    else
        g_ptr_array_add(warnings, g_strdup_printf(
            "integration '%s': %s and %s share one %s -- give each its own "
            "under per_agent, or they will both answer as the same account",
            name, previous, agent_id, key));
}

/*
 * A connector's own identity keys, which are not its integration type's.
 *
 * The `connector` type declares none on purpose: a fleet-wide GitHub
 * account read through by every agent is what "give all of them GitHub"
 * means, and warning about it would be warning about the feature.  But
 * that is a fact about GitHub rather than about connectors, and a
 * service that records *who* did something has the opposite answer.  So
 * the entry says, and this is the one place that asks it.
 *
 * The catalogue is loaded here rather than passed in because the fleet
 * validation's only caller has no catalogue to give it, and the overlay
 * directory is named by the same config this is already holding -- an
 * entry somebody added in connectors.d is checked exactly like a
 * built-in one.
 */
static void
check_connector_identity(GPtrArray               *catalog,
                         GHashTable              *seen,
                         GPtrArray               *warnings,
                         ClawtIntegrationBinding *binding,
                         const gchar             *agent_id)
{
    const ClawtConnectorInfo *info;
    const gchar *provider;
    g_autofree gchar *slot_type = NULL;
    gsize k;

    if (catalog == NULL)
        return;

    provider = clawt_integration_binding_get_string(binding, "provider");
    info = clawt_connector_catalog_find(catalog, provider);

    if (info == NULL || info->identity_keys == NULL)
        return;

    /*
     * Keyed by provider as well as by type, so two agents sharing a
     * venture token collide and an agent whose Forgejo token file
     * happens to have the same name does not.
     */
    slot_type = g_strdup_printf("connector:%s", info->id);

    for (k = 0; info->identity_keys[k] != NULL; k++) {
        const gchar *key = info->identity_keys[k];

        check_identity(seen, warnings, slot_type, key,
                       clawt_integration_binding_get_string(binding, key),
                       agent_id,
                       clawt_integration_binding_get_name(binding),
                       info->identity_note);
    }
}

gboolean
clawt_integration_validate_fleet(ClawtConfig *config, GPtrArray **warnings)
{
    g_autoptr(GHashTable) identities = NULL;
    g_autoptr(GHashTable) fleet_exclusive = NULL;
    g_autoptr(GPtrArray) catalog = NULL;
    g_autofree gchar *overlay_dir = NULL;
    GPtrArray *agents;
    GPtrArray *instances;
    GPtrArray *found;
    guint i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), FALSE);

    found = g_ptr_array_new_with_free_func(g_free);
    overlay_dir = clawt_config_get_path_value(config, "connectors.dir");
    catalog = clawt_connector_catalog_load(overlay_dir, NULL);
    identities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fleet_exclusive = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);

    agents = clawt_config_get_agents(config);
    instances = clawt_config_get_integrations(config);

    /*
     * An instance naming an agent that is not there.
     *
     * A warning rather than an error: an agent removed for the afternoon
     * should not stop the daemon, and the message is the only way anybody
     * finds out the name was misspelt.
     */
    for (i = 0; instances != NULL && i < instances->len; i++) {
        ClawtIntegrationConfig *instance = g_ptr_array_index(instances, i);
        g_auto(GStrv) named = NULL;
        guint k;

        if (clawt_integration_config_is_shadow(instance)) {
            g_ptr_array_add(found, g_strdup_printf(
                "integration '%s' is disabled: %s",
                clawt_integration_config_get_name(instance),
                clawt_integration_config_get_shadow_reason(instance)));
            continue;
        }

        if (clawt_integration_find(
                clawt_integration_config_get_type_id(instance)) == NULL) {
            g_ptr_array_add(found, g_strdup_printf(
                "integration '%s' has an unknown type '%s'",
                clawt_integration_config_get_name(instance),
                clawt_integration_config_get_type_id(instance)));
            continue;
        }

        if (clawt_integration_config_get_scope(instance) !=
            CLAWT_SCOPE_SELECTED)
            continue;

        named = clawt_integration_config_get_agents(instance);

        for (k = 0; named != NULL && named[k] != NULL; k++) {
            if (clawt_config_get_agent(config, named[k]) != NULL)
                continue;

            g_ptr_array_add(found, g_strdup_printf(
                "integration '%s' names '%s', which is not an agent",
                clawt_integration_config_get_name(instance), named[k]));
        }
    }

    /* Then everything only visible once the bindings are resolved. */
    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
        g_autoptr(GPtrArray) bindings = NULL;
        const gchar *agent_id = clawt_agent_config_get_id(agent);
        guint b;

        bindings = clawt_integration_resolve_for_agent(config, agent);

        for (b = 0; b < bindings->len; b++) {
            ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, b);
            g_autoptr(GError) error = NULL;
            gsize k;

            if (!clawt_integration_binding_validate(binding, &error))
                g_ptr_array_add(found, g_strdup(error->message));

            if (binding->info->one_per_fleet) {
                const gchar *previous =
                    g_hash_table_lookup(fleet_exclusive, binding->info->id);

                if (previous != NULL)
                    g_ptr_array_add(found, g_strdup_printf(
                        "%s and %s both have the %s integration, and only "
                        "one agent may",
                        previous, agent_id, binding->info->id));
                else
                    g_hash_table_insert(fleet_exclusive,
                                        g_strdup(binding->info->id),
                                        g_strdup(agent_id));
            }

            /*
             * Identity collisions only matter for a shared instance.  Two
             * agents with their own inline Matrix blocks pointing at one
             * account is odd, but it is two deliberate statements rather
             * than one instance quietly covering both.
             */
            if (!clawt_integration_binding_is_shared(binding))
                continue;

            for (k = 0; binding->info->identity_keys != NULL &&
                        binding->info->identity_keys[k] != NULL; k++) {
                const gchar *key = binding->info->identity_keys[k];

                check_identity(identities, found, binding->info->id, key,
                               clawt_integration_binding_get_string(binding,
                                                                    key),
                               agent_id,
                               clawt_integration_binding_get_name(binding),
                               NULL);
            }

            /* And the connector's own, which the type does not have. */
            if (g_strcmp0(binding->info->id, "connector") == 0)
                check_connector_identity(catalog, identities, found, binding,
                                         agent_id);
        }
    }

    if (warnings != NULL)
        *warnings = found;
    else
        g_ptr_array_unref(found);

    return found->len == 0;
}

/* ── Health ──────────────────────────────────────────────────────── */

/*
 * What one check is waiting on.
 *
 * It carries the binding because the #GTask cannot: g_task_new() takes a
 * #GObject as its source and refs it, and a binding is a plain
 * reference-counted struct -- handing one over anyway ran g_object_ref()
 * on a pointer that is not a GObject and took the daemon down on the
 * first health check anybody asked for.
 */
typedef struct {
    ClawtIntegrationBinding *binding;   /* owned */
    gchar                   *host;
    guint16                  port;
    guint                    timeout;
} Reach;

static void
reach_free(Reach *self)
{
    if (self == NULL)
        return;

    g_clear_pointer(&self->binding, clawt_integration_binding_unref);
    g_free(self->host);
    g_free(self);
}

/*
 * The task data, kept across both legs of an email check.
 *
 * Replacing it would drop the binding the second leg needs, so the
 * host and port are updated in place instead.
 */
static Reach *
reach_for(GTask *task, ClawtIntegrationBinding *binding)
{
    Reach *reach = g_task_get_task_data(task);

    if (reach != NULL)
        return reach;

    reach = g_new0(Reach, 1);
    reach->binding = clawt_integration_binding_ref(binding);
    g_task_set_task_data(task, reach, (GDestroyNotify)reach_free);

    return reach;
}

static void
on_connected(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    Reach *reach = g_task_get_task_data(task);

    connection = g_socket_client_connect_to_host_finish(
        G_SOCKET_CLIENT(source), result, &error);

    if (connection == NULL) {
        g_prefix_error(&error, "could not reach %s:%u: ",
                       reach->host, reach->port);
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_task_return_boolean(task, TRUE);
}

/*
 * Opens a TCP connection and closes it again, without waiting.
 *
 * The point is to catch the failures people actually hit -- a typo in a
 * hostname, a port nothing is listening on, a firewall -- before an agent
 * starts and quietly does nothing.
 */
static void
reach_async(GTask *task, ClawtIntegrationBinding *binding,
            const gchar *host, guint16 port, guint timeout)
{
    g_autoptr(GSocketClient) client = NULL;
    Reach *reach;

    if (host == NULL || port == 0) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "no host or port to check");
        g_object_unref(task);
        return;
    }

    reach = reach_for(task, binding);
    g_free(reach->host);
    reach->host = g_strdup(host);
    reach->port = port;
    reach->timeout = timeout;

    client = g_socket_client_new();
    g_socket_client_set_timeout(client, timeout > 0 ? timeout : 10);

    g_socket_client_connect_to_host_async(client, host, port,
                                          g_task_get_cancellable(task),
                                          on_connected, task);
}

/*
 * The second half of an email check, run once IMAP has answered.
 *
 * Both directions are checked because an agent that can read mail but
 * cannot send looks like it is ignoring people.
 */
static void
on_imap_reached(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    Reach *reach = g_task_get_task_data(task);
    ClawtIntegrationBinding *binding = reach->binding;
    const gchar *smtp_host;
    guint16 smtp_port;
    guint timeout;

    connection = g_socket_client_connect_to_host_finish(
        G_SOCKET_CLIENT(source), result, &error);

    if (connection == NULL) {
        g_prefix_error(&error, "could not reach IMAP at %s:%u: ",
                       reach->host, reach->port);
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

    smtp_host = clawt_integration_binding_get_string(binding, "smtp_host");
    smtp_port = (guint16)clawt_integration_binding_get_int(binding,
                                                           "smtp_port");
    timeout = reach->timeout;

    reach_async(g_steal_pointer(&task), binding, smtp_host, smtp_port,
                timeout);
}

void
clawt_integration_health_check_async(ClawtIntegrationBinding *binding,
                                     guint                    timeout_seconds,
                                     GCancellable            *cancellable,
                                     GAsyncReadyCallback      callback,
                                     gpointer                 user_data)
{
    GTask *task;
    const gchar *id;

    g_return_if_fail(binding != NULL);

    task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, clawt_integration_health_check_async);
    reach_for(task, binding);

    id = binding->info->id;

    if (g_strcmp0(id, "matrix") == 0) {
        const gchar *homeserver =
            clawt_integration_binding_get_string(binding, "homeserver");
        g_autoptr(GUri) uri = NULL;
        g_autoptr(GError) error = NULL;
        gint port;

        if (homeserver == NULL) {
            g_task_return_new_error(task, CLAWT_ERROR,
                                    CLAWT_ERROR_CONFIG_INVALID,
                                    "no homeserver is configured");
            g_object_unref(task);
            return;
        }

        uri = g_uri_parse(homeserver, G_URI_FLAGS_NONE, &error);

        if (uri == NULL) {
            g_task_return_error(task, g_steal_pointer(&error));
            g_object_unref(task);
            return;
        }

        port = g_uri_get_port(uri);

        if (port <= 0)
            port = g_strcmp0(g_uri_get_scheme(uri), "http") == 0 ? 80 : 443;

        reach_async(task, binding, g_uri_get_host(uri), (guint16)port,
                    timeout_seconds);
        return;
    }

    if (g_strcmp0(id, "email") == 0) {
        g_autoptr(GSocketClient) client = g_socket_client_new();
        const gchar *host =
            clawt_integration_binding_get_string(binding, "imap_host");
        guint16 port =
            (guint16)clawt_integration_binding_get_int(binding, "imap_port");
        Reach *reach;

        if (host == NULL || port == 0) {
            g_task_return_new_error(task, CLAWT_ERROR,
                                    CLAWT_ERROR_CONFIG_INVALID,
                                    "no IMAP host or port to check");
            g_object_unref(task);
            return;
        }

        reach = reach_for(task, binding);
        g_free(reach->host);
        reach->host = g_strdup(host);
        reach->port = port;
        reach->timeout = timeout_seconds;

        g_socket_client_set_timeout(client,
                                    timeout_seconds > 0 ? timeout_seconds : 10);

        g_socket_client_connect_to_host_async(client, host, port, cancellable,
                                              on_imap_reached, task);
        return;
    }

    if (g_strcmp0(id, "webhook") == 0) {
        guint16 port =
            (guint16)clawt_integration_binding_get_int(binding, "port");
        g_autoptr(GSocketListener) listener = g_socket_listener_new();
        g_autoptr(GError) local = NULL;

        /*
         * Checked by trying to bind rather than by connecting: for a
         * listener, the failure that matters is the port already being
         * taken.  Binding and immediately closing is the only way to find
         * that out without starting the agent -- and it needs no network
         * round trip, so it answers on this turn of the loop.
         */
        if (!g_socket_listener_add_inet_port(listener, port, NULL, &local)) {
            g_task_return_new_error(task, CLAWT_ERROR,
                                    CLAWT_ERROR_CONFIG_INVALID,
                                    "port %u is not available: %s",
                                    port, local->message);
            g_object_unref(task);
            return;
        }

        g_socket_listener_close(listener);
        g_task_return_boolean(task, TRUE);
        g_object_unref(task);
        return;
    }

    /*
     * A connector's health is whether there is a live credential, and
     * that is answered from disk.  Calling the provider to find out
     * would spend a real API call -- and a rate limit -- on a question
     * the token file already answers, on a path a person may hit for
     * every connector at once.
     */
    if (g_strcmp0(id, "connector") == 0) {
        const gchar *token_file =
            clawt_integration_binding_get_string(binding, "token_file");
        const gchar *provider =
            clawt_integration_binding_get_string(binding, "provider");
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autoptr(GError) load_error = NULL;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;

        if (provider == NULL) {
            g_task_return_new_error(task, CLAWT_ERROR,
                                    CLAWT_ERROR_CONFIG_INVALID,
                                    "no provider is set");
            g_object_unref(task);
            return;
        }

        if (token_file == NULL || !g_file_test(token_file,
                                               G_FILE_TEST_EXISTS)) {
            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                    "not connected yet -- run "
                                    "`clawtilla connector add %s`",
                                    binding->name);
            g_object_unref(task);
            return;
        }

        token = clawt_oauth_token_load(token_file, &load_error);

        if (token == NULL) {
            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                    "the stored credential cannot be read: "
                                    "%s", load_error->message);
            g_object_unref(task);
            return;
        }

        /*
         * Expired but renewable is healthy.  Reporting it as broken
         * would light up every connector in the list each hour, and
         * teach somebody to ignore the one that genuinely is.
         */
        if (clawt_oauth_token_is_expired(token, now, 0) &&
            token->refresh_token == NULL) {
            g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                    "the credential expired and the "
                                    "provider issued nothing to renew it "
                                    "with; connect again");
            g_object_unref(task);
            return;
        }

        g_task_return_boolean(task, TRUE);
        g_object_unref(task);
        return;
    }

    if (g_strcmp0(id, "mcp") == 0) {
        const gchar *command =
            clawt_integration_binding_get_string(binding, "command");
        const gchar *url =
            clawt_integration_binding_get_string(binding, "url");

        /*
         * A command is checked for existence rather than run.  Starting an
         * MCP server to find out whether it starts is a side effect nobody
         * asked for -- it may log in somewhere, or cost money -- and the
         * failure people actually hit is a binary that is not installed.
         */
        if (command != NULL) {
            g_autofree gchar *found = NULL;

            if (g_path_is_absolute(command))
                found = g_file_test(command, G_FILE_TEST_IS_EXECUTABLE)
                        ? g_strdup(command) : NULL;
            else
                found = g_find_program_in_path(command);

            if (found == NULL) {
                g_task_return_new_error(task, CLAWT_ERROR,
                                        CLAWT_ERROR_NOT_FOUND,
                                        "'%s' is not on the daemon's PATH",
                                        command);
                g_object_unref(task);
                return;
            }

            g_task_return_boolean(task, TRUE);
            g_object_unref(task);
            return;
        }

        if (url != NULL) {
            g_autoptr(GUri) uri = NULL;
            g_autoptr(GError) error = NULL;
            gint port;

            uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);

            if (uri == NULL) {
                g_task_return_error(task, g_steal_pointer(&error));
                g_object_unref(task);
                return;
            }

            port = g_uri_get_port(uri);

            if (port <= 0)
                port = g_strcmp0(g_uri_get_scheme(uri), "http") == 0
                       ? 80 : 443;

            reach_async(task, binding, g_uri_get_host(uri), (guint16)port,
                        timeout_seconds);
            return;
        }

        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "neither command nor url is set");
        g_object_unref(task);
        return;
    }

    /* local and cmacs need nothing outside the process. */
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

gboolean
clawt_integration_health_check_finish(ClawtIntegrationBinding  *binding,
                                      GAsyncResult             *result,
                                      GError                  **error)
{
    g_return_val_if_fail(binding != NULL, FALSE);
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}
