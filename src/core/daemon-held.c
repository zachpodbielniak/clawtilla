/*
 * daemon-held.c - agents whose every turn is refused
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent says it is held with agent.failover, state "held": every
 * turn it takes is refused for a reason that is its account's -- a login
 * that is gone, a usage limit -- and nothing could take the turn
 * elsewhere.  ClawtAgent keeps that (clawt_agent_set_failover()), shows
 * it as degraded, and the router stops feeding it
 * (clawt_agent_is_turn_held()).  Two things are left, and they are here:
 *
 * - The drain when the hold runs out.  The router only delivers when
 *   something asks it to, and nothing would: the mail is already
 *   queued.  So the hold arms a timer for its end -- in slices of at
 *   most a day, re-armed while the agent is still held, so a hold of
 *   weeks neither wraps the timer nor drains early.
 *
 * - The alert.  A login is the account's, so every agent on it is held
 *   at the same moment, and one notification per agent is the noise
 *   that once turned error alerts off altogether.  One per account,
 *   then, and only once the account has stayed held for the alert
 *   delay: a hold that ends inside it is a blip, and alerts nobody.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* How long an account must stay held before anybody is told. */
#define HELD_ALERT_DELAY_MS (120u * 1000u)

/*
 * The longest one wake timer is armed for.  A hold can run for weeks
 * (CLAWT_AGENT_HELD_MAX_SECONDS), and a GSource timeout is a guint of
 * milliseconds, which wraps a little short of 50 days: a hold past that
 * armed in one go fired early, drained while the agent was still held,
 * and nothing armed it again.  So a long hold is waited out in slices of
 * a day, each re-armed from the clock when it runs.
 */
#define HELD_WAKE_SLICE_MS (24u * 3600u * 1000u)

typedef struct {
    ClawtDaemon *daemon;    /* borrowed: the daemon owns this */
    gchar       *account;   /* the key */
    gchar       *label;     /* how a person names it */
    GSource     *timer;     /* pending alert, or NULL */
    gboolean     alerted;
} HeldAccount;

static void
held_account_free(gpointer data)
{
    HeldAccount *held = data;

    if (held->timer != NULL) {
        g_source_destroy(held->timer);
        g_source_unref(held->timer);
    }
    g_free(held->account);
    g_free(held->label);
    g_free(held);
}

static void
destroy_source(gpointer data)
{
    GSource *source = data;

    g_source_destroy(source);
    g_source_unref(source);
}

/*
 * The login an agent's turns run on: its `accounts:` entry, or its
 * provider's default login.  Two agents with the same key are behind
 * the same wall.  An account's key names its provider too when the
 * agent's config says it: an account id is the operator's label, and two
 * providers' logins under one label are two walls, not one.
 */
static gchar *
account_key(ClawtAgent *agent, gchar **label_out)
{
    ClawtAgentConfig *config = clawt_agent_get_config(agent);
    const gchar *account = NULL;
    const gchar *provider = NULL;
    gboolean provider_known;

    if (config != NULL) {
        account = clawt_agent_config_get_string(config, "model.account");
        provider = clawt_agent_config_get_string(config, "model.provider");
    }

    provider_known = (provider != NULL && *provider != '\0');
    if (!provider_known)
        provider = "default";

    if (account != NULL && *account != '\0') {
        *label_out = g_strdup_printf("account %s", account);
        return provider_known
            ? g_strdup_printf("account:%s:%s", provider, account)
            : g_strdup_printf("account:%s", account);
    }

    *label_out = g_strdup_printf("the default %s login", provider);
    return g_strdup_printf("default:%s", provider);
}

/* The held agents on @account, and the first one's failover. */
static guint
count_held(ClawtDaemon *self, const gchar *account, ClawtAgent **first_out)
{
    GPtrArray *agents = clawt_agent_manager_list(self->agents);
    guint held = 0;
    guint i;

    *first_out = NULL;

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        g_autofree gchar *label = NULL;
        g_autofree gchar *key = NULL;

        if (!clawt_failover_is_held(clawt_agent_get_failover(agent)))
            continue;

        key = account_key(agent, &label);
        if (g_strcmp0(key, account) != 0)
            continue;

        if (*first_out == NULL)
            *first_out = agent;
        held++;
    }

    return held;
}

static gboolean
on_alert_due(gpointer user_data)
{
    HeldAccount *held = user_data;
    ClawtDaemon *self = held->daemon;
    g_autoptr(ClawtNotification) notification = NULL;
    g_autofree gchar *title = NULL;
    g_autofree gchar *body = NULL;
    ClawtAgent *first = NULL;
    JsonObject *failover;
    guint n;

    g_clear_pointer(&held->timer, g_source_unref);

    n = count_held(self, held->account, &first);
    if (n == 0 || held->alerted)
        return G_SOURCE_REMOVE;

    held->alerted = TRUE;
    failover = clawt_agent_get_failover(first);

    title = g_strdup_printf("%s cannot serve turns", held->label);
    body = g_strdup_printf(
        "%u agent%s held: every turn is refused because it %s. %s",
        n, n == 1 ? " is" : "s are",
        clawt_failover_reason_phrase(
            json_object_get_string_member_with_default(failover, "reason",
                                                       NULL)),
        json_object_get_string_member_with_default(failover, "message",
                                                   ""));

    /* "8 agents are held" reads as "8 agents" to a matcher, and to Ben */
    notification = clawt_notification_new(CLAWT_NOTIFY_EVENTS_ERROR,
                                          clawt_agent_get_id(first), NULL,
                                          title, body);

    g_message("held: %s: %s", title, body);
    clawt_daemon_notify(self, notification);

    return G_SOURCE_REMOVE;
}

typedef struct {
    ClawtDaemon *daemon;    /* borrowed: the daemon owns the timer */
    gchar       *agent_id;
} Wake;

static void
wake_free(gpointer data)
{
    Wake *wake = data;

    g_free(wake->agent_id);
    g_free(wake);
}

static void arm_wake(ClawtDaemon *self, ClawtAgent *agent);

/*
 * A wake ran.  Either the hold is over -- drain, and the queued mail goes
 * -- or this was one slice of a longer hold (or the wall clock moved
 * back), and the rest of it is armed again.  Draining while the agent is
 * still held delivers nothing, and with no timer left nothing would ever
 * deliver the mail after the hold did run out.
 */
static gboolean
on_hold_ran_out(gpointer user_data)
{
    Wake *wake = user_data;
    ClawtDaemon *self = wake->daemon;
    g_autofree gchar *agent_id = g_strdup(wake->agent_id);
    ClawtAgent *agent;

    /*
     * Out of the table first, which destroys this source; @wake itself
     * lives until this callback returns, but is not used past here.  The
     * drain's own turn may be refused, and the renewal it sends arms a
     * fresh timer under the same key.
     */
    if (self->held_wakes != NULL)
        g_hash_table_remove(self->held_wakes, agent_id);

    agent = (self->agents != NULL)
        ? clawt_agent_manager_get(self->agents, agent_id) : NULL;

    /* The router's own gate, so the two cannot disagree about "held". */
    if (agent != NULL &&
        clawt_agent_is_turn_held(agent, g_get_real_time() / G_USEC_PER_SEC)) {
        arm_wake(self, agent);
        return G_SOURCE_REMOVE;
    }

    if (self->router != NULL)
        clawt_mailbox_router_drain(self->router, agent_id);

    return G_SOURCE_REMOVE;
}

/*
 * Milliseconds to the next wake for a hold that runs out at @held_until
 * (Unix seconds), from @now (microseconds): a little past the second, so
 * the gate's "now < until" has passed, and never more than @slice_ms.
 * Worked in gint64 and clamped before it becomes a guint, so neither a
 * far-off hold nor a huge @held_until can wrap.
 */
static guint
wake_delay_ms(gint64 now, gint64 held_until, guint slice_ms)
{
    gint64 until;
    gint64 left;

    until = (held_until > G_MAXINT64 / G_USEC_PER_SEC)
        ? G_MAXINT64 : held_until * G_USEC_PER_SEC;
    left = (until > now) ? (until - now) / 1000 + 50 : 50;

    if (left > (gint64)slice_ms)
        left = slice_ms;

    return (guint)left;
}

static void
arm_wake(ClawtDaemon *self, ClawtAgent *agent)
{
    gint64 held_until = clawt_agent_get_held_until(agent);
    guint ms;
    GSource *source;

    if (self->held_wakes == NULL)
        self->held_wakes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, destroy_source);

    g_hash_table_remove(self->held_wakes, clawt_agent_get_id(agent));

    if (held_until <= 0)
        return;

    ms = wake_delay_ms(g_get_real_time(), held_until,
                       self->held_wake_slice_ms > 0
                           ? self->held_wake_slice_ms : HELD_WAKE_SLICE_MS);

    {
        Wake *wake = g_new0(Wake, 1);

        wake->daemon = self;
        wake->agent_id = g_strdup(clawt_agent_get_id(agent));
        source = g_timeout_source_new(ms);
        g_source_set_callback(source, on_hold_ran_out, wake, wake_free);
    }
    g_source_attach(source, self->main_context);
    g_hash_table_insert(self->held_wakes,
                        g_strdup(clawt_agent_get_id(agent)), source);
}

void
clawt_daemon_held_changed(ClawtDaemon *self, ClawtAgent *agent)
{
    g_autofree gchar *label = NULL;
    g_autofree gchar *key = NULL;
    ClawtAgent *first = NULL;
    HeldAccount *held;

    g_return_if_fail(CLAWT_IS_DAEMON(self));
    g_return_if_fail(CLAWT_IS_AGENT(agent));

    arm_wake(self, agent);

    if (self->held_accounts == NULL)
        self->held_accounts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                    g_free,
                                                    held_account_free);

    key = account_key(agent, &label);

    /* Nothing on the account is held any more: the alert, if any, ends. */
    if (count_held(self, key, &first) == 0) {
        g_hash_table_remove(self->held_accounts, key);
        return;
    }

    if (g_hash_table_contains(self->held_accounts, key))
        return;

    held = g_new0(HeldAccount, 1);
    held->daemon = self;
    held->account = g_strdup(key);
    held->label = g_steal_pointer(&label);
    held->timer = g_timeout_source_new(self->held_alert_delay_ms > 0
                                       ? self->held_alert_delay_ms
                                       : HELD_ALERT_DELAY_MS);
    g_source_set_callback(held->timer, on_alert_due, held, NULL);
    g_source_attach(held->timer, self->main_context);
    g_hash_table_insert(self->held_accounts, g_strdup(key), held);
}

void
clawt_daemon_held_stop(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    g_clear_pointer(&self->held_wakes, g_hash_table_unref);
    g_clear_pointer(&self->held_accounts, g_hash_table_unref);
}
