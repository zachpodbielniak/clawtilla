/*
 * clawt-agent-manager.c - The fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-agent-manager.h"

enum {
    SIGNAL_AGENT_ADDED,
    SIGNAL_AGENT_REMOVED,
    SIGNAL_AGENT_STATE_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtAgentManager {
    GObject parent_instance;

    ClawtConfig *config;
    gchar       *state_dir;

    GPtrArray   *agents;   /* ClawtAgent*, configuration order */
    GHashTable  *by_id;    /* agent_id -> ClawtAgent, unowned */

    /*
     * The team and fleet memory databases, one set per fleet.
     *
     * Built lazily from state_dir, because clawt_agent_manager_new()
     * runs before clawt_agent_manager_set_state_dir() on the daemon's
     * path and a set built from the wrong directory would quietly write
     * a fleet's shared memories somewhere nobody reads.
     */
    ClawtMemoryScopes *scopes;
};

G_DEFINE_FINAL_TYPE(ClawtAgentManager, clawt_agent_manager, G_TYPE_OBJECT)

ClawtAgentManager *
clawt_agent_manager_new(ClawtConfig *config)
{
    ClawtAgentManager *self;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

    self = g_object_new(CLAWT_TYPE_AGENT_MANAGER, NULL);
    self->config = g_object_ref(config);
    self->state_dir = clawt_config_get_path_value(config, "daemon.state_dir");

    return self;
}

void
clawt_agent_manager_set_state_dir(ClawtAgentManager *self,
                                  const gchar       *state_dir)
{
    g_return_if_fail(CLAWT_IS_AGENT_MANAGER(self));

    g_free(self->state_dir);
    self->state_dir = clawt_expand_path(state_dir);
}

ClawtMemoryScopes *
clawt_agent_manager_get_memory_scopes(ClawtAgentManager *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), NULL);

    if (self->scopes == NULL && self->state_dir != NULL)
        self->scopes = clawt_memory_scopes_new(self->state_dir);

    return self->scopes;
}

/*
 * The team this agent is on, or %NULL.
 *
 * Read from the agent's config each time rather than remembered: a team
 * is edited in clawtilla.yaml, and a value cached at load would have an
 * agent still writing into the memories of a team it has left.
 */
static const gchar *
team_of(ClawtAgentManager *self, const gchar *agent_id)
{
    ClawtAgent *agent = clawt_agent_manager_get(self, agent_id);
    const gchar *team;

    if (agent == NULL)
        return NULL;

    team = clawt_agent_config_get_string(clawt_agent_get_config(agent),
                                         "team");

    return (team != NULL && *team != '\0') ? team : NULL;
}

ClawtMemoryStore *
clawt_agent_manager_memory_write_store(ClawtAgentManager *self,
                                       const gchar *agent_id,
                                       GError **error)
{
    ClawtAgent *agent;
    ClawtMemoryScopes *scopes;
    ClawtMemoryScope scope = CLAWT_MEMORY_SCOPE_AGENT;

    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), NULL);

    agent = (agent_id != NULL) ? clawt_agent_manager_get(self, agent_id)
                               : NULL;

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no agent called '%s'",
                    agent_id != NULL ? agent_id : "");
        return NULL;
    }

    scope = (ClawtMemoryScope)clawt_agent_config_get_enum(
        clawt_agent_get_config(agent), "memories.scope");

    /*
     * Agent scope is answered from the store the manager already opened,
     * rather than by opening the same file again.  Two connections to
     * one sqlite file are two page caches, and the one that did not
     * write is the one a search reads from.
     */
    if (scope == CLAWT_MEMORY_SCOPE_AGENT)
        return clawt_agent_get_memory(agent);

    scopes = clawt_agent_manager_get_memory_scopes(self);

    if (scopes == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "this fleet has no state directory, so it has "
                            "no shared memories");
        return NULL;
    }

    return clawt_memory_scopes_open_for_write(
        scopes, scope,
        (scope == CLAWT_MEMORY_SCOPE_TEAM) ? team_of(self, agent_id) : NULL,
        error);
}

GPtrArray *
clawt_agent_manager_memory_search(ClawtAgentManager *self,
                                  const gchar *agent_id, const gchar *query,
                                  const gchar *category, gboolean pinned_only,
                                  guint limit)
{
    ClawtAgent *agent;
    ClawtMemoryScopes *scopes;
    ClawtMemoryStore *own;

    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), NULL);

    agent = (agent_id != NULL) ? clawt_agent_manager_get(self, agent_id)
                               : NULL;

    if (agent == NULL)
        return g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_memory_free);

    own = clawt_agent_get_memory(agent);
    scopes = clawt_agent_manager_get_memory_scopes(self);

    if (scopes == NULL) {
        /*
         * No shared scopes to fan out across, so the agent's own store
         * is the whole answer -- and an agent whose memories are off has
         * none of that either.
         */
        if (own == NULL)
            return g_ptr_array_new_with_free_func(
                (GDestroyNotify)clawt_memory_free);

        return (query != NULL && *query != '\0')
            ? clawt_memory_store_search(own, query, category, limit, NULL)
            : clawt_memory_store_list(own, category, pinned_only, limit,
                                      NULL);
    }

    return (query != NULL && *query != '\0')
        ? clawt_memory_scopes_search(scopes, own, team_of(self, agent_id),
                                     query, category, limit, NULL)
        : clawt_memory_scopes_list(scopes, own, team_of(self, agent_id),
                                   category, pinned_only, limit, NULL);
}

static void
on_agent_state_changed(ClawtAgent  *agent,
                       gint         state,
                       const gchar *detail,
                       gpointer     user_data)
{
    ClawtAgentManager *self = user_data;

    g_signal_emit(self, signals[SIGNAL_AGENT_STATE_CHANGED], 0,
                  clawt_agent_get_id(agent), state, detail);
}

/*
 * One mailbox setting, as this agent sees it.
 *
 * The agent-relative spelling comes from the schema rather than from a
 * list here -- and so does the fallback, since
 * clawt_agent_config_get_int() now follows the same relation down to the
 * fleet key and then to the schema default. That is why this no longer
 * asks whether the agent set the key: the answer to "what is it for this
 * agent" is one lookup, not a choice between two.
 *
 * A fleet key with no agent spelling is a schema mistake rather than a
 * runtime one, so it warns and falls back to the fleet value instead of
 * silently reading nothing.
 */
static gint64
mailbox_setting(ClawtAgentManager *self,
                ClawtAgentConfig  *agent_config,
                const gchar       *fleet_key)
{
    const gchar *agent_key = clawt_config_schema_agent_key_for(fleet_key);

    if (agent_key == NULL) {
        g_warning("schema: %s has no agent-relative name, so an agent "
                  "cannot override it", fleet_key);

        return clawt_config_get_int(self->config, fleet_key);
    }

    return clawt_agent_config_get_int(agent_config, agent_key);
}

/*
 * Applies the mailbox policy from configuration, letting an agent override
 * the fleet defaults.
 */
static void
configure_mailbox(ClawtAgentManager *self,
                  ClawtAgentConfig  *agent_config,
                  ClawtMailbox      *mailbox)
{
    ClawtOverflowPolicy overflow;

    /*
     * Overflow was read from the fleet alone for the whole life of this
     * function, while the schema has always flagged it PER_AGENT -- so
     * an agent that set `mailbox.overflow` was told it could and then
     * ignored. It is read the same way as the other five now.
     */
    {
        const gchar *agent_key = clawt_config_schema_agent_key_for(
            "orchestration.mailbox.overflow");

        overflow = (ClawtOverflowPolicy)
            ((agent_key != NULL)
             ? clawt_agent_config_get_enum(agent_config, agent_key)
             : clawt_config_get_enum(self->config,
                                     "orchestration.mailbox.overflow"));
    }

    clawt_mailbox_set_policy(
        mailbox,
        (guint)mailbox_setting(self, agent_config,
                               "orchestration.mailbox.max_depth"),
        overflow,
        (guint)mailbox_setting(self, agent_config,
                               "orchestration.mailbox.max_attempts"),
        (guint)mailbox_setting(self, agent_config,
                               "orchestration.mailbox.lease_seconds"),
        (guint)mailbox_setting(self, agent_config,
                               "orchestration.mailbox.backoff_seconds"),
        (guint)mailbox_setting(self, agent_config,
                               "orchestration.mailbox.default_ttl_seconds"));
}

/*
 * Builds one agent from its configuration.
 *
 * Returns: (transfer full): the agent
 */
static ClawtAgent *
build_agent(ClawtAgentManager *self, ClawtAgentConfig *agent_config)
{
    const gchar *agent_id = clawt_agent_config_get_id(agent_config);
    g_autoptr(ClawtMailbox) mailbox = NULL;
    g_autoptr(GError) mailbox_error = NULL;
    g_autofree gchar *db_path = NULL;
    ClawtAgent *agent;

    db_path = g_build_filename(self->state_dir, "agents", agent_id,
                               "mailbox.db", NULL);

    mailbox = clawt_mailbox_new(agent_id, db_path, &mailbox_error);

    /*
     * A mailbox that cannot be opened costs that agent, not the fleet.
     * It becomes a shadow explaining why, and the other nine keep
     * running.
     */
    if (mailbox == NULL)
        g_warning("agent %s: its mailbox could not be opened: %s",
                  agent_id, mailbox_error->message);
    else
        configure_mailbox(self, agent_config, mailbox);

    agent = clawt_agent_new(agent_config, mailbox);

    if (mailbox == NULL)
        clawt_agent_mark_shadow(agent, "its mailbox could not be opened");

    /*
     * Its memory, beside its mailbox and no further.  One database per
     * agent, so an agent reading its own memories cannot reach anybody
     * else's -- there is no query that crosses, because the other
     * memories are in another file.
     *
     * A memory store that will not open costs the agent its memory and
     * nothing else: an agent that cannot remember is still an agent, and
     * refusing to start it would be a worse answer than a warning.
     */
    if (clawt_agent_config_get_boolean(agent_config, "memories.enabled")) {
        g_autofree gchar *memory_path = NULL;
        g_autoptr(ClawtMemoryStore) memory = NULL;
        g_autoptr(GError) memory_error = NULL;

        memory_path = g_build_filename(self->state_dir, "agents", agent_id,
                                       "memory.db", NULL);
        memory = clawt_memory_store_new(memory_path, &memory_error);

        if (memory == NULL)
            g_warning("agent %s: its memory could not be opened: %s",
                      agent_id, memory_error->message);
        else
            clawt_agent_set_memory(agent, memory);
    }

    g_signal_connect(agent, "state-changed",
                     G_CALLBACK(on_agent_state_changed), self);

    return agent;
}

void
clawt_agent_manager_set_config(ClawtAgentManager *self, ClawtConfig *config)
{
    g_return_if_fail(CLAWT_IS_AGENT_MANAGER(self));
    g_return_if_fail(CLAWT_IS_CONFIG(config));

    if (self->config == config)
        return;

    g_clear_object(&self->config);
    self->config = g_object_ref(config);
}

gboolean
clawt_agent_manager_load(ClawtAgentManager *self, GError **error)
{
    GPtrArray *agent_configs;
    g_autoptr(GHashTable) wanted = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), FALSE);

    (void)error;

    agent_configs = clawt_config_get_agents(self->config);
    wanted = g_hash_table_new(g_str_hash, g_str_equal);

    /*
     * Reconciled against what is already here, never rebuilt.
     *
     * This used to empty both containers and construct every agent
     * afresh, which meant adding one agent destroyed the live object of
     * every other agent in the fleet -- their runtimes, computers and
     * links went with them, while the link server carried on holding
     * connections for agents that no longer existed.  Creating an agent
     * must not disturb the ones already working.
     */
    for (i = 0; i < agent_configs->len; i++) {
        ClawtAgentConfig *agent_config = g_ptr_array_index(agent_configs, i);
        const gchar *agent_id = clawt_agent_config_get_id(agent_config);
        ClawtAgent *existing;

        g_hash_table_add(wanted, (gpointer)agent_id);

        existing = g_hash_table_lookup(self->by_id, agent_id);

        if (existing != NULL) {
            /*
             * The configuration objects belong to the ClawtConfig that
             * was just loaded, so even an unchanged agent needs the new
             * one -- the old is about to be freed with its config.
             */
            clawt_agent_set_config(existing, agent_config);
            continue;
        }

        {
            ClawtAgent *agent = build_agent(self, agent_config);

            g_ptr_array_add(self->agents, agent);
            g_hash_table_insert(self->by_id,
                                g_strdup(clawt_agent_get_id(agent)), agent);

            g_signal_emit(self, signals[SIGNAL_AGENT_ADDED], 0,
                          clawt_agent_get_id(agent));
        }
    }

    /* Anything no longer in the configuration is stopped and dropped. */
    for (i = self->agents->len; i > 0; i--) {
        ClawtAgent *agent = g_ptr_array_index(self->agents, i - 1);
        const gchar *agent_id = clawt_agent_get_id(agent);
        g_autofree gchar *removed_id = NULL;

        if (g_hash_table_contains(wanted, agent_id))
            continue;

        removed_id = g_strdup(agent_id);

        clawt_agent_stop(agent);
        g_hash_table_remove(self->by_id, agent_id);
        g_ptr_array_remove_index(self->agents, i - 1);

        g_signal_emit(self, signals[SIGNAL_AGENT_REMOVED], 0, removed_id);
    }

    return TRUE;
}

ClawtConfig *
clawt_agent_manager_get_config(ClawtAgentManager *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), NULL);

    return self->config;
}

GPtrArray *
clawt_agent_manager_list(ClawtAgentManager *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), NULL);

    return self->agents;
}

ClawtAgent *
clawt_agent_manager_get(ClawtAgentManager *self, const gchar *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    return g_hash_table_lookup(self->by_id, agent_id);
}

ClawtAgent *
clawt_agent_manager_get_chief_of_staff(ClawtAgentManager *self)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), NULL);

    for (i = 0; i < self->agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(self->agents, i);

        if (clawt_agent_is_chief_of_staff(agent) &&
            clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_SHADOW)
            return agent;
    }

    /*
     * Falling back to orchestration.chief_of_staff, so the role can be
     * named in one place rather than on the agent, whichever the user finds
     * more natural.
     */
    {
        const gchar *named =
            clawt_config_get_string(self->config,
                                    "orchestration.chief_of_staff");

        if (named != NULL)
            return clawt_agent_manager_get(self, named);
    }

    return NULL;
}

guint
clawt_agent_manager_start_all(ClawtAgentManager *self)
{
    guint started = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(self), 0);

    for (i = 0; i < self->agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(self->agents, i);
        ClawtAgentConfig *config = clawt_agent_get_config(agent);
        g_autoptr(GError) error = NULL;
        gboolean autostart;

        if (clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_SHADOW)
            continue;

        autostart = clawt_agent_config_has_key(config, "runtime.autostart")
                    ? clawt_agent_config_get_boolean(config,
                                                     "runtime.autostart")
                    : clawt_config_get_boolean(self->config,
                                               "defaults.autostart");

        if (!autostart)
            continue;

        /*
         * One agent failing does not stop the others.  A fleet where a
         * single misconfigured agent prevents the rest from running would
         * be worse than one that starts nine of ten and says which failed.
         */
        if (clawt_agent_start(agent, &error))
            started++;
        else
            g_warning("agent %s: %s", clawt_agent_get_id(agent),
                      error->message);
    }

    return started;
}

void
clawt_agent_manager_stop_all(ClawtAgentManager *self)
{
    guint i;

    g_return_if_fail(CLAWT_IS_AGENT_MANAGER(self));

    for (i = 0; i < self->agents->len; i++)
        clawt_agent_stop(g_ptr_array_index(self->agents, i));
}

static void
clawt_agent_manager_dispose(GObject *object)
{
    ClawtAgentManager *self = CLAWT_AGENT_MANAGER(object);

    if (self->agents != NULL) {
        guint i;

        for (i = 0; i < self->agents->len; i++)
            g_signal_handlers_disconnect_by_func(
                g_ptr_array_index(self->agents, i),
                G_CALLBACK(on_agent_state_changed), self);
    }

    g_clear_pointer(&self->by_id, g_hash_table_unref);
    g_clear_pointer(&self->agents, g_ptr_array_unref);
    g_clear_object(&self->scopes);
    g_clear_object(&self->config);

    G_OBJECT_CLASS(clawt_agent_manager_parent_class)->dispose(object);
}

static void
clawt_agent_manager_finalize(GObject *object)
{
    ClawtAgentManager *self = CLAWT_AGENT_MANAGER(object);

    g_clear_pointer(&self->state_dir, g_free);

    G_OBJECT_CLASS(clawt_agent_manager_parent_class)->finalize(object);
}

static void
clawt_agent_manager_class_init(ClawtAgentManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_agent_manager_dispose;
    object_class->finalize = clawt_agent_manager_finalize;

    signals[SIGNAL_AGENT_ADDED] =
        g_signal_new("agent-added", CLAWT_TYPE_AGENT_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_AGENT_REMOVED] =
        g_signal_new("agent-removed", CLAWT_TYPE_AGENT_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_AGENT_STATE_CHANGED] =
        g_signal_new("agent-state-changed", CLAWT_TYPE_AGENT_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_INT,
                     G_TYPE_STRING);
}

static void
clawt_agent_manager_init(ClawtAgentManager *self)
{
    self->agents = g_ptr_array_new_with_free_func(g_object_unref);

    /*
     * Keys are borrowed from the agents in the array, which own them and
     * outlive this table -- the array is cleared after it in dispose.
     */
    /*
     * The keys are owned copies, not pointers into an agent's
     * configuration.  A reload hands every agent a new ClawtAgentConfig
     * and frees the old one, so a borrowed id would dangle the moment
     * the fleet was reconciled.
     */
    self->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        NULL);
}
