/*
 * daemon-idle.c - Putting idle agents to sleep, and telling somebody about the cap
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two things a running fleet does that nobody was watching:
 *
 * - An agent that has not taken a turn for days still holds its
 *   process, its ports and its computer.  `agents.runtime.idle_stop_hours`
 *   stops it, and the next message queued for it starts it again, so a
 *   sleeping agent loses nothing: the message waits in its mailbox, which
 *   is durable, while it comes up.
 * - A daily cap refusing work.  The router enforces
 *   `orchestration.daily_fleet_budget_micros` and publishes
 *   `spending.limit-reached` once per scope and day; this hands that to a
 *   notifier that asked for `budget`, since otherwise the fleet simply
 *   stops answering until midnight and only the event log says why.
 *
 * The idle check is a one-minute tick on the daemon's own context.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include <string.h>

#define IDLE_TICK_SECONDS (60)

/* When @agent_id last did anything: its last turn, or its start if later. */
static gint64
last_activity(ClawtDaemon *self, const gchar *agent_id)
{
    g_autofree gchar *state_dir = clawt_config_agent_state_dir(self->config, agent_id);
    g_autofree gchar *db_path = NULL;
    ClawtUsageTotals totals;
    gint64 last_turn_at = 0;
    gpointer started;

    if (state_dir != NULL) {
        db_path = clawt_usage_database_path(state_dir);
        if (!clawt_usage_read_report(db_path, G_MAXINT64, NULL, 0, &totals,
                                     &last_turn_at, NULL, NULL))
            last_turn_at = 0;
    }

    /*
     * An agent that has never taken a turn is measured from when it
     * started -- or it would be stopped the minute it came up.  And one
     * restarted after a long sleep is measured from the restart, not
     * from a turn days ago.
     */
    started = g_hash_table_lookup(self->idle_started, agent_id);
    if (started != NULL)
        last_turn_at = MAX(last_turn_at, *(gint64 *)started);

    return last_turn_at;
}

static void
stop_idle_agents(ClawtDaemon *self, gint64 now)
{
    GPtrArray *agents = clawt_agent_manager_list(self->agents);
    guint i;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        const gchar *id = clawt_agent_get_id(agent);
        gint64 hours = clawt_agent_config_get_int(clawt_agent_get_config(agent),
                                                  "runtime.idle_stop_hours");
        gint64 since;
        g_autoptr(ClawtEvent) event = NULL;
        g_autofree gchar *hours_text = NULL;

        if (hours <= 0 || clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_RUNNING)
            continue;

        /* never mid-turn: an agent typing somewhere is not idle */
        if (clawt_agent_get_typing_rooms(agent) > 0)
            continue;

        since = last_activity(self, id);
        if (since <= 0 || now - since < hours * 3600)
            continue;

        g_message("%s: no turn for %" G_GINT64_FORMAT " hours; stopping it "
                  "until a message arrives (agents.runtime.idle_stop_hours)",
                  id, hours);

        if (!clawt_daemon_stop_agent(self, id, TRUE))
            continue;

        g_hash_table_add(self->idle_stopped, g_strdup(id));

        event = clawt_event_new("agent.idle_stopped", id);
        hours_text = g_strdup_printf("%" G_GINT64_FORMAT, hours);
        clawt_event_set_detail(event, "hours", hours_text);
        clawt_event_bus_publish(self->bus, event);
    }
}

void
clawt_daemon_idle_tick(ClawtDaemon *self)
{
    gint64 now;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->agents == NULL || self->config == NULL)
        return;

    now = g_get_real_time() / G_USEC_PER_SEC;
    stop_idle_agents(self, now);
}

static gboolean
on_idle_tick(gpointer user_data)
{
    clawt_daemon_idle_tick(user_data);
    return G_SOURCE_CONTINUE;
}

typedef struct {
    ClawtDaemon *daemon;   /* ref */
    gchar       *agent_id;
} Wake;

static void
wake_free(gpointer data)
{
    Wake *wake = data;

    g_object_unref(wake->daemon);
    g_free(wake->agent_id);
    g_free(wake);
}

static gboolean
on_wake(gpointer user_data)
{
    Wake *wake = user_data;
    g_autoptr(GError) error = NULL;

    if (!clawt_daemon_start_agent(wake->daemon, wake->agent_id, &error))
        g_warning("%s: a message arrived but the agent would not start: %s",
                  wake->agent_id, error != NULL ? error->message : "unknown");

    return G_SOURCE_REMOVE;
}

/*
 * A cap refused work: say so to a notifier that asked for `budget`.
 * The router already publishes this once per scope and day, so there is
 * nothing to deduplicate here.  @subject is `fleet` or `agent:<id>`.
 */
static void
notify_limit(ClawtDaemon *self, ClawtEvent *event, const gchar *subject)
{
    g_autoptr(ClawtNotification) notification = NULL;
    g_autofree gchar *spent = NULL;
    g_autofree gchar *cap = NULL;
    g_autofree gchar *body = NULL;
    const gchar *agent_id = NULL;

    if (self->notifier == NULL)
        return;

    if (g_str_has_prefix(subject, "agent:"))
        agent_id = subject + strlen("agent:");

    spent = clawt_usage_format_cost(clawt_event_get_detail_int(event, "cost_micros"));
    cap = clawt_usage_format_cost(clawt_event_get_detail_int(event, "limit_micros"));
    body = agent_id != NULL
        ? g_strdup_printf("%s has spent %s today, at its daily cap of %s. "
                          "New messages to it wait until midnight or a "
                          "higher cap.", agent_id, spent, cap)
        : g_strdup_printf("The fleet has spent %s today, at its daily cap "
                          "of %s. New work waits until midnight or a "
                          "higher cap.", spent, cap);

    notification = clawt_notification_new(CLAWT_NOTIFY_EVENTS_BUDGET, agent_id,
                                          NULL, "Daily spending cap reached",
                                          body);
    clawt_notifier_notify(self->notifier, notification);
}

/*
 * The edges: an agent started (by anyone) is no longer asleep and is
 * measured from now; a message queued for an agent this put to sleep
 * wakes it.  The wake is an idle on the daemon's context rather than a
 * start from inside the signal, which fires in the middle of the
 * router's send -- starting an agent there drains its mailbox while the
 * send is still adding to it.
 */
static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    const gchar *kind = clawt_event_get_kind(event);
    const gchar *subject = clawt_event_get_subject(event);

    (void)bus;

    if (subject == NULL)
        return;

    if (g_strcmp0(kind, "spending.limit-reached") == 0) {
        notify_limit(self, event, subject);
        return;
    }

    if (g_strcmp0(kind, "agent.started") == 0) {
        gint64 *now = g_new(gint64, 1);

        *now = g_get_real_time() / G_USEC_PER_SEC;
        g_hash_table_replace(self->idle_started, g_strdup(subject), now);
        g_hash_table_remove(self->idle_stopped, subject);
        return;
    }

    if (g_strcmp0(kind, "mailbox.queued") == 0 &&
        g_hash_table_remove(self->idle_stopped, subject)) {
        Wake *wake = g_new0(Wake, 1);
        GSource *source = g_idle_source_new();

        wake->daemon = g_object_ref(self);
        wake->agent_id = g_strdup(subject);
        g_source_set_callback(source, on_wake, wake, wake_free);
        g_source_attach(source, self->main_context);
        g_source_unref(source);
    }
}

void
clawt_daemon_idle_setup(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->idle_started == NULL)
        self->idle_started = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, g_free);
    if (self->idle_stopped == NULL)
        self->idle_stopped = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);

    if (self->bus != NULL && self->idle_bus_handler == 0)
        self->idle_bus_handler = g_signal_connect(self->bus, "event",
                                                  G_CALLBACK(on_bus_event), self);

    /* On the daemon's own context, named here where the source attaches. */
    if (self->idle_tick == NULL) {
        if (self->main_context != NULL)
            g_main_context_push_thread_default(self->main_context);
        self->idle_tick = clawt_timeout_add_seconds(IDLE_TICK_SECONDS,
                                                    on_idle_tick, self);
        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
    }
}

gboolean
clawt_daemon_idle_is_asleep(ClawtDaemon *self, const gchar *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    return self->idle_stopped != NULL &&
           g_hash_table_contains(self->idle_stopped, agent_id);
}

void
clawt_daemon_idle_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->idle_tick != NULL) {
        g_source_destroy(self->idle_tick);
        g_clear_pointer(&self->idle_tick, g_source_unref);
    }
    if (self->bus != NULL && self->idle_bus_handler != 0)
        g_signal_handler_disconnect(self->bus, self->idle_bus_handler);
    self->idle_bus_handler = 0;
    g_clear_pointer(&self->idle_started, g_hash_table_unref);
    g_clear_pointer(&self->idle_stopped, g_hash_table_unref);
}
