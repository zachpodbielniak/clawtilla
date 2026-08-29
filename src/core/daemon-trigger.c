/*
 * daemon-trigger.c - The receiver, and the client surface: trigger.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A routine is a clock; a trigger is an event.  Both end in the same
 * queued run against the same agent, so the delivery path below builds a
 * prompt and hands it to a #ClawtRoutineRunFunc -- the callback the
 * routine runner already uses -- rather than growing an execution path
 * of its own.  Two paths would differ exactly once and the difference
 * would be found by an operator rather than by a test.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * How long a receipt is worth keeping, and therefore how long a retry
 * stays idempotent. Seven days: longer than any forge's retry schedule,
 * short enough that a busy fleet's database does not grow without bound.
 */
#define DELIVERY_RETENTION_SECONDS (7 * 24 * 60 * 60)

/*
 * The largest delivery that will be read.
 *
 * A megabyte is far more than any forge's push payload and far less than
 * a body worth buffering from an unauthenticated caller. Refused at the
 * headers, before the body is read and therefore before any HMAC is
 * computed over it -- a cap enforced after buffering would still refuse,
 * having already paid for the attack it exists to stop.
 */
#define MAX_DELIVERY_BYTES ((gsize)(1024 * 1024))

/* The rate limit, per endpoint. */
#define RATE_WINDOW_SECONDS (60)
#define RATE_MAX_DELIVERIES (60)

/*
 * How many runs one trigger may have in flight.
 *
 * A push that fires a five-minute run, repeated twenty times while the
 * first is still going, is twenty concurrent agents on one machine. The
 * cap is checked *after* the duplicate check on purpose: a sender
 * retrying a delivery clawtilla already accepted must stay idempotent
 * even when the queue is full, so only new work consumes a slot.
 */
#define MAX_UNFINISHED_RUNS (4)

/* ── Running ─────────────────────────────────────────────────────── */

static gboolean
trigger_is_isolated(ClawtDaemon *self, const gchar *trigger_id)
{
    ClawtTrigger *trigger = clawt_config_get_trigger(self->config,
                                                     trigger_id);

    return trigger != NULL && clawt_trigger_get_boolean(trigger, "isolate");
}

/*
 * The #ClawtRoutineRunFunc a trigger run goes through.
 *
 * Deliberately the same signature as run_routine()'s, because it is the
 * same kind of work: a prompt nobody typed, queued behind whatever the
 * agent is already doing, reported as a task.  What differs is only the
 * room namespace and the event that is published.
 */
static const gchar *
run_trigger(const gchar *trigger_id, const gchar *agent_id,
            const gchar *prompt, gpointer user_data, GError **error)
{
    ClawtDaemon *self = user_data;
    ClawtTask *task;
    g_autofree gchar *room_id = NULL;
    const gchar *sender = "user";
    const gchar *target = agent_id;

    if (clawt_agent_manager_get(self->agents, agent_id) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "trigger '%s' names '%s', which is not an agent",
                    trigger_id, agent_id);
        return NULL;
    }

    /*
     * A room and a sender of its own, when the trigger asked for them --
     * both, for the reason the routine path documents: libreclaw's
     * session key is channel, room and sender together.
     */
    if (trigger_is_isolated(self, trigger_id)) {
        ClawtRoom *room = clawt_room_manager_get_trigger(self->rooms,
                                                         trigger_id,
                                                         agent_id);

        if (room != NULL) {
            room_id = g_strdup(clawt_room_get_id(room));
            sender = "trigger";
            target = room_id;
        }
    }

    task = clawt_task_manager_create(self->tasks, sender, agent_id, prompt,
                                     NULL, error);

    if (task == NULL)
        return NULL;

    /*
     * Started before it is delivered, as the routine path does: an agent
     * that answers instantly would otherwise complete a task that had
     * not been marked running, which reads as a run that never happened.
     */
    clawt_task_manager_start(self->tasks, clawt_task_get_id(task));

    if (clawt_mailbox_router_send_to(self->router, sender, target, prompt,
                                     clawt_task_get_id(task), 0, error) < 0) {
        clawt_task_manager_fail(self->tasks, clawt_task_get_id(task),
                                (error != NULL && *error != NULL)
                                    ? (*error)->message
                                    : "it could not be delivered");
        return NULL;
    }

    return clawt_task_get_id(task);
}

/* ── Secrets ─────────────────────────────────────────────────────── */

/*
 * The trigger's shared secret, resolved.
 *
 * Returned to this file and nowhere else. It never reaches an IPC
 * response, a log line, an event or a transcript: it is compared
 * against what the caller sent and freed.
 */
static gchar *
resolve_secret(ClawtDaemon *self, ClawtTrigger *trigger)
{
    g_autoptr(ClawtSecretRef) ref = clawt_trigger_get_secret(trigger,
                                                             "secret");
    g_autofree gchar *secrets_dir = NULL;

    if (ref == NULL)
        return NULL;

    secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");

    return clawt_secret_ref_resolve(
        ref, secrets_dir,
        (guint)clawt_config_get_int(self->config,
                                    "secrets.command_timeout_seconds"),
        NULL);
}

/*
 * Mints a secret, writes it 0600, and points the trigger at the file.
 *
 * From /dev/urandom through clawt_generate_token(), never g_random_*:
 * that is a Mersenne Twister, and a handful of its outputs give away
 * every other one. A predictable webhook secret is no secret at all --
 * the endpoint is public by construction.
 *
 * Returns: (transfer full): the secret, to be shown exactly once
 */
static gchar *
mint_secret(ClawtDaemon *self, ClawtTrigger *trigger, GError **error)
{
    g_autofree gchar *secrets_dir =
        clawt_config_get_path_value(self->config, "secrets.dir");
    g_autofree gchar *path = NULL;
    gchar *secret;

    if (!clawt_ensure_dir(secrets_dir, 0700, error))
        return NULL;

    secret = clawt_generate_token(error);

    if (secret == NULL)
        return NULL;

    path = clawt_trigger_secret_path(secrets_dir,
                                     clawt_trigger_get_id(trigger));

    /*
     * 0600 and no backup: a .bak of a credential is a second copy of the
     * secret nobody asked for, and it outlives the rotation that was
     * meant to retire it.
     */
    if (!clawt_write_file_atomic(path, secret, -1, 0600, FALSE, error)) {
        g_free(secret);
        return NULL;
    }

    clawt_trigger_set_secret(trigger, "secret", CLAWT_SECRET_BACKEND_FILE,
                             path);

    return secret;
}

/* ── Delivery ────────────────────────────────────────────────────── */

typedef struct {
    guint        status;
    const gchar *body;
} Answer;

static gchar *
finish(Answer answer, guint *out_status)
{
    *out_status = answer.status;

    return g_strdup(answer.body);
}

/*
 * One delivery, from the socket to a queued run.
 *
 * The order of the checks is the security, and it is this:
 *
 *   1. Is there an endpoint by that name, and a trigger behind it, and
 *      is it switched on?  All three answer 404, so scanning learns
 *      nothing and a disabled trigger does not advertise that it exists.
 *   2. Does it authenticate?  Before anything is parsed, over the raw
 *      bytes, in constant time.
 *   3. Is it a duplicate?  Before the pending cap, so a retry of
 *      accepted work stays idempotent even when the queue is full.
 *   4. Is it wanted -- the event list, the repository and branch
 *      filters?  A delivery outside them is *accepted* and recorded as
 *      ignored, because a sender told "error" for something you simply
 *      did not ask for will retry it for ever.
 *   5. Is this the handshake?  The first authenticated delivery is held
 *      and shown rather than run.
 *   6. Only then does anything start.
 */
static gchar *
on_delivery(const gchar  *endpoint,
            GHashTable   *headers,
            const gchar  *presented,
            const guchar *body,
            gsize         body_length,
            gpointer      user_data,
            guint        *out_status)
{
    ClawtDaemon *self = user_data;
    g_autofree gchar *trigger_id = NULL;
    ClawtTrigger *trigger;
    ClawtTriggerProvider provider;
    ClawtTriggerHandler *handler;
    g_autofree gchar *secret = NULL;
    g_autoptr(ClawtTriggerEvent) event = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reason = NULL;
    const gchar *agent;
    const gchar *task_id;

    if (self->trigger_store == NULL)
        return finish((Answer){ 503,
                                "no trigger store\n" }, out_status);

    trigger_id = clawt_trigger_store_trigger_for_endpoint(self->trigger_store,
                                                          endpoint);

    if (trigger_id == NULL)
        return finish((Answer){ 404, "no such endpoint\n" }, out_status);

    trigger = clawt_config_get_trigger(self->config, trigger_id);

    if (trigger == NULL)
        return finish((Answer){ 404, "no such endpoint\n" }, out_status);

    /*
     * A trigger that is switched off answers exactly as one that never
     * existed.  Distinguishing them would tell a caller which endpoints
     * are real, and turning a trigger off would announce it.
     *
     * Except while it is still waiting for its first delivery.
     *
     * `trigger.add` deliberately creates one disabled, and the handshake
     * is that the first authenticated delivery is captured and shown --
     * so refusing every delivery to a disabled trigger would mean the
     * capture could never happen, the pending flag could never clear,
     * and the trigger could never be switched on. Each half was right
     * and together they reached nobody; the end-to-end test is what
     * found it, because from outside it looked exactly like a forge
     * that had not been pointed at the right URL.
     */
    if (!clawt_trigger_get_boolean(trigger, "enabled") &&
        (self->trigger_store == NULL ||
         !clawt_trigger_store_is_pending_verification(self->trigger_store,
                                                      trigger_id)))
        return finish((Answer){ 404, "no such endpoint\n" }, out_status);

    provider = clawt_trigger_get_provider(trigger);

    /*
     * Configured intent first.  Forgejo also sends Gitea- and
     * GitHub-shaped headers, so sniffing cannot decide for a trigger
     * that named its provider -- and sniffing may never *widen* what a
     * configured trigger accepts. It is reached only for `generic`,
     * which is the setting that means "I did not say".
     */
    if (provider == CLAWT_TRIGGER_PROVIDER_GENERIC &&
        !clawt_trigger_has_key(trigger, "provider")) {
        ClawtTriggerProvider sniffed;

        if (clawt_trigger_sniff_provider(headers, &sniffed))
            provider = sniffed;
    }

    handler = clawt_trigger_handler_for(provider);
    secret = resolve_secret(self, trigger);

    /*
     * A capability URL first, and only where it is allowed.
     *
     * A sender that can name a URL and nothing else -- podomation's
     * webhook module is the one this exists for -- has no way to prove
     * anything in a header. clawt_trigger_verify_url_secret() answers
     * for the provider rather than for this call site, so a
     * `provider: forgejo` trigger goes on requiring its signature and a
     * forge cannot be opened by a string somebody put in a query.
     */
    if (!clawt_trigger_verify_url_secret(provider, secret, presented, NULL) &&
        !clawt_trigger_handler_verify(handler, secret, headers, body,
                                      body_length, &error)) {
        /*
         * Recorded without the event, because nothing here is trusted
         * enough to have been parsed. The message names no header, no
         * length and no trigger detail: a caller that cannot
         * authenticate learns only that it did not.
         */
        clawt_trigger_store_record(self->trigger_store, trigger_id, NULL,
                                   CLAWT_DELIVERY_REFUSED,
                                   error != NULL ? error->message
                                                 : "it did not authenticate",
                                   NULL);

        clawt_event_bus_emit(self->bus, "trigger.refused", trigger_id);

        return finish((Answer){ 401,
                                "no\n" }, out_status);
    }

    event = clawt_trigger_handler_normalise(handler, headers, body,
                                            body_length);

    if (event == NULL)
        return finish((Answer){ 400,
                                "that body could not be read\n" },
                      out_status);

    /*
     * The configured header, for a generic sender that names its events
     * somewhere of its own choosing. Applied here because this is the
     * only place that has both the trigger and the headers.
     */
    if (clawt_trigger_event_get_name(event) == NULL) {
        const gchar *header_name = clawt_trigger_get_string(trigger,
                                                            "header");

        if (header_name != NULL && *header_name != '\0') {
            g_autofree gchar *lowered = g_ascii_strdown(header_name, -1);

            clawt_trigger_event_set_identity(
                event, g_hash_table_lookup(headers, lowered), NULL);
        }
    }

    /*
     * Deduplication, before the pending-run cap.
     *
     * A forge that retries a delivery clawtilla already accepted must
     * get the same answer however full the queue is -- otherwise a busy
     * trigger turns a retry into a refusal, the forge retries harder,
     * and the queue stays full because of the retries.
     */
    if (clawt_trigger_store_seen_delivery(
            self->trigger_store, trigger_id,
            clawt_trigger_event_get_delivery_id(event)))
        return finish((Answer){ 200,
                                "already had that one\n" }, out_status);

    if (clawt_trigger_store_recent_count(self->trigger_store, trigger_id,
                                         RATE_WINDOW_SECONDS)
        >= RATE_MAX_DELIVERIES) {
        clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                                   CLAWT_DELIVERY_REFUSED,
                                   "more deliveries than this trigger's "
                                   "rate limit allows", NULL);

        return finish((Answer){ 429, "slow down\n" }, out_status);
    }

    if (!clawt_trigger_accepts_event(trigger,
                                     clawt_trigger_event_get_name(event))) {
        clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                                   CLAWT_DELIVERY_IGNORED,
                                   "the event is not in this trigger's "
                                   "events list", NULL);

        /*
         * 200, not 4xx.  A sender told "error" for a delivery clawtilla
         * deliberately did not want will keep retrying it, and a forge's
         * retry schedule outlives anybody's patience.
         */
        return finish((Answer){ 200, "not wanted\n" },
                      out_status);
    }

    if (!clawt_trigger_accepts_delivery(trigger, event, &reason)) {
        clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                                   CLAWT_DELIVERY_IGNORED, reason, NULL);

        return finish((Answer){ 200, "not wanted\n" },
                      out_status);
    }

    /*
     * The handshake.
     *
     * The first authenticated delivery is captured and shown rather than
     * run, so somebody can read what the caller actually sends before an
     * agent acts on it. A trigger cannot be both enabled and pending --
     * `trigger.add` starts it disabled, and enabling it is the step that
     * says the captured body was what you expected.
     */
    if (clawt_trigger_store_is_pending_verification(self->trigger_store,
                                                    trigger_id)) {
        clawt_trigger_store_capture(self->trigger_store, trigger_id, event,
                                    NULL);
        clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                                   CLAWT_DELIVERY_CAPTURED,
                                   "held for you to look at, because this "
                                   "is the first delivery this trigger has "
                                   "authenticated", NULL);

        clawt_event_bus_emit(self->bus, "trigger.verified", trigger_id);

        return finish((Answer){ 200, "captured\n" }, out_status);
    }

    if (clawt_trigger_store_count_unfinished(self->trigger_store, trigger_id)
        >= MAX_UNFINISHED_RUNS) {
        clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                                   CLAWT_DELIVERY_FAILED,
                                   "this trigger already has as many runs "
                                   "in flight as it is allowed", NULL);

        return finish((Answer){ 429, "too much already running\n" },
                      out_status);
    }

    agent = clawt_trigger_get_string(trigger, "agent");

    if (agent == NULL || *agent == '\0') {
        clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                                   CLAWT_DELIVERY_FAILED,
                                   "no agent is set", NULL);

        return finish((Answer){ 200, "misconfigured\n" },
                      out_status);
    }

    {
        g_autofree gchar *prompt = clawt_trigger_build_prompt(trigger, event);

        task_id = run_trigger(trigger_id, agent, prompt, self, &error);
    }

    if (task_id == NULL) {
        clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                                   CLAWT_DELIVERY_FAILED,
                                   error != NULL ? error->message
                                                 : "it did not start", NULL);

        return finish((Answer){ 200, "could not start\n" },
                      out_status);
    }

    clawt_trigger_store_record(self->trigger_store, trigger_id, event,
                               CLAWT_DELIVERY_RAN, NULL, task_id);

    clawt_event_bus_emit(self->bus, "trigger.fired", trigger_id);

    return finish((Answer){ 200, "accepted\n" }, out_status);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

void
clawt_daemon_triggers_start(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    guint16 port;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    path = g_build_filename(self->state_dir, "triggers.db", NULL);
    self->trigger_store = clawt_trigger_store_new(path, &error);

    /*
     * A store that cannot open is a warning, not a refusal to start.
     * Losing the receipts is bad; losing the fleet because of it is
     * worse, and every other surface still works.
     */
    if (self->trigger_store == NULL) {
        g_warning("triggers: %s", error->message);
        return;
    }

    clawt_trigger_store_prune(self->trigger_store,
                              DELIVERY_RETENTION_SECONDS);

    /*
     * Every configured trigger gets its endpoint now rather than on its
     * first delivery, so `trigger.list` can print an address for one
     * somebody added by editing the file by hand.
     */
    {
        GPtrArray *triggers = clawt_config_get_triggers(self->config);
        guint i;

        for (i = 0; triggers != NULL && i < triggers->len; i++) {
            ClawtTrigger *trigger = g_ptr_array_index(triggers, i);
            g_autoptr(GError) local = NULL;
            g_autofree gchar *endpoint = clawt_trigger_store_endpoint_for(
                self->trigger_store, clawt_trigger_get_id(trigger), TRUE,
                &local);

            if (endpoint == NULL)
                g_warning("triggers: '%s' has no endpoint: %s",
                          clawt_trigger_get_id(trigger),
                          local != NULL ? local->message : "unknown");
        }
    }

    if (!clawt_config_get_boolean(self->config, "daemon.webhook_enabled"))
        return;

    port = (guint16)clawt_config_get_int(self->config, "daemon.webhook_port");

    self->ingress = clawt_webhook_ingress_new(port, MAX_DELIVERY_BYTES);

    clawt_webhook_ingress_set_deliver_func(self->ingress, on_delivery, self);

    if (!clawt_webhook_ingress_start(
            self->ingress,
            clawt_config_get_boolean(self->config, "daemon.tailscale"),
            &error)) {
        /*
         * The receiver failing is a warning rather than a refusal to
         * start, for the same reason the decision store's is: a fleet
         * that will not come up because a port is held is worse than one
         * that comes up unable to take deliveries and says so.
         */
        g_warning("triggers: the receiver could not listen on port %u: %s"
                  " -- no deliveries will arrive", (guint)port,
                  error->message);
        g_clear_object(&self->ingress);
        return;
    }

    /*
     * Announced from what was bound rather than from what was asked
     * for. A convenience address whose bind failed is exactly the
     * interesting case, and naming the request would say it was
     * reachable there.
     */
    {
        GPtrArray *where = clawt_webhook_ingress_get_addresses(self->ingress);
        guint i;

        for (i = 0; where != NULL && i < where->len; i++)
            g_message("webhooks: accepting deliveries at "
                      "http://%s:%u/hooks/... -- `clawtilla trigger list` "
                      "prints each endpoint",
                      (const gchar *)g_ptr_array_index(where, i),
                      (guint)port);
    }
}

void
clawt_daemon_triggers_stop(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->ingress != NULL) {
        clawt_webhook_ingress_stop(self->ingress);
        g_clear_object(&self->ingress);
    }

    g_clear_object(&self->trigger_store);
}

/* ── The client surface ──────────────────────────────────────────── */

/*
 * One trigger, as a client sees it.
 *
 * The fields come from the schema rather than a list here, for the
 * reason the routine and integration ones do: a list in the daemon and a
 * list in the schema drift, and the drift is silent.
 *
 * With one exception that is not a drift risk but a rule:
 * %CLAWT_SCHEMA_SECRET is skipped outright. The generic loop would
 * otherwise read `secret` as a string and put the reference -- and for
 * an env or command backend, effectively the credential's whereabouts --
 * into every listing every client makes.
 */
static void
add_trigger_object(ClawtDaemon *self, JsonBuilder *builder,
                   ClawtTrigger *trigger)
{
    const gchar *id = clawt_trigger_get_id(trigger);
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize k;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, id);

    entries = clawt_config_schema_get(&n_entries);

    for (k = 0; k < n_entries; k++) {
        const gchar *leaf;

        if (!g_str_has_prefix(entries[k].key, "triggers."))
            continue;

        leaf = entries[k].key + strlen("triggers.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        switch (entries[k].type) {
        case CLAWT_SCHEMA_SECRET:
            /*
             * Never. A secret's value must not reach an IPC response,
             * and neither must the reference: `{command: pass show
             * fleet/trigger-x}` names where the credential lives, to
             * every client on the tailnet.
             */
            break;

        case CLAWT_SCHEMA_BOOLEAN:
            json_builder_set_member_name(builder, leaf);
            json_builder_add_boolean_value(
                builder, clawt_trigger_get_boolean(trigger, leaf));
            break;

        case CLAWT_SCHEMA_INT:
            json_builder_set_member_name(builder, leaf);
            json_builder_add_int_value(
                builder, clawt_trigger_get_int(trigger, leaf));
            break;

        case CLAWT_SCHEMA_STRING_LIST: {
            g_auto(GStrv) values = clawt_trigger_get_string_list(trigger,
                                                                 leaf);
            guint v;

            json_builder_set_member_name(builder, leaf);
            json_builder_begin_array(builder);

            for (v = 0; values != NULL && values[v] != NULL; v++)
                json_builder_add_string_value(builder, values[v]);

            json_builder_end_array(builder);
            break;
        }

        default: {
            const gchar *value = clawt_trigger_get_string(trigger, leaf);

            if (value == NULL)
                break;

            json_builder_set_member_name(builder, leaf);
            json_builder_add_string_value(builder, value);
            break;
        }
        }
    }

    /*
     * Whether a secret is configured at all, which is not the secret.
     * A trigger with none authenticates nothing and would refuse every
     * delivery, and "it is silently refusing everything" is exactly the
     * state a listing has to be able to show.
     */
    {
        g_autoptr(ClawtSecretRef) ref = clawt_trigger_get_secret(trigger,
                                                                 "secret");

        json_builder_set_member_name(builder, "has_secret");
        json_builder_add_boolean_value(builder, ref != NULL);
    }

    if (self->trigger_store != NULL) {
        g_autofree gchar *endpoint = clawt_trigger_store_endpoint_for(
            self->trigger_store, id, FALSE, NULL);
        gboolean pending = clawt_trigger_store_is_pending_verification(
            self->trigger_store, id);

        if (endpoint != NULL) {
            json_builder_set_member_name(builder, "endpoint");
            json_builder_add_string_value(builder, endpoint);
        }

        json_builder_set_member_name(builder, "pending_verification");
        json_builder_add_boolean_value(builder, pending);

        json_builder_set_member_name(builder, "unfinished");
        json_builder_add_int_value(
            builder, clawt_trigger_store_count_unfinished(self->trigger_store,
                                                          id));
    }

    /*
     * Where the receiver is listening, so a client can print a whole URL
     * rather than asking somebody to assemble one. Absent when nothing
     * is listening, which is how a client knows to say so.
     */
    if (self->ingress != NULL) {
        GPtrArray *where = clawt_webhook_ingress_get_addresses(self->ingress);
        guint i;

        json_builder_set_member_name(builder, "listening");
        json_builder_begin_array(builder);

        for (i = 0; where != NULL && i < where->len; i++) {
            g_autofree gchar *base = g_strdup_printf(
                "http://%s:%u",
                (const gchar *)g_ptr_array_index(where, i),
                (guint)clawt_webhook_ingress_get_port(self->ingress));

            json_builder_add_string_value(builder, base);
        }

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);
}

/*
 * Applies the posted fields, dispatching on what the schema says each
 * one is.
 *
 * A list written as a scalar is accepted, echoed back, saved, and read
 * as the default -- so `events: push` would silently mean "every event"
 * rather than "only push", which is the widest possible reading of the
 * narrowest possible instruction.
 */
static void
apply_trigger_fields(ClawtTrigger *trigger, JsonObject *payload)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize i;

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const gchar *leaf;

        if (!g_str_has_prefix(entries[i].key, "triggers."))
            continue;

        leaf = entries[i].key + strlen("triggers.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0 ||
            !json_object_has_member(payload, leaf))
            continue;

        switch (entries[i].type) {
        case CLAWT_SCHEMA_SECRET:
            /*
             * There is deliberately no way to post a secret. It is
             * generated by the daemon and shown once; a client that
             * could send one would have it in a JSON frame, a log and
             * possibly a shell history.
             */
            break;

        case CLAWT_SCHEMA_BOOLEAN:
            clawt_trigger_set_boolean(
                trigger, leaf,
                clawt_ipc_payload_boolean(payload, leaf, FALSE));
            break;

        case CLAWT_SCHEMA_INT:
            clawt_trigger_set_int(trigger, leaf,
                                  clawt_ipc_payload_int(payload, leaf, 0));
            break;

        case CLAWT_SCHEMA_STRING_LIST: {
            JsonNode *node = json_object_get_member(payload, leaf);
            g_auto(GStrv) values = NULL;

            if (JSON_NODE_HOLDS_ARRAY(node)) {
                JsonArray *array = json_node_get_array(node);
                guint length = json_array_get_length(array);
                guint v;

                values = g_new0(gchar *, length + 1);

                for (v = 0; v < length; v++)
                    values[v] = g_strdup(
                        json_array_get_string_element(array, v));
            } else {
                /*
                 * A comma-separated string too, because that is what a
                 * command line and an HTML form both produce, and both
                 * reach this handler.
                 */
                const gchar *text = clawt_ipc_payload_string(payload, leaf);

                if (text != NULL && *text != '\0') {
                    guint v;

                    values = g_strsplit(text, ",", -1);

                    for (v = 0; values[v] != NULL; v++)
                        g_strstrip(values[v]);
                }
            }

            clawt_trigger_set_string_list(trigger, leaf,
                                          (const gchar *const *)values);
            break;
        }

        default:
            clawt_trigger_set_string(trigger, leaf,
                                     clawt_ipc_payload_string(payload, leaf));
            break;
        }
    }
}

JsonNode *
clawt_daemon_handle_trigger(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    if (g_strcmp0(kind, "trigger.list") == 0) {
        GPtrArray *triggers = clawt_config_get_triggers(self->config);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "triggers");
        json_builder_begin_array(builder);

        for (i = 0; triggers != NULL && i < triggers->len; i++)
            add_trigger_object(self, builder,
                               g_ptr_array_index(triggers, i));

        json_builder_end_array(builder);

        json_builder_set_member_name(builder, "receiving");
        json_builder_add_boolean_value(builder, self->ingress != NULL);

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "trigger.add") == 0 ||
        g_strcmp0(kind, "trigger.update") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        gboolean adding = g_strcmp0(kind, "trigger.add") == 0;
        ClawtTrigger *trigger;
        g_autofree gchar *secret = NULL;
        g_autofree gchar *endpoint = NULL;

        if (adding) {
            trigger = clawt_config_add_trigger(self->config, id, &error);

            if (trigger == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        } else {
            trigger = (id != NULL)
                ? clawt_config_get_trigger(self->config, id) : NULL;

            if (trigger == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "there is no trigger called "
                                           "that");
        }

        apply_trigger_fields(trigger, payload);

        /*
         * An enabled trigger can never be pending.
         *
         * Switching one on before it has ever been called is refused
         * rather than allowed, because the two states together are a
         * lie: the listing would say "on", and the next delivery would
         * still be captured instead of run. Somebody watching for a run
         * that the client told them to expect would go looking at the
         * forge, the secret and the filters, in that order, before
         * finding out that the trigger had simply not been verified.
         *
         * The remedy is one command and it is named here, because a
         * refusal that does not say what to do next is a refusal
         * somebody works around.
         */
        if (!adding && clawt_trigger_get_boolean(trigger, "enabled") &&
            self->trigger_store != NULL &&
            clawt_trigger_store_is_pending_verification(self->trigger_store,
                                                        id)) {
            clawt_trigger_set_boolean(trigger, "enabled", FALSE);

            return clawt_ipc_error_new(
                request, CLAWT_ERROR_AGENT_STATE,
                "that trigger has not had a delivery yet, so it cannot be "
                "switched on: point the webhook at its endpoint, let one "
                "arrive, read it with `clawtilla trigger capture`, and "
                "then switch it on");
        }

        if (adding) {
            /*
             * A new trigger starts disabled, whatever was posted.
             *
             * The first authenticated delivery is captured and shown
             * rather than run, and an enabled trigger can never be
             * pending -- so honouring `enabled: true` here would run an
             * agent on the first body anybody sent, which is exactly
             * what the handshake exists to prevent.
             */
            clawt_trigger_set_boolean(trigger, "enabled", FALSE);

            secret = mint_secret(self, trigger, &error);

            if (secret == NULL) {
                clawt_config_remove_trigger(self->config, id);
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
            }
        }

        if (!clawt_config_save(self->config, &error)) {
            if (adding)
                clawt_config_remove_trigger(self->config, id);

            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (self->trigger_store != NULL)
            endpoint = clawt_trigger_store_endpoint_for(self->trigger_store,
                                                        id, TRUE, NULL);

        clawt_event_bus_emit(self->bus, "trigger.changed", id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);

        if (endpoint != NULL) {
            json_builder_set_member_name(builder, "endpoint");
            json_builder_add_string_value(builder, endpoint);
        }

        /*
         * The one and only time the secret crosses IPC.
         *
         * It is never stored anywhere a listing can reach and never
         * printed again, so a client that loses it has to rotate. That
         * is the trade: showing it twice means showing it to whoever
         * asks next.
         */
        if (secret != NULL) {
            json_builder_set_member_name(builder, "secret");
            json_builder_add_string_value(builder, secret);
            json_builder_set_member_name(builder, "secret_shown_once");
            json_builder_add_boolean_value(builder, TRUE);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "trigger.remove") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");

        if (id == NULL || !clawt_config_remove_trigger(self->config, id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no trigger called that");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "trigger.changed", id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "trigger.rotate") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        ClawtTrigger *trigger = (id != NULL)
            ? clawt_config_get_trigger(self->config, id) : NULL;
        g_autofree gchar *secret = NULL;
        g_autofree gchar *endpoint = NULL;

        if (trigger == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no trigger called that");

        secret = mint_secret(self, trigger, &error);

        if (secret == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The address changes with the secret, and the trigger goes back
         * to pending. Rotating because a secret leaked and leaving the
         * endpoint in place would mean whoever had it still knows where
         * to knock; and the next delivery is from a registration nobody
         * has yet seen work, which is what the handshake is for.
         */
        if (self->trigger_store != NULL)
            endpoint = clawt_trigger_store_rotate_endpoint(self->trigger_store,
                                                           id, NULL);

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "trigger.changed", id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_set_member_name(builder, "secret");
        json_builder_add_string_value(builder, secret);
        json_builder_set_member_name(builder, "secret_shown_once");
        json_builder_add_boolean_value(builder, TRUE);

        if (endpoint != NULL) {
            json_builder_set_member_name(builder, "endpoint");
            json_builder_add_string_value(builder, endpoint);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "trigger.test") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        ClawtTrigger *trigger = (id != NULL)
            ? clawt_config_get_trigger(self->config, id) : NULL;
        g_autoptr(ClawtTriggerEvent) event = NULL;
        const gchar *agent;
        const gchar *task_id;

        if (trigger == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no trigger called that");

        agent = clawt_trigger_get_string(trigger, "agent");

        if (agent == NULL || *agent == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "that trigger has no agent");

        /*
         * A synthetic delivery, so somebody can see what the agent will
         * actually be asked before a forge sends anything.
         *
         * It goes through nothing that authenticates, because there is
         * no caller to authenticate -- and it is deliberately *not* a
         * loopback HTTP request against the real endpoint, which would
         * need the secret in a process listing to make it.
         */
        event = clawt_trigger_event_new(clawt_trigger_get_provider(trigger),
                                        clawt_ipc_payload_string(payload,
                                                                 "event"),
                                        NULL);

        if (clawt_trigger_event_get_name(event) == NULL)
            clawt_trigger_event_set_identity(event, "push", NULL);

        clawt_trigger_event_set_repo(
            event, clawt_trigger_get_string(trigger, "repo"));
        clawt_trigger_event_set_actor(event, "clawtilla");
        clawt_trigger_event_set_payload(
            event, "{\"note\": \"this is a test delivery from "
                   "clawtilla trigger test\"}");

        {
            const gchar *branch = clawt_trigger_get_string(trigger, "branch");

            if (branch != NULL && *branch != '\0') {
                g_autofree gchar *ref = g_strdup_printf("refs/heads/%s",
                                                        branch);

                clawt_trigger_event_set_ref(event, ref);
            }
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);

        if (clawt_ipc_payload_boolean(payload, "run", FALSE)) {
            g_autofree gchar *prompt =
                clawt_trigger_build_prompt(trigger, event);

            task_id = run_trigger(id, agent, prompt, self, &error);

            if (task_id == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            json_builder_set_member_name(builder, "task");
            json_builder_add_string_value(builder, task_id);
        } else {
            g_autofree gchar *prompt =
                clawt_trigger_build_prompt(trigger, event);

            json_builder_set_member_name(builder, "prompt");
            json_builder_add_string_value(builder, prompt);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "trigger.deliveries") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        g_autoptr(GPtrArray) rows = NULL;
        guint i;

        if (self->trigger_store == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this daemon has no trigger store");

        rows = clawt_trigger_store_list_deliveries(
            self->trigger_store, id,
            (guint)clawt_ipc_payload_int(payload, "limit", 50));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "deliveries");
        json_builder_begin_array(builder);

        for (i = 0; i < rows->len; i++) {
            GHashTable *row = g_ptr_array_index(rows, i);
            static const gchar *columns[] = {
                "trigger", "delivery", "event", "repo", "branch", "actor",
                "outcome", "detail", "task", "at"
            };
            gsize c;

            json_builder_begin_object(builder);

            for (c = 0; c < G_N_ELEMENTS(columns); c++) {
                const gchar *value = g_hash_table_lookup(row, columns[c]);

                if (value == NULL)
                    continue;

                json_builder_set_member_name(builder, columns[c]);
                json_builder_add_string_value(builder, value);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        /*
         * An empty list has four causes and they are not the same
         * thing, so it says which one this is rather than leaving a
         * reader to guess between "the forge never called" and "the
         * receiver is not running".
         */
        if (rows->len == 0) {
            json_builder_set_member_name(builder, "note");
            json_builder_add_string_value(
                builder,
                self->ingress != NULL
                    ? "nothing has been delivered yet"
                    : "the receiver is not running, so nothing can be "
                      "delivered -- set daemon.webhook_enabled");
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "trigger.capture") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *capture = NULL;

        if (self->trigger_store == NULL || id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no trigger called that");

        capture = clawt_trigger_store_get_capture(self->trigger_store, id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);

        if (capture != NULL) {
            json_builder_set_member_name(builder, "payload");
            json_builder_add_string_value(builder, capture);
        }

        json_builder_set_member_name(builder, "pending_verification");
        json_builder_add_boolean_value(
            builder,
            clawt_trigger_store_is_pending_verification(self->trigger_store,
                                                        id));

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
