/*
 * daemon-selinux.c - Each team's SELinux level, kept and reported
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * computer.container.selinux_level under "auto" puts a team's containers
 * at one stored MCS pair, so a folder they share works for all of them.
 * The pair is picked the first time a team's container is created and
 * kept in tasks.db -- the daemon's durable store, already open before any
 * agent starts -- so it survives a daemon restart and a container
 * rebuild.  The computer factory reaches it through the function this
 * file registers; see clawt_selinux_level_set_team_func().
 *
 * Also here: the shared-folder warning with the real pairs (the Fleet
 * page's channel), and the "this agent's level changed, recreate its
 * container" report a reload and agent.set give back.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include <string.h>

/*
 * One allocation at a time.  The factory can be reached from a worker
 * thread, and two containers of a new team starting together must not
 * both pick -- and both try to store -- a pair.
 */
G_LOCK_DEFINE_STATIC(allocate);

/* How often a stored pair that collided with another team's is retried. */
#define SELINUX_STORE_ATTEMPTS (3)

gchar *
clawt_daemon_selinux_team_level_from(ClawtTaskStore  *store,
                                     ClawtConfig     *config,
                                     const gchar     *team,
                                     GError         **error)
{
    gchar *level = NULL;
    guint attempt;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(store), NULL);
    g_return_val_if_fail(team != NULL, NULL);

    G_LOCK(allocate);

    level = clawt_task_store_team_selinux_level(store, team);

    for (attempt = 0; level == NULL && attempt < SELINUX_STORE_ATTEMPTS;
         attempt++) {
        g_autoptr(GHashTable) stored =
            clawt_task_store_team_selinux_levels(store);
        g_auto(GStrv) explicit_levels = (config != NULL)
            ? clawt_selinux_level_explicit_levels(config) : NULL;
        g_autoptr(GPtrArray) taken = g_ptr_array_new();
        g_autofree gchar *picked = NULL;
        g_autoptr(GError) local = NULL;
        GHashTableIter iter;
        gpointer value;
        guint i;

        /* Every other team's pair, and every level the config names. */
        g_hash_table_iter_init(&iter, stored);
        while (g_hash_table_iter_next(&iter, NULL, &value))
            g_ptr_array_add(taken, value);

        for (i = 0; explicit_levels != NULL && explicit_levels[i] != NULL;
             i++)
            g_ptr_array_add(taken, explicit_levels[i]);

        g_ptr_array_add(taken, NULL);

        picked = clawt_selinux_level_allocate(
            (const gchar * const *)taken->pdata, NULL);

        if (picked == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "every MCS pair is taken or overlaps one in use");
            break;
        }

        if (clawt_task_store_add_team_selinux_level(store, team, picked,
                                                    &local)) {
            g_message("team %s: SELinux level %s, kept in %s; its "
                      "containers are created at it from now on", team,
                      picked, clawt_task_store_get_path(store));
            level = g_steal_pointer(&picked);
            break;
        }

        /*
         * Another daemon or a hand edit got there first -- for this team,
         * or with this pair.  Read again: the team may have one now.
         */
        level = clawt_task_store_team_selinux_level(store, team);

        if (level == NULL && attempt + 1 == SELINUX_STORE_ATTEMPTS)
            g_propagate_error(error, g_steal_pointer(&local));
    }

    G_UNLOCK(allocate);

    return level;
}

/*
 * The team function the factory calls: the stored pair, or a new one
 * kept before it is handed out.  Kept first, so a container is never
 * created at a level the next daemon will not remember.
 */
static gchar *
team_level(const gchar  *team,
           gpointer      user_data,
           GError      **error)
{
    ClawtDaemon *self = CLAWT_DAEMON(user_data);

    if (self->task_store == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "tasks.db under %s is not open, so a level for the team "
                    "could not be kept",
                    self->state_dir != NULL ? self->state_dir
                                            : "the state directory");
        return NULL;
    }

    return clawt_daemon_selinux_team_level_from(self->task_store,
                                                self->config, team, error);
}

void
clawt_daemon_selinux_setup(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    clawt_selinux_level_set_team_func(team_level, self);
}

void
clawt_daemon_selinux_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    /* Only ours: a second daemon in the same process keeps its own. */
    clawt_selinux_level_clear_team_func(self);
}

GHashTable *
clawt_daemon_team_selinux_levels(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    if (self->task_store == NULL)
        return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                     g_free);

    return clawt_task_store_team_selinux_levels(self->task_store);
}

GStrv
clawt_daemon_selinux_warnings(ClawtDaemon *self)
{
    g_autoptr(GHashTable) levels = NULL;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    levels = clawt_daemon_team_selinux_levels(self);

    return clawt_selinux_level_shared_mount_warnings(self->config, levels);
}

/*
 * The agents of the configuration on the shared machine @use, for a
 * machine's team under "auto".  From the configuration rather than the
 * daemon's pool, because a reload compares two configurations.
 */
static GPtrArray *
config_users_of(ClawtConfig *config, const gchar *use)
{
    GPtrArray *agents = clawt_config_get_agents(config);
    GPtrArray *users = g_ptr_array_new();
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);

        if (g_strcmp0(clawt_agent_config_get_string(agent, "computer.use"),
                      use) == 0)
            g_ptr_array_add(users, agent);
    }

    return users;
}

gchar *
clawt_daemon_selinux_agent_level(ClawtDaemon *self,
                                 ClawtConfig *config,
                                 const gchar *agent_id,
                                 GHashTable  *team_levels)
{
    ClawtAgentConfig *agent;
    const gchar *use;
    g_autoptr(GPtrArray) users = NULL;
    gchar *level;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    agent = (config != NULL && agent_id != NULL)
            ? clawt_config_get_agent(config, agent_id) : NULL;

    if (agent == NULL ||
        clawt_agent_config_get_enum(agent, "computer.type") !=
            CLAWT_COMPUTER_CONTAINER)
        return NULL;

    use = clawt_agent_config_get_string(agent, "computer.use");

    if (use != NULL && *use != '\0')
        users = config_users_of(config, use);

    level = clawt_selinux_level_effective(agent, users, team_levels);

    /* "" for podman's random pair, so NULL keeps meaning "no container". */
    return (level != NULL) ? level : g_strdup("");
}

GHashTable *
clawt_daemon_selinux_snapshot(ClawtDaemon *self)
{
    g_autoptr(GHashTable) levels = NULL;
    GHashTable *snapshot;
    GPtrArray *agents;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    snapshot = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    if (self->config == NULL)
        return snapshot;

    levels = clawt_daemon_team_selinux_levels(self);
    agents = clawt_config_get_agents(self->config);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        const gchar *id =
            clawt_agent_config_get_id(g_ptr_array_index(agents, i));
        gchar *level = clawt_daemon_selinux_agent_level(self, self->config,
                                                        id, levels);

        if (level != NULL)
            g_hash_table_insert(snapshot, g_strdup(id), level);
    }

    return snapshot;
}

/* A level as the operator reads it. */
static gchar *
level_said(const gchar *level)
{
    if (level == NULL || *level == '\0')
        return g_strdup("podman's random pair");

    if (g_str_has_prefix(level, "team:"))
        return g_strdup_printf("team %s's level", level + strlen("team:"));

    return g_strdup(level);
}

gchar *
clawt_daemon_selinux_change_note(const gchar *agent_id,
                                 const gchar *before,
                                 const gchar *after)
{
    g_autofree gchar *was = NULL;
    g_autofree gchar *now = NULL;

    g_return_val_if_fail(agent_id != NULL, NULL);

    if (before == NULL || after == NULL || g_strcmp0(before, after) == 0)
        return NULL;

    was = level_said(before);
    now = level_said(after);

    return g_strdup_printf(
        "agent %s: its container's SELinux level changes from %s to %s. A "
        "level is set only when a container is created, so the container "
        "must be recreated for it to apply: stop the agent and start it "
        "again (its next start makes the container afresh, and anything "
        "installed inside it is lost), or stop it and run `clawtilla "
        "computer rebuild %s`", agent_id, was, now, agent_id);
}

/* g_ptr_array_sort() hands over pointers to the elements. */
static gint
compare_notes(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

GPtrArray *
clawt_daemon_selinux_changes(ClawtDaemon *self, GHashTable *before)
{
    g_autoptr(GHashTable) after = NULL;
    GPtrArray *notes;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    notes = g_ptr_array_new_with_free_func(g_free);

    if (before == NULL)
        return notes;

    after = clawt_daemon_selinux_snapshot(self);

    /*
     * Only agents that had a container before and still have one: a new
     * agent, or one that just became a container, has nothing to
     * recreate.
     */
    g_hash_table_iter_init(&iter, after);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gchar *note = clawt_daemon_selinux_change_note(
            key, g_hash_table_lookup(before, key), value);

        if (note != NULL)
            g_ptr_array_add(notes, note);
    }

    /* In agent order, so the same reload always reads the same way. */
    g_ptr_array_sort(notes, compare_notes);

    return notes;
}
