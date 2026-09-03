/*
 * daemon-hold.c - control.pause and control.resume
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Restarting the daemon killed every agent mid-turn, so operators
 * stopped restarting: changes got batched and deferred because the cost
 * of applying one was "whatever every agent happened to be doing is
 * gone", unbounded and unknowable at the moment you press the button.
 *
 * A hold converts that into a bounded, observable operation.  Stop
 * handing out new work, let what is running finish, restart, then put
 * back exactly what was running.
 *
 * Pause is not stop, and that is the whole feature.  A stop closes the
 * link and takes the process down -- which is the loss being avoided, so
 * a pause that killed would not be one.  It also must not tear down a
 * container: `computer.container.keep` is false by default and stopping
 * a container removes it, and a paused agent that lost its computer is
 * not paused, it is broken.  Nothing here touches a computer at all.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

#include <string.h>

/* ── The gate ────────────────────────────────────────────────────── */

/*
 * Whether this agent's own runtime is gated.
 *
 * Asked of the runtime rather than of the ClawtHold, so what a client is
 * told is what the router will actually do.  The two can differ for one
 * moment -- an agent whose runtime was built before a hold landed -- and
 * reporting the intention rather than the effect is how a "held" badge
 * ends up over an agent that is quietly still taking work.
 */
gboolean
clawt_daemon_agent_held(ClawtAgent *agent)
{
    ClawtAgentRuntime *runtime;

    if (agent == NULL)
        return FALSE;

    runtime = clawt_agent_get_runtime(agent);

    return runtime != NULL && clawt_agent_runtime_is_held(runtime);
}

void
clawt_daemon_hold_reapply(ClawtDaemon *self)
{
    GPtrArray *agents;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->hold == NULL || self->agents == NULL)
        return;

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentRuntime *runtime = clawt_agent_get_runtime(agent);

        if (runtime == NULL)
            continue;

        clawt_agent_runtime_set_held(
            runtime, clawt_hold_covers(self->hold,
                                       clawt_agent_get_id(agent)));
    }
}

guint
clawt_daemon_hold_draining(ClawtDaemon *self)
{
    GPtrArray *agents;
    guint draining = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

    if (self->hold == NULL || !clawt_hold_is_any(self->hold) ||
        self->agents == NULL)
        return 0;

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);

        if (clawt_hold_covers(self->hold, clawt_agent_get_id(agent)) &&
            clawt_agent_get_busy(agent))
            draining++;
    }

    return draining;
}

void
clawt_daemon_hold_describe(ClawtDaemon *self, JsonBuilder *builder)
{
    guint draining;

    g_return_if_fail(CLAWT_IS_DAEMON(self));
    g_return_if_fail(builder != NULL);

    json_builder_set_member_name(builder, "hold");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "held");
    json_builder_add_boolean_value(
        builder, self->hold != NULL && clawt_hold_is_any(self->hold));

    json_builder_set_member_name(builder, "fleet");
    json_builder_add_boolean_value(
        builder, self->hold != NULL && clawt_hold_is_fleet(self->hold));

    /*
     * Written even at zero while a hold is on, because "draining, 0 in
     * flight" is the answer somebody is waiting for -- it is the defined
     * moment at which a restart is safe, which is the thing this feature
     * exists to give and which did not exist at all before.
     */
    draining = clawt_daemon_hold_draining(self);

    json_builder_set_member_name(builder, "draining");
    json_builder_add_int_value(builder, (gint64)draining);

    if (self->hold != NULL) {
        g_autoptr(GPtrArray) named = clawt_hold_held_agents(self->hold);
        guint i;

        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder,
                                   clawt_hold_get_since(self->hold));

        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        for (i = 0; i < named->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(named, i));

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);
}

/* ── Start ───────────────────────────────────────────────────────── */

void
clawt_daemon_hold_start(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    path = g_build_filename(self->state_dir, "hold.yaml", NULL);
    self->hold = clawt_hold_new(path);
    clawt_hold_load(self->hold);

    /*
     * The running set is *not* started here.  autostart_schedule() is
     * the one place agents come up, from an idle queue, because the
     * fleet coming up must not hold the loop -- so the remembered set
     * joins that queue through clawt_daemon_hold_was_running() rather
     * than being spawned from this function.  Two ways to start an agent
     * would be two behaviours.
     */

    if (clawt_hold_is_any(self->hold)) {
        clawt_daemon_hold_reapply(self);
        g_message("hold: %s still held; nothing will be delivered until "
                  "`clawtilla resume`",
                  clawt_hold_is_fleet(self->hold) ? "the whole fleet"
                                                  : "some agents");
    }

}

gboolean
clawt_daemon_hold_was_running(ClawtDaemon *self, const gchar *agent_id)
{
    GPtrArray *running;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    if (self->hold == NULL || agent_id == NULL)
        return FALSE;

    running = clawt_hold_get_running(self->hold);

    for (i = 0; running != NULL && i < running->len; i++) {
        if (g_strcmp0(g_ptr_array_index(running, i), agent_id) == 0)
            return TRUE;
    }

    return FALSE;
}

void
clawt_daemon_hold_forget_running(ClawtDaemon *self)
{
    g_autoptr(GError) error = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->hold == NULL ||
        clawt_hold_get_running(self->hold)->len == 0)
        return;

    clawt_hold_set_running(self->hold, NULL);

    if (!clawt_hold_save(self->hold, &error))
        g_warning("hold: could not write the record: %s",
                  error != NULL ? error->message : "unknown");
}

/* ── The verbs ───────────────────────────────────────────────────── */

/*
 * Which agents are running right now, for the record.
 */
static GPtrArray *
running_agent_ids(ClawtDaemon *self)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *agents;
    guint i;

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentState state = clawt_agent_get_state(agent);

        if (state == CLAWT_AGENT_STATE_RUNNING ||
            state == CLAWT_AGENT_STATE_STARTING ||
            state == CLAWT_AGENT_STATE_DEGRADED)
            g_ptr_array_add(out, g_strdup(clawt_agent_get_id(agent)));
    }

    return out;
}

JsonNode *
clawt_daemon_handle_hold(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
){
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *agent_id;

    *handled = TRUE;
    builder = json_builder_new();
    agent_id = clawt_ipc_payload_string(payload, "agent");

    if (agent_id != NULL && *agent_id == '\0')
        agent_id = NULL;

    if (g_strcmp0(kind, "control.pause") == 0) {
        if (agent_id != NULL &&
            clawt_agent_manager_get(self->agents, agent_id) == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no agent called that");

        /*
         * The running set is recorded on the *first* hold only.  A
         * second pause while one is already on would otherwise overwrite
         * it with whatever is running now -- which, under a hold, is a
         * shrinking set, so pausing twice would quietly lose agents from
         * what gets resumed.
         */
        if (!clawt_hold_is_any(self->hold)) {
            g_autoptr(GPtrArray) running = running_agent_ids(self);

            clawt_hold_set_running(self->hold, running);
        }

        clawt_hold_apply(self->hold, agent_id);
        clawt_daemon_hold_reapply(self);

        if (!clawt_hold_save(self->hold, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "hold.changed",
                             agent_id != NULL ? agent_id : "");

        json_builder_begin_object(builder);
        clawt_daemon_hold_describe(self, builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "control.resume") == 0) {
        /*
         * Releasing one agent while the fleet is held is refused rather
         * than accepted and ignored.  The fleet hold would still cover
         * it, so a caller told it worked would sit watching an agent
         * that never moves -- and the remedy is a different command,
         * which the refusal names.
         */
        if (agent_id != NULL && clawt_hold_is_fleet(self->hold))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "the whole fleet is held, so releasing one agent would "
                "change nothing; use `resume` with no agent");

        clawt_hold_release(self->hold, agent_id);
        clawt_daemon_hold_reapply(self);

        if (!clawt_hold_save(self->hold, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Nudged rather than waited for.  Every held message is still
         * queued, in order, and the drain hands it over on the agent's
         * own link -- this just means a resume is visible now rather
         * than at whatever the next arrival happens to be.
         */
        {
            GPtrArray *agents = clawt_agent_manager_list(self->agents);
            guint i;

            for (i = 0; agents != NULL && i < agents->len; i++) {
                ClawtAgent *agent = g_ptr_array_index(agents, i);

                if (!clawt_hold_covers(self->hold,
                                       clawt_agent_get_id(agent)))
                    clawt_mailbox_router_drain(
                        self->router, clawt_agent_get_id(agent));
            }
        }

        clawt_event_bus_emit(self->bus, "hold.changed",
                             agent_id != NULL ? agent_id : "");

        json_builder_begin_object(builder);
        clawt_daemon_hold_describe(self, builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;

    return NULL;
}
