/*
 * daemon-shared-computer.c - Machines several agents share
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent with computer.use names an entry under computers:. Every agent
 * naming the same entry gets the same #ClawtComputer object, built once,
 * named after the entry rather than any agent, and carrying each user's
 * workspace and exchange folder. The pool here is the one place that knows
 * which agents a machine is under; stopping, rebuilding and removing ask
 * it before doing anything to the machine itself.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

const gchar *
clawt_daemon_shared_computer_id(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *config = agent_id != NULL
        ? clawt_config_get_agent(self->config, agent_id) : NULL;
    const gchar *use = config != NULL
        ? clawt_agent_config_get_string(config, "computer.use") : NULL;

    return (use != NULL && *use != '\0') ? use : NULL;
}

GPtrArray *
clawt_daemon_shared_users(ClawtDaemon *self, const gchar *computer_id,
                          gboolean running_only, const gchar *except)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *agents = clawt_config_get_agents(self->config);
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *config = g_ptr_array_index(agents, i);
        const gchar *id = clawt_agent_config_get_id(config);
        ClawtAgent *agent;

        if (g_strcmp0(id, except) == 0 || clawt_agent_config_is_shadow(config))
            continue;
        if (g_strcmp0(clawt_agent_config_get_string(config, "computer.use"), computer_id) != 0)
            continue;

        agent = clawt_agent_manager_get(self->agents, id);
        if (running_only &&
            (agent == NULL || clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_RUNNING))
            continue;

        g_ptr_array_add(out, g_strdup(id));
    }

    return out;
}

gchar *
clawt_daemon_shared_busy_refusal(ClawtDaemon *self, const gchar *agent_id,
                                 const gchar *what)
{
    const gchar *use = clawt_daemon_shared_computer_id(self, agent_id);
    g_autoptr(GPtrArray) running = NULL;
    g_autoptr(GString) names = NULL;
    guint i;

    if (use == NULL)
        return NULL;

    running = clawt_daemon_shared_users(self, use, TRUE, agent_id);
    if (running->len == 0)
        return NULL;

    names = g_string_new(NULL);
    for (i = 0; i < running->len; i++)
        g_string_append_printf(names, "%s%s", i == 0 ? "" : ", ",
                               (const gchar *)g_ptr_array_index(running, i));

    return g_strdup_printf("'%s' is the shared computer '%s', and %s %s running on it; "
                           "stop %s first to %s", agent_id, use, names->str,
                           running->len == 1 ? "is" : "are",
                           running->len == 1 ? "it" : "them", what);
}

/* Adds a mount unless the machine already has one at that path. */
static void
add_mount_once(ClawtComputer *computer, ClawtMount *mount)
{
    GPtrArray *existing = clawt_computer_get_mounts(computer);
    guint i;

    for (i = 0; existing != NULL && i < existing->len; i++)
        if (g_strcmp0(clawt_mount_get_target(g_ptr_array_index(existing, i)),
                      clawt_mount_get_target(mount)) == 0)
            return;

    clawt_computer_add_mount(computer, mount);
}

ClawtComputer *
clawt_daemon_shared_computer_get(ClawtDaemon *self, ClawtAgent *agent, GError **error)
{
    const gchar *agent_id = clawt_agent_get_id(agent);
    const gchar *use = clawt_daemon_shared_computer_id(self, agent_id);
    ClawtComputer *computer;
    g_autoptr(GPtrArray) user_ids = NULL;
    g_autoptr(GPtrArray) users = NULL;
    g_autoptr(GPtrArray) defaults = NULL;
    guint i;

    g_return_val_if_fail(use != NULL, NULL);

    if (self->shared_computers == NULL)
        self->shared_computers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, g_object_unref);

    /*
     * Reused while it exists. A stale copy is rebuilt only when nothing
     * runs on it: provisioning a container replaces it, which under a
     * running agent would destroy what that agent was working on.
     */
    computer = g_hash_table_lookup(self->shared_computers, use);
    if (computer != NULL &&
        (!clawt_agent_computer_is_stale(agent) ||
         clawt_computer_get_state(computer) == CLAWT_COMPUTER_STATE_RUNNING))
        return g_object_ref(computer);

    /* Every agent on it, so every workspace is mounted when it is created. */
    user_ids = clawt_daemon_shared_users(self, use, FALSE, NULL);
    users = g_ptr_array_new();
    for (i = 0; i < user_ids->len; i++)
        g_ptr_array_add(users, clawt_config_get_agent(self->config,
                                                      g_ptr_array_index(user_ids, i)));
    if (users->len == 0)
        g_ptr_array_add(users, clawt_agent_get_config(agent));

    defaults = clawt_config_get_default_mounts(self->config);
    computer = clawt_computer_factory_create_shared(use, users, defaults,
                                                    self->pod_bridge, error);
    if (computer == NULL)
        return NULL;

    /* Each agent's exchange folder; the shared root and shared/ once. */
    if (self->exchange != NULL &&
        clawt_computer_type_shares_host_paths(clawt_computer_get_computer_type(computer))) {
        for (i = 0; i < user_ids->len; i++) {
            const gchar *user = g_ptr_array_index(user_ids, i);
            ClawtAgentConfig *config = clawt_config_get_agent(self->config, user);
            g_autoptr(GError) local = NULL;
            g_autoptr(GPtrArray) mounts = NULL;
            guint m;

            if (!clawt_agent_config_get_boolean(config, "computer.exchange") ||
                !clawt_exchange_prepare(self->exchange, user, &local))
                continue;
            mounts = clawt_exchange_get_mounts(self->exchange, user);
            for (m = 0; m < mounts->len; m++)
                add_mount_once(computer, g_ptr_array_index(mounts, m));
        }
    }

    g_hash_table_replace(self->shared_computers, g_strdup(use), g_object_ref(computer));
    return computer;
}

void
clawt_daemon_shared_computer_forget(ClawtDaemon *self, const gchar *computer_id)
{
    if (self->shared_computers != NULL && computer_id != NULL)
        g_hash_table_remove(self->shared_computers, computer_id);
}

/* ── One screen, one driver ───────────────────────────────────────── */

typedef struct {
    gchar  *agent_id;
    gint64  expires;     /* monotonic microseconds */
} DriveLease;

static void
drive_lease_free(gpointer data)
{
    DriveLease *lease = data;

    g_free(lease->agent_id);
    g_free(lease);
}

/*
 * Whether @agent_id may type or click on its shared machine's screen now,
 * taking (or renewing) the lease when it may. A shared machine has one
 * desktop; two agents driving it at once would each click on what the
 * other just opened. The lease lapses after the holder has been idle for
 * computer.desktop.driver_idle_seconds, so an agent that finished does not
 * keep the screen.
 */
gboolean
clawt_daemon_shared_take_screen(ClawtDaemon *self, const gchar *agent_id,
                                gchar **out_refusal)
{
    const gchar *use = clawt_daemon_shared_computer_id(self, agent_id);
    ClawtAgentConfig *config = clawt_config_get_agent(self->config, agent_id);
    gint64 now = g_get_monotonic_time();
    gint idle = config != NULL
        ? clawt_agent_config_get_int(config, "computer.desktop.driver_idle_seconds") : 120;
    DriveLease *lease;

    if (use == NULL)
        return TRUE;

    if (self->drive_leases == NULL)
        self->drive_leases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   drive_lease_free);

    lease = g_hash_table_lookup(self->drive_leases, use);
    if (lease != NULL && g_strcmp0(lease->agent_id, agent_id) != 0 && lease->expires > now) {
        if (out_refusal != NULL)
            *out_refusal = g_strdup_printf(
                "%s is driving the screen of the shared computer '%s'. Looking still "
                "works; try again in %" G_GINT64_FORMAT " seconds, or leave a note in the "
                "exchange for them.", lease->agent_id, use,
                (lease->expires - now) / G_USEC_PER_SEC + 1);
        return FALSE;
    }

    lease = g_new0(DriveLease, 1);
    lease->agent_id = g_strdup(agent_id);
    lease->expires = now + (gint64)MAX(idle, 1) * G_USEC_PER_SEC;
    g_hash_table_replace(self->drive_leases, g_strdup(use), lease);
    return TRUE;
}

/*
 * Whether a person holds the screen of any agent on @agent_id's shared
 * machine: one screen, so a takeover of it is a takeover for all of them.
 */
gboolean
clawt_daemon_shared_screen_held(ClawtDaemon *self, const gchar *agent_id)
{
    const gchar *use = clawt_daemon_shared_computer_id(self, agent_id);
    g_autoptr(GPtrArray) users = NULL;
    guint i;

    if (use == NULL)
        return FALSE;

    users = clawt_daemon_shared_users(self, use, FALSE, NULL);
    for (i = 0; i < users->len; i++)
        if (clawt_takeover_is_held(self->takeover, g_ptr_array_index(users, i)))
            return TRUE;

    return FALSE;
}

