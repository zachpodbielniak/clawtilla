/*
 * daemon-ephemeral.c - Ephemeral agents: spawned for a piece of work, reaped after
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An ephemeral agent is a helper an agent makes for itself: a copy of
 * how the parent runs, with a silly name, on the team tagged
 * `ephemeral`, there until the parent says the work is done and reaps
 * it.  Three rules hold it together:
 *
 * - It never does more than its parent.  The copy is made by
 *   clawt_config_add_ephemeral_agent(), which drops identities and
 *   turns off spawning and agent creation; the computer chosen here is
 *   never stronger than the parent's; confinement is copied, not asked.
 * - It is the parent's to end, and nobody else's but the operator's.
 *   Not on a timer and not when a task finishes: a parent that batches
 *   work hands a helper a second piece after the first, and a helper
 *   reaped under it is a turn talking to nobody.  A finished, idle
 *   helper earns its parent one reminder, and that is all.
 * - It outlives a restart.  The store (`<state_dir>/ephemeral.yaml`) is
 *   written on every change and read back at start and after every
 *   config reload, because clawtilla.yaml -- which never holds one --
 *   is what a reload reads.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

/* How often finished helpers are looked at for a reminder. */
#define EPHEMERAL_SWEEP_SECONDS (600)

static gchar *
store_path(ClawtDaemon *self)
{
    return g_build_filename(self->state_dir, "ephemeral.yaml", NULL);
}

gboolean
clawt_daemon_ephemeral_save(ClawtDaemon *self, GError **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    if (self->state_dir == NULL || self->config == NULL)
        return TRUE;

    path = store_path(self);
    text = clawt_config_ephemeral_to_string(self->config);
    /* the entries copy their parent's settings, so the file is the operator's alone */
    if (!g_file_set_contents_full(path, text, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
        return FALSE;
    return TRUE;
}

void
clawt_daemon_ephemeral_load(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GPtrArray) skipped = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GError) error = NULL;
    guint added;
    guint i;

    if (self->state_dir == NULL || self->config == NULL)
        return;

    path = store_path(self);
    if (!g_file_get_contents(path, &text, NULL, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("ephemeral agents: %s could not be read, so none were "
                      "restored: %s", path, error->message);
        return;
    }

    added = clawt_config_ephemeral_load(self->config, text, skipped);
    for (i = 0; i < skipped->len; i++)
        g_warning("ephemeral agents: an entry in %s was not restored: %s",
                  path, (const gchar *)g_ptr_array_index(skipped, i));
    if (added > 0)
        g_debug("ephemeral agents: %u restored from %s", added, path);
}

/* ── Who is whose ───────────────────────────────────────────────── */

static GPtrArray *
children_of(ClawtDaemon *self, const gchar *parent_id)
{
    GPtrArray *agents = clawt_config_get_agents(self->config);
    GPtrArray *out = g_ptr_array_new();
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);

        if (!clawt_agent_config_is_ephemeral(agent))
            continue;
        if (parent_id == NULL ||
            g_strcmp0(clawt_agent_config_get_ephemeral_string(agent, "parent"),
                      parent_id) == 0)
            g_ptr_array_add(out, agent);
    }
    return out;
}

gboolean
clawt_daemon_ephemeral_has_children(ClawtDaemon *self, const gchar *parent_id)
{
    g_autoptr(GPtrArray) children = children_of(self, parent_id);

    return children->len > 0;
}

/*
 * The team helpers go on: the one tagged `ephemeral`, or one made now,
 * called Ephemeral, the first time it is needed -- declared in the file
 * like any team, so it can be renamed, recoloured or replaced by tagging
 * another.
 */
static gchar *
ensure_team(ClawtDaemon *self, GError **error)
{
    g_autofree gchar *team = clawt_team_find_by_tag(self->config,
                                                    CLAWT_TEAM_TAG_EPHEMERAL);
    const gchar *const tags[] = { CLAWT_TEAM_TAG_EPHEMERAL, NULL };
    const gchar *id = "ephemeral";
    g_autoptr(ClawtTeamSpec) existing = NULL;

    if (team != NULL)
        return g_steal_pointer(&team);

    /* an untagged team of that id is somebody else's; take another */
    existing = clawt_config_get_team(self->config, id);
    if (existing != NULL)
        id = "ephemeral-agents";
    if (!clawt_config_add_team(self->config, id, error))
        return NULL;
    clawt_config_set_team_string(self->config, id, "name", "Ephemeral");
    clawt_config_set_team_string(self->config, id, "description",
        "Temporary helpers agents spawn for one piece of work and reap when "
        "it is done. Assign work here only through the agent that spawned "
        "each one.");
    clawt_config_set_team_string_list(self->config, id, "tags", tags);
    if (!clawt_config_save(self->config, error))
        return NULL;
    clawt_event_bus_emit(self->bus, "team.changed", id);
    return g_strdup(id);
}

/*
 * The computer a helper may have, never stronger than its parent's: the
 * same as the parent's own when that is none or the host, chat-only for
 * a parent in a machine (a second VM per helper is a machine built for
 * an errand), and a container only when the parent could run one.
 */
static gboolean
pick_computer(ClawtAgentConfig *parent,
              const gchar      *asked,
              const gchar     **out_type,
              GError          **error)
{
    const gchar *own = clawt_agent_config_get_string(parent, "computer.type");
    gboolean parent_on_host = g_strcmp0(own, "host") == 0 ||
                              g_strcmp0(own, "display") == 0;
    gboolean parent_in_box = g_strcmp0(own, "container") == 0 ||
                             g_strcmp0(own, "distrobox") == 0;

    if (asked == NULL || *asked == '\0') {
        *out_type = (own == NULL || g_strcmp0(own, "none") == 0 ||
                     g_strcmp0(own, "host") == 0) ? NULL : "none";
        return TRUE;
    }
    if (g_strcmp0(asked, "none") == 0) {
        *out_type = "none";
        return TRUE;
    }
    if (g_strcmp0(asked, "host") == 0 && g_strcmp0(own, "host") == 0) {
        *out_type = NULL;
        return TRUE;
    }
    if (g_strcmp0(asked, "container") == 0 && (parent_on_host || parent_in_box)) {
        *out_type = "container";
        return TRUE;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                "a helper cannot have a '%s' computer when you have '%s': it "
                "may have none, the same as yours on the host, or a container "
                "if you are on the host or in one. VMs are never spawned.",
                asked, own != NULL ? own : "none");
    return FALSE;
}

static gboolean
name_taken(const gchar *name, gpointer user_data)
{
    ClawtDaemon *self = user_data;

    return clawt_config_get_agent(self->config, name) != NULL ||
           clawt_agent_id_is_reserved(name);
}

gchar *
clawt_daemon_ephemeral_spawn(ClawtDaemon  *self,
                             const gchar  *parent_id,
                             JsonObject   *args,
                             GError      **error)
{
    ClawtAgentConfig *parent;
    ClawtAgentConfig *created;
    const gchar *name = clawt_ipc_payload_string(args, "name");
    const gchar *description = clawt_ipc_payload_string(args, "description");
    const gchar *purpose = clawt_ipc_payload_string(args, "purpose");
    const gchar *provider = clawt_ipc_payload_string(args, "provider");
    const gchar *model = clawt_ipc_payload_string(args, "model");
    const gchar *image = clawt_ipc_payload_string(args, "container_image");
    const gchar *computer = NULL;
    g_autoptr(GPtrArray) mine = NULL;
    g_autoptr(GPtrArray) all = NULL;
    g_autofree gchar *team = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *stamp = NULL;
    gint64 per_parent;
    gint64 fleet;

    parent = clawt_config_get_agent(self->config, parent_id);
    if (parent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no agent '%s'", parent_id);
        return NULL;
    }
    if (clawt_agent_config_is_ephemeral(parent) ||
        !clawt_agent_config_get_boolean(parent, "tools.ephemeral")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "%s may not spawn ephemeral agents (tools.ephemeral)",
                    parent_id);
        return NULL;
    }
    if (description == NULL || *description == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "description is required: one line saying what "
                            "this helper is for");
        return NULL;
    }

    /* the limits, each naming what is holding the room */
    per_parent = clawt_config_get_int(self->config,
                                      "orchestration.ephemeral_max_per_parent");
    fleet = clawt_config_get_int(self->config, "orchestration.ephemeral_max_fleet");
    mine = children_of(self, parent_id);
    all = children_of(self, NULL);
    if ((gint64)mine->len >= per_parent) {
        GString *names = g_string_new(NULL);
        guint i;

        for (i = 0; i < mine->len; i++)
            g_string_append_printf(names, "%s%s", i > 0 ? ", " : "",
                clawt_agent_config_get_id(g_ptr_array_index(mine, i)));
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "you already have %u ephemeral agent%s, the most "
                    "orchestration.ephemeral_max_per_parent allows (%s); reap "
                    "one that is done with clawtilla_ephemeral_reap first",
                    mine->len, mine->len == 1 ? "" : "s",
                    names->len > 0 ? names->str : "none");
        g_string_free(names, TRUE);
        return NULL;
    }
    if ((gint64)all->len >= fleet) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "the fleet already has %u ephemeral agents, the most "
                    "orchestration.ephemeral_max_fleet allows", all->len);
        return NULL;
    }

    if (!pick_computer(parent, clawt_ipc_payload_string(args, "computer"),
                       &computer, error))
        return NULL;

    /* a name the parent chose, else one of ours */
    if (name != NULL && *name != '\0') {
        if (!clawt_is_valid_id(name) || name_taken(name, self)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                        "'%s' cannot be the name: it is taken or not usable "
                        "as an id (lowercase letters, digits, '-' and '_')",
                        name);
            return NULL;
        }
        id = g_strdup(name);
    } else {
        id = clawt_ephemeral_name_generate(NULL, name_taken, self);
    }

    team = ensure_team(self, error);
    if (team == NULL)
        return NULL;

    created = clawt_config_add_ephemeral_agent(self->config, id, parent_id, error);
    if (created == NULL)
        return NULL;

    stamp = g_strdup_printf("%" G_GINT64_FORMAT, g_get_real_time() / G_USEC_PER_SEC);
    clawt_agent_config_set_ephemeral_string(created, "parent", parent_id);
    clawt_agent_config_set_ephemeral_string(created, "created_at", stamp);
    clawt_agent_config_set_ephemeral_string(created, "brief", description);
    clawt_agent_config_set_from_string(created, "description", description);
    clawt_agent_config_set_from_string(created, "team", team);
    clawt_agent_config_set_from_string(created, "team_role", "member");
    if (provider != NULL && *provider != '\0')
        clawt_agent_config_set_from_string(created, "model.provider", provider);
    if (model != NULL && *model != '\0')
        clawt_agent_config_set_from_string(created, "model.model", model);
    if (computer != NULL)
        clawt_agent_config_set_from_string(created, "computer.type", computer);
    if (g_strcmp0(computer, "container") == 0 && image != NULL && *image != '\0')
        clawt_agent_config_set_from_string(created, "computer.container.image", image);

    {
        gint shape;
        const gchar *color;

        clawt_face_pick_random(&shape, &color);
        clawt_agent_config_set_from_string(created, "face",
                                           clawt_face_shape_name(shape));
        clawt_agent_config_set_from_string(created, "color", color);
    }

    /* rolled back, not kept, when it could not work: see create_agent */
    if (!clawt_agent_config_validate_computer(created, error) ||
        !clawt_workspace_scaffold_with_mission(created, purpose, NULL, error)) {
        clawt_config_remove_ephemeral_agent(self->config, id);
        return NULL;
    }
    if (!clawt_daemon_ephemeral_save(self, error)) {
        clawt_config_remove_ephemeral_agent(self->config, id);
        return NULL;
    }

    clawt_agent_manager_load(self->agents, NULL);
    clawt_event_bus_emit(self->bus, "agent.created", id);

    {
        g_autoptr(GError) start_error = NULL;

        /* it exists either way; a start that failed is said, not undone */
        if (!self->ephemeral_no_start &&
            !clawt_daemon_start_agent(self, id, &start_error))
            g_warning("ephemeral agent %s (for %s) did not start: %s", id,
                      parent_id, start_error != NULL ? start_error->message : "?");
    }
    return g_steal_pointer(&id);
}

/* The tasks still open on @agent_id, as a list of ids for a refusal. */
static gchar *
open_tasks(ClawtDaemon *self, const gchar *agent_id)
{
    g_autoptr(GPtrArray) tasks = clawt_task_manager_list(self->tasks, agent_id, FALSE);
    GString *out = NULL;
    guint i;

    for (i = 0; tasks != NULL && i < tasks->len; i++) {
        ClawtTask *task = g_ptr_array_index(tasks, i);

        if (clawt_task_state_is_finished(clawt_task_get_state(task)))
            continue;
        if (out == NULL)
            out = g_string_new(NULL);
        else
            g_string_append(out, ", ");
        g_string_append(out, clawt_task_get_id(task));
    }
    return out != NULL ? g_string_free(out, FALSE) : NULL;
}

gboolean
clawt_daemon_ephemeral_reap(ClawtDaemon  *self,
                            const gchar  *caller_id,
                            const gchar  *agent_id,
                            gboolean      force,
                            GError      **error)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    const gchar *parent;
    g_autofree gchar *busy = NULL;
    g_autoptr(GError) purge_error = NULL;

    if (agent == NULL || !clawt_agent_config_is_ephemeral(agent)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "'%s' is not an ephemeral agent", agent_id);
        return FALSE;
    }
    parent = clawt_agent_config_get_ephemeral_string(agent, "parent");

    /* the parent, or the operator (no caller); never a sibling */
    if (caller_id != NULL && g_strcmp0(caller_id, parent) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "%s is %s's to reap, not yours", agent_id,
                    parent != NULL ? parent : "nobody's");
        return FALSE;
    }

    busy = open_tasks(self, agent_id);
    if (busy != NULL && !force) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "%s still has work open (%s); wait for it, cancel it, or "
                    "reap with force to drop it", agent_id, busy);
        return FALSE;
    }

    clawt_daemon_stop_agent(self, agent_id, TRUE);
    if (!clawt_daemon_purge_agent_files(self, agent, NULL, &purge_error))
        g_warning("ephemeral agent %s: files not removed: %s", agent_id,
                  purge_error != NULL ? purge_error->message : "?");

    clawt_observer_drop_agent(self->observer, agent_id);
    clawt_takeover_clear_agent(self->takeover, agent_id);
    clawt_config_remove_ephemeral_agent(self->config, agent_id);
    if (!clawt_daemon_ephemeral_save(self, error))
        return FALSE;
    clawt_agent_manager_load(self->agents, NULL);
    clawt_event_bus_emit(self->bus, "agent.removed", agent_id);
    return TRUE;
}

/*
 * A parent removed from the fleet leaves its helpers to the chief of
 * staff, with a notice -- not reaped, because whatever they were doing
 * may be the reason somebody removed the parent in a hurry.
 */
void
clawt_daemon_ephemeral_reparent(ClawtDaemon *self, const gchar *gone_id)
{
    g_autoptr(GPtrArray) orphans = children_of(self, gone_id);
    ClawtAgent *chief_agent;
    const gchar *chief;
    guint i;

    if (orphans->len == 0)
        return;

    chief_agent = clawt_agent_manager_get_chief_of_staff(self->agents);
    chief = chief_agent != NULL ? clawt_agent_get_id(chief_agent) : NULL;

    for (i = 0; i < orphans->len; i++) {
        ClawtAgentConfig *orphan = g_ptr_array_index(orphans, i);

        clawt_agent_config_set_ephemeral_string(orphan, "parent", chief);
        clawt_agent_config_set_ephemeral_string(orphan, "reminded_at", NULL);
    }
    {
        g_autoptr(GError) error = NULL;

        if (!clawt_daemon_ephemeral_save(self, &error))
            g_warning("ephemeral agents: %s's helpers were handed on but not "
                      "saved: %s", gone_id, error->message);
    }

    if (chief != NULL && self->rooms != NULL && self->router != NULL) {
        ClawtRoom *room = clawt_room_manager_get_direct(self->rooms, "user", chief);
        g_autoptr(ClawtMessage) message = NULL;
        GString *body = g_string_new(NULL);

        g_string_append_printf(body, "%s was removed from the fleet, so its "
                               "ephemeral agents are yours now: ", gone_id);
        for (i = 0; i < orphans->len; i++)
            g_string_append_printf(body, "%s%s", i > 0 ? ", " : "",
                clawt_agent_config_get_id(g_ptr_array_index(orphans, i)));
        g_string_append(body, ". Reap each one with clawtilla_ephemeral_reap "
                        "when you are sure nothing it was doing is needed.");
        if (room != NULL) {
            message = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER,
                                        body->str);
            clawt_message_set_depth(message, 0);
            clawt_message_set_invites_reply(message, FALSE);
            clawt_message_set_only_for(message, chief);
            clawt_mailbox_router_send(self->router, message, NULL);
        }
        g_string_free(body, TRUE);
    }
    clawt_event_bus_emit(self->bus, "agent.changed", NULL);
}

/* ── The reminder ───────────────────────────────────────────────── */

/*
 * A helper with nothing open and at least one finished task is done as
 * far as anybody can see; after ephemeral_reap_reminder_hours of that,
 * its parent hears about it once.  New work clears both stamps, so a
 * helper given a second job is reminded about afresh when that ends.
 */
void
clawt_daemon_ephemeral_sweep(ClawtDaemon *self, gint64 now)
{
    g_autoptr(GPtrArray) helpers = NULL;
    gint64 hours;
    gboolean changed = FALSE;
    guint i;

    if (self->config == NULL || self->tasks == NULL)
        return;

    hours = clawt_config_get_int(self->config,
                                 "orchestration.ephemeral_reap_reminder_hours");
    helpers = children_of(self, NULL);

    for (i = 0; i < helpers->len; i++) {
        ClawtAgentConfig *helper = g_ptr_array_index(helpers, i);
        const gchar *id = clawt_agent_config_get_id(helper);
        const gchar *done = clawt_agent_config_get_ephemeral_string(helper, "done_at");
        const gchar *parent = clawt_agent_config_get_ephemeral_string(helper, "parent");
        g_autofree gchar *busy = open_tasks(self, id);
        g_autoptr(GPtrArray) all = clawt_task_manager_list(self->tasks, id, TRUE);

        if (busy != NULL || all == NULL || all->len == 0) {
            if (done != NULL) {
                clawt_agent_config_set_ephemeral_string(helper, "done_at", NULL);
                clawt_agent_config_set_ephemeral_string(helper, "reminded_at", NULL);
                changed = TRUE;
            }
            continue;
        }

        if (done == NULL) {
            g_autofree gchar *stamp = g_strdup_printf("%" G_GINT64_FORMAT, now);

            clawt_agent_config_set_ephemeral_string(helper, "done_at", stamp);
            changed = TRUE;
            continue;
        }

        if (hours <= 0 ||
            clawt_agent_config_get_ephemeral_string(helper, "reminded_at") != NULL ||
            now - g_ascii_strtoll(done, NULL, 10) < hours * 3600 || parent == NULL)
            continue;

        {
            ClawtRoom *room = self->rooms != NULL
                ? clawt_room_manager_get_direct(self->rooms, "user", parent) : NULL;
            g_autofree gchar *body = g_strdup_printf(
                "Your ephemeral agent %s has had nothing open for %" G_GINT64_FORMAT
                " hours. If its work is done, reap it with clawtilla_ephemeral_reap; "
                "if you still need it, give it the next piece. This is the only "
                "reminder.", id, hours);
            g_autofree gchar *stamp = g_strdup_printf("%" G_GINT64_FORMAT, now);

            if (room != NULL && self->router != NULL) {
                g_autoptr(ClawtMessage) message = clawt_message_new(
                    clawt_room_get_id(room), CLAWT_SYSTEM_SENDER, body);

                clawt_message_set_depth(message, 0);
                clawt_message_set_invites_reply(message, FALSE);
                clawt_message_set_only_for(message, parent);
                clawt_mailbox_router_send(self->router, message, NULL);
            }
            clawt_agent_config_set_ephemeral_string(helper, "reminded_at", stamp);
            changed = TRUE;
        }
    }

    if (changed) {
        g_autoptr(GError) error = NULL;

        if (!clawt_daemon_ephemeral_save(self, &error))
            g_warning("ephemeral agents: not saved: %s", error->message);
    }
}

static gboolean
on_sweep(gpointer user_data)
{
    clawt_daemon_ephemeral_sweep(user_data, g_get_real_time() / G_USEC_PER_SEC);
    return G_SOURCE_CONTINUE;
}

/* ── The tools' hooks ───────────────────────────────────────────── */

static gchar *
hook_spawn(const gchar *agent_id, JsonObject *arguments, gboolean *is_error,
           gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = clawt_daemon_ephemeral_spawn(self, agent_id, arguments,
                                                        &error);
    ClawtAgent *agent;

    if (id == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("Not spawned: %s", error->message);
    }
    agent = clawt_agent_manager_get(self->agents, id);
    return g_strdup_printf(
        "Spawned %s (%s). Hand it the work with clawtilla_delegate to %s, and "
        "reap it with clawtilla_ephemeral_reap when that work is done -- it "
        "stays, across restarts, until you do.",
        id, agent != NULL && clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_RUNNING
                ? "running" : "starting",
        id);
}

static gchar *
hook_list(const gchar *agent_id, gpointer user_data)
{
    return clawt_daemon_ephemeral_describe(user_data, agent_id);
}

static gchar *
hook_reap(const gchar *agent_id, JsonObject *arguments, gboolean *is_error,
          gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    const gchar *target = clawt_ipc_payload_string(arguments, "agent_id");

    if (target == NULL) {
        *is_error = TRUE;
        return g_strdup("agent_id is required: which of your helpers?");
    }
    if (!clawt_daemon_ephemeral_reap(user_data, agent_id, target,
            clawt_ipc_payload_boolean(arguments, "force", FALSE), &error)) {
        *is_error = TRUE;
        return g_strdup_printf("Not reaped: %s", error->message);
    }
    return g_strdup_printf("Reaped %s: stopped, and its workspace removed.", target);
}

static gboolean
hook_has_children(const gchar *agent_id, gpointer user_data)
{
    return clawt_daemon_ephemeral_has_children(user_data, agent_id);
}

void
clawt_daemon_ephemeral_setup(ClawtDaemon *self)
{
    static const ClawtMcpEphemeralHooks hooks = {
        hook_spawn, hook_list, hook_reap, hook_has_children
    };

    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_ephemeral_hooks(self->mcp_tools, &hooks, self, NULL);

    /* attached to the daemon's own context, pushed here, not the ambient one */
    if (self->ephemeral_sweep == NULL) {
        if (self->main_context != NULL)
            g_main_context_push_thread_default(self->main_context);
        self->ephemeral_sweep = clawt_timeout_add_seconds(
            EPHEMERAL_SWEEP_SECONDS, on_sweep, self);
        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
    }
}

void
clawt_daemon_ephemeral_teardown(ClawtDaemon *self)
{
    if (self->ephemeral_sweep != NULL) {
        g_source_destroy(self->ephemeral_sweep);
        g_clear_pointer(&self->ephemeral_sweep, g_source_unref);
    }
}

/* ── For the agent and the clients ──────────────────────────────── */

void
clawt_daemon_ephemeral_add_members(ClawtAgentConfig *agent, JsonBuilder *builder)
{
    gboolean ephemeral = clawt_agent_config_is_ephemeral(agent);

    json_builder_set_member_name(builder, "ephemeral");
    json_builder_add_boolean_value(builder, ephemeral);
    if (ephemeral) {
        const gchar *parent = clawt_agent_config_get_ephemeral_string(agent, "parent");
        const gchar *created = clawt_agent_config_get_ephemeral_string(agent, "created_at");
        const gchar *done = clawt_agent_config_get_ephemeral_string(agent, "done_at");

        if (parent != NULL) {
            json_builder_set_member_name(builder, "parent");
            json_builder_add_string_value(builder, parent);
        }
        if (created != NULL) {
            json_builder_set_member_name(builder, "ephemeral_since");
            json_builder_add_int_value(builder, g_ascii_strtoll(created, NULL, 10));
        }
        /* done as far as anybody can see, and waiting to be reaped */
        json_builder_set_member_name(builder, "awaiting_reap");
        json_builder_add_boolean_value(builder, done != NULL);
    }
}

gchar *
clawt_daemon_ephemeral_describe(ClawtDaemon *self, const gchar *parent_id)
{
    g_autoptr(GPtrArray) mine = children_of(self, parent_id);
    GString *out = g_string_new(NULL);
    guint i;

    if (mine->len == 0) {
        g_string_free(out, TRUE);
        return g_strdup(
            "You have no ephemeral agents. Spawn one with "
            "clawtilla_ephemeral_spawn when a piece of work deserves its own "
            "helper.");
    }

    g_string_append_printf(out, "Your ephemeral agents (%u):\n", mine->len);
    for (i = 0; i < mine->len; i++) {
        ClawtAgentConfig *helper = g_ptr_array_index(mine, i);
        const gchar *id = clawt_agent_config_get_id(helper);
        ClawtAgent *agent = clawt_agent_manager_get(self->agents, id);
        g_autofree gchar *busy = open_tasks(self, id);

        g_string_append_printf(out, "- %s: %s. %s; %s.\n", id,
            clawt_agent_config_get_ephemeral_string(helper, "brief"),
            agent != NULL ? clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                               clawt_agent_get_state(agent))
                          : "not loaded",
            busy != NULL ? "open work" : "nothing open -- reap it if it is done");
    }
    g_string_append(out, "Delegate to one with clawtilla_delegate; reap it with "
                    "clawtilla_ephemeral_reap when its work is done.");
    return g_string_free(out, FALSE);
}

JsonNode *
clawt_daemon_handle_ephemeral(ClawtDaemon *self,
                              const gchar *kind,
                              JsonNode    *request,
                              JsonObject  *payload,
                              gboolean    *handled)
{
    g_autoptr(GError) error = NULL;

    if (g_strcmp0(kind, "agent.reap") != 0) {
        *handled = FALSE;
        return NULL;
    }
    *handled = TRUE;

    {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent?");
        /* the operator may reap anybody's, but open work still needs force */
        if (!clawt_daemon_ephemeral_reap(self, NULL, agent_id,
                clawt_ipc_payload_boolean(payload, "force", FALSE), &error))
            return clawt_ipc_error_new(request, error->code, error->message);
    }
    return clawt_ipc_response_new(request, NULL);
}
