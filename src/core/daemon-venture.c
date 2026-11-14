/*
 * daemon-venture.c - Polling VENTURE, and answering it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything about the VENTURE bridge that needs the daemon: which
 * connectors are bound, where their credentials are, and the one place
 * in the tree that makes an outbound HTTP request on the daemon's own
 * context.
 *
 * In its own file for the same reason daemon-trigger.c is: the whole
 * subsystem is one thing, and clawt_daemon_start() gains two lines
 * rather than a feature.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include "integration/clawt-connector.h"
#include "integration/clawt-oauth.h"
#include "integration/clawt-venture-bridge.h"
#include "trigger/clawt-trigger-provider.h"
#include "trigger/clawt-trigger-store.h"

#include <libsoup/soup.h>

#include <string.h>

#define USER_AGENT "clawtilla/" CLAWT_VERSION_STRING

/*
 * How long a poll may take before it is abandoned.
 *
 * Shorter than the shortest sensible interval, so a venture server that
 * has stopped answering cannot leave requests stacking up faster than
 * they finish.
 */
#define VENTURE_TIMEOUT_SECONDS (20)

/* ── The transport ───────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon *daemon;      /* not owned: it outlives the bridge */
    SoupSession *session;     /* owned */
} VentureTransport;

typedef struct {
    ClawtVentureBridge *bridge;    /* reffed: a reload can replace it */
    SoupMessage        *message;   /* reffed: the status is read from it */
    gchar              *url;
} VentureRequest;

static void
venture_transport_free(gpointer data)
{
    VentureTransport *self = data;

    if (self == NULL)
        return;

    g_clear_object(&self->session);
    g_free(self);
}

static void
venture_request_free(VentureRequest *self)
{
    if (self == NULL)
        return;

    g_clear_object(&self->bridge);
    g_clear_object(&self->message);
    g_free(self->url);
    g_free(self);
}

/*
 * What a status means for an answer that is owed.
 *
 * The bridge keeps anything it is told failed, so this decides between
 * "try again" and "there is nothing left to try" -- and getting that
 * backwards is expensive in both directions.  A 401 retried is a
 * credential somebody can fix; a 409 retried can never succeed, because
 * VENTURE *drops* a confirmation whose record moved underneath it, and
 * the queue would grow a POST that fails identically for ever.
 *
 * Returns: %TRUE when the request is finished with, whatever it said
 */
static gboolean
status_is_settled(const gchar *method, guint status, GError **error)
{
    if (SOUP_STATUS_IS_SUCCESSFUL(status))
        return TRUE;

    /*
     * A failed poll is always just a failed poll: the next tick asks
     * again, and the two readings below are about a card, which a GET
     * does not have.
     */
    if (g_strcmp0(method, "POST") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "VENTURE answered %u", status);
        return FALSE;
    }

    if (status == SOUP_STATUS_NOT_FOUND) {
        g_warning("venture: the change is no longer in its queue -- it was "
                  "answered elsewhere, or it expired before this arrived");
        return TRUE;
    }

    if (status == SOUP_STATUS_CONFLICT) {
        /*
         * The record moved while the card was waiting. VENTURE refuses
         * rather than overwriting, and then drops the confirmation --
         * so approving it again could not succeed either. Said plainly
         * here because the agent will otherwise be waiting on an
         * approval that happened and did nothing.
         */
        g_warning("venture: the record changed while this was waiting, so "
                  "nothing was written and the change was dropped; look at "
                  "the record as it is now and stage it again");
        return TRUE;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                "VENTURE answered %u", status);

    return FALSE;
}

static void
on_venture_answered(GObject *source, GAsyncResult *result, gpointer user_data)
{
    VentureRequest *request = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        clawt_venture_bridge_complete(request->bridge, request->url, NULL, 0,
                                      error);
        venture_request_free(request);
        return;
    }

    /*
     * A body is not a yes.  libsoup hands back the error page for a 401
     * exactly as it hands back the queue for a 200, and treating the
     * two alike would drop an answer that never landed -- and read an
     * authentication failure as an empty queue, which is the one answer
     * that stops anybody looking.
     */
    if (!status_is_settled(soup_message_get_method(request->message),
                           soup_message_get_status(request->message),
                           &error)) {
        clawt_venture_bridge_complete(request->bridge, request->url, NULL, 0,
                                      error);
        venture_request_free(request);
        return;
    }

    {
        gsize length = 0;
        const gchar *data = g_bytes_get_data(body, &length);

        clawt_venture_bridge_complete(request->bridge, request->url, data,
                                      (gssize)length, NULL);
    }

    venture_request_free(request);
}

/*
 * One request, sent without waiting for it.
 *
 * The context is pushed around the call because
 * soup_session_send_and_read_async() takes the *thread-default* one, and
 * this is reached from a timer dispatch -- dispatching a source does not
 * push the source's own context, so without this the reply would be
 * delivered on whichever loop happened to be current.  For an embedded
 * daemon that is a loop nobody runs, and the symptom is a poll that is
 * sent and never lands.
 */
static void
venture_send(ClawtVentureBridge *bridge, const gchar *method,
             const gchar *url, const gchar *token, gpointer user_data)
{
    VentureTransport *transport = user_data;
    g_autoptr(SoupMessage) message = NULL;
    VentureRequest *request;
    GMainContext *context;

    message = soup_message_new(method, url);

    if (message == NULL) {
        g_autoptr(GError) error = NULL;

        g_set_error(&error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a URL clawtilla can dial", url);
        clawt_venture_bridge_complete(bridge, url, NULL, 0, error);
        return;
    }

    /*
     * The credential goes in a header and nowhere else.  Not in the
     * URL, which reaches venture's access log and clawtilla's own
     * event log; not in an argv, which every process on the machine can
     * read.
     */
    if (token != NULL && *token != '\0') {
        g_autofree gchar *value = g_strconcat("Bearer ", token, NULL);

        soup_message_headers_replace(soup_message_get_request_headers(message),
                                     "Authorization", value);
    }

    request = g_new0(VentureRequest, 1);
    request->bridge = g_object_ref(bridge);
    request->message = g_object_ref(message);
    request->url = g_strdup(url);

    context = transport->daemon->main_context;

    if (context != NULL)
        g_main_context_push_thread_default(context);

    soup_session_send_and_read_async(transport->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     on_venture_answered, request);

    if (context != NULL)
        g_main_context_pop_thread_default(context);
}

/* ── Which connectors are bound ──────────────────────────────────── */

/*
 * The token for one connector instance.
 *
 * Read from the 0600 file the path in the config names, never from the
 * config itself -- which is a file people keep in git.
 */
static gchar *
venture_token_for(ClawtIntegrationBinding *binding)
{
    const gchar *token_file =
        clawt_integration_binding_get_string(binding, "token_file");
    g_autoptr(ClawtOauthToken) token = NULL;

    if (token_file == NULL)
        return NULL;

    token = clawt_oauth_token_load(token_file, NULL);

    if (token == NULL || token->access_token == NULL)
        return NULL;

    return g_strdup(token->access_token);
}

void
clawt_daemon_venture_sync(ClawtDaemon *self)
{
    GPtrArray *agents;
    g_autoptr(GPtrArray) catalog = NULL;
    g_autofree gchar *overlay_dir = NULL;
    guint interval = 0;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->venture == NULL)
        return;

    clawt_venture_bridge_stop(self->venture);
    clawt_venture_bridge_clear_sources(self->venture);

    overlay_dir = clawt_config_get_path_value(self->config, "connectors.dir");
    catalog = clawt_connector_catalog_load(overlay_dir, NULL);
    agents = clawt_config_get_agents(self->config);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
        g_autoptr(GPtrArray) bindings =
            clawt_integration_resolve_for_agent(self->config, agent);
        guint b;

        for (b = 0; bindings != NULL && b < bindings->len; b++) {
            ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, b);
            const ClawtConnectorInfo *info;
            const gchar *name;
            g_autofree gchar *base = NULL;
            g_autofree gchar *token = NULL;
            gint64 seconds;

            if (g_strcmp0(clawt_integration_binding_get_info(binding)->id,
                          "connector") != 0)
                continue;

            info = clawt_connector_catalog_find(
                catalog,
                clawt_integration_binding_get_string(binding, "provider"));

            /*
             * Not "is it called venture" but "does it stage its writes
             * and offer a queue" -- which today only venture does, and
             * which an overlay entry could also declare by naming the
             * same tools.  Matched on the id because that is what the
             * URL shapes below belong to; a second such service would
             * be a second module, not a widened match here.
             */
            if (info == NULL || g_strcmp0(info->id, "venture") != 0)
                continue;

            /*
             * `confirm_writes` off means the agent's writes land
             * immediately, so there is no queue to bridge.  Polling
             * anyway would be a request a minute against a server that
             * will always answer with an empty list.
             */
            if (!clawt_integration_binding_get_boolean(binding,
                                                       "confirm_writes"))
                continue;

            name = clawt_integration_binding_get_name(binding);

            /*
             * The first agent that has it wins, in fleet order.  A
             * shared instance covering several agents is already a
             * warning -- venture audits by actor and the connector
             * declares `token_file` as an identity key -- and there is
             * genuinely no way to tell from a card which of them staged
             * it, since they are one actor to venture.  Deterministic
             * beats arbitrary: whichever agent the file lists first.
             */
            if (clawt_venture_bridge_has_source(self->venture, name))
                continue;

            base = clawt_connector_resolve_url(
                info, "",
                clawt_integration_binding_get_string(binding, "instance"));

            if (base == NULL)
                continue;

            token = venture_token_for(binding);

            if (token == NULL) {
                /*
                 * Said rather than skipped silently: a connector nobody
                 * has finished authorising looks, from the inbox, like
                 * a venture server that never has anything staged.
                 */
                g_message("venture: '%s' has no credential yet, so nothing "
                          "staged on it will reach the decision inbox; run "
                          "`clawtilla connector connect %s`", name, name);
                continue;
            }

            clawt_venture_bridge_set_source(
                self->venture, name, base, token,
                clawt_integration_binding_get_agent_id(binding));

            seconds = clawt_integration_binding_get_int(binding,
                                                        "poll_seconds");

            /*
             * One timer, so the shortest interval anybody asked for is
             * the one that runs: a connector set to 30 seconds must not
             * be checked every five minutes because another one is.
             */
            if (seconds > 0 &&
                (interval == 0 || (guint)seconds < interval))
                interval = (guint)seconds;
        }
    }

    if (clawt_venture_bridge_source_count(self->venture) == 0)
        return;

    clawt_venture_bridge_start(self->venture, interval > 0 ? interval : 60);
}

/* ── Business-event replay ───────────────────────────────────────── */

/*
 * How often the feed is read, and how much of it at once.
 *
 * A minute, like the confirmations poll: VENTURE pushes the lane itself
 * with retries, so this exists for the deliveries made while the daemon
 * was down, not for latency.  The baseline read takes the largest page
 * VENTURE allows, because it delivers nothing and only wants the end.
 */
#define REPLAY_INTERVAL_SECONDS (60)
#define REPLAY_PAGE (100)
#define REPLAY_BASELINE_PAGE (1000)

/*
 * Reference counted, because an aborted read still completes: libsoup
 * delivers the cancellation from the loop afterwards, into a callback
 * that must find this alive and see `stopped` rather than find it freed.
 */
struct _ClawtVentureReplay {
    gint         refs;
    gboolean     stopped;
    ClawtDaemon *daemon;       /* not owned: it owns this */
    SoupSession *session;      /* owned */
    GSource     *timer;        /* owned */
    GHashTable  *in_flight;    /* trigger id -> TRUE */
    GHashTable  *last_error;   /* trigger id -> message, said once */
};

static struct _ClawtVentureReplay *
replay_ref(struct _ClawtVentureReplay *self)
{
    self->refs++;

    return self;
}

static void
replay_unref(struct _ClawtVentureReplay *self)
{
    if (self == NULL || --self->refs > 0)
        return;

    g_clear_object(&self->session);
    g_hash_table_unref(self->in_flight);
    g_hash_table_unref(self->last_error);
    g_free(self);
}

typedef struct {
    struct _ClawtVentureReplay *replay;
    SoupMessage *message;
    gchar       *trigger_id;
    gchar       *base;
    gchar       *token;
    gint64       organization_id;
    gboolean     baseline;
    guint        limit;
} ReplayRead;

static void replay_read_page(struct _ClawtVentureReplay *replay,
                             const gchar *trigger_id, const gchar *base,
                             const gchar *token, gint64 organization_id,
                             const gchar *after);

static void
replay_read_free(ReplayRead *self)
{
    if (self == NULL)
        return;

    g_clear_object(&self->message);
    g_free(self->trigger_id);
    g_free(self->base);
    replay_unref(self->replay);

    if (self->token != NULL)
        memset((void *volatile)self->token, 0, strlen(self->token));

    g_free(self->token);
    g_free(self);
}

/*
 * A failure, said once per trigger until it changes.
 *
 * Every minute against a server that is down would otherwise be a
 * warning a minute, and the one line that matters -- the 403 that says
 * the token is not an owner's -- would be lost among them.
 */
static void
replay_say(struct _ClawtVentureReplay *replay, const gchar *trigger_id,
           const gchar *message)
{
    const gchar *previous = g_hash_table_lookup(replay->last_error,
                                                trigger_id);

    if (message == NULL) {
        g_hash_table_remove(replay->last_error, trigger_id);
        return;
    }

    if (g_strcmp0(previous, message) == 0)
        return;

    g_warning("venture: trigger '%s' could not catch up: %s", trigger_id,
              message);
    g_hash_table_replace(replay->last_error, g_strdup(trigger_id),
                         g_strdup(message));
}

gchar *
clawt_daemon_venture_replay_page(ClawtDaemon  *self,
                                 const gchar  *trigger_id,
                                 const gchar  *body,
                                 gssize        length,
                                 gboolean      baseline,
                                 guint        *out_count,
                                 GError      **error)
{
    g_autoptr(GPtrArray) events = NULL;
    g_autofree gchar *next = NULL;
    ClawtTrigger *trigger;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    g_return_val_if_fail(trigger_id != NULL, NULL);

    if (out_count != NULL)
        *out_count = 0;

    events = clawt_venture_business_events_parse(body, length, &next, error);

    if (events == NULL)
        return NULL;

    if (out_count != NULL)
        *out_count = events->len;

    /* A reload can have removed the trigger while the page was in flight. */
    trigger = clawt_config_get_trigger(self->config, trigger_id);

    if (trigger == NULL || self->trigger_store == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "trigger '%s' is gone", trigger_id);
        return NULL;
    }

    if (!baseline) {
        ClawtTriggerHandler *handler =
            clawt_trigger_handler_for(CLAWT_TRIGGER_PROVIDER_VENTURE);
        g_autoptr(GHashTable) headers = clawt_trigger_headers_new();

        for (i = 0; i < events->len; i++) {
            const gchar *text = g_ptr_array_index(events, i);
            g_autoptr(ClawtTriggerEvent) event = NULL;
            const gchar *said = NULL;
            guint code;

            event = clawt_trigger_handler_normalise(
                handler, headers, (const guchar *)text, strlen(text));

            if (event == NULL)
                continue;

            code = clawt_daemon_trigger_deliver(self, trigger, event, &said);

            if (code == 429 || code >= 500) {
                g_autofree gchar *what = g_strdup(said != NULL ? said : "?");

                g_strchomp(what);
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the trigger could not take a replayed event "
                            "(%s); the page will be read again", what);
                return NULL;
            }
        }
    }

    if (!clawt_trigger_store_set_cursor(self->trigger_store, trigger_id,
                                        next, error))
        return NULL;

    return g_steal_pointer(&next);
}

/*
 * One page has been read; deliver it, then move the cursor.
 *
 * The cursor moves only after every event up to it has been handed to
 * clawt_daemon_trigger_deliver(), which deduplicates on the event's
 * stable id -- so a crash between the two reads the page again and runs
 * nothing twice, where saving first would skip what had not run.  A
 * delivery the daemon could not take (503, or 429 from a full queue)
 * stops the page there and leaves the cursor where it was.
 */
static void
on_replay_page(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ReplayRead *read = user_data;
    struct _ClawtVentureReplay *replay = read->replay;
    ClawtDaemon *daemon = replay->daemon;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *next = NULL;
    gboolean full = FALSE;
    guint status;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);
    status = soup_message_get_status(read->message);

    if (replay->stopped) {
        replay_read_free(read);
        return;
    }

    g_hash_table_remove(replay->in_flight, read->trigger_id);

    if (body == NULL) {
        replay_say(replay, read->trigger_id, error->message);
        replay_read_free(read);
        return;
    }

    if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
        g_autofree gchar *message = g_strdup_printf(
            "VENTURE answered %u%s", status,
            (status == 401 || status == 403)
                ? " -- the replay connector's token must belong to an "
                  "owner or administrator of the organization"
                : "");

        replay_say(replay, read->trigger_id, message);
        replay_read_free(read);
        return;
    }

    {
        gsize length = 0;
        const gchar *data = g_bytes_get_data(body, &length);
        guint count = 0;

        next = clawt_daemon_venture_replay_page(daemon, read->trigger_id,
                                                data, (gssize)length,
                                                read->baseline, &count,
                                                &error);

        if (next == NULL) {
            replay_say(replay, read->trigger_id,
                       error != NULL ? error->message : "nothing was read");
            replay_read_free(read);
            return;
        }

        replay_say(replay, read->trigger_id, NULL);
        full = count >= read->limit;
    }

    /*
     * A full page means there may be more, so it asks again at once
     * rather than a minute from now -- otherwise a daemon down for a
     * busy day would take an hour of ticks to catch up a hundred at a
     * time.
     */
    if (full)
        replay_read_page(replay, read->trigger_id, read->base, read->token,
                         read->organization_id, next);

    replay_read_free(read);
}

static void
replay_read_page(struct _ClawtVentureReplay *replay, const gchar *trigger_id,
                 const gchar *base, const gchar *token,
                 gint64 organization_id, const gchar *after)
{
    ReplayRead *read;
    g_autofree gchar *url = NULL;
    g_autofree gchar *value = NULL;
    GMainContext *context = replay->daemon->main_context;

    if (g_hash_table_contains(replay->in_flight, trigger_id))
        return;

    read = g_new0(ReplayRead, 1);
    read->replay = replay_ref(replay);
    read->trigger_id = g_strdup(trigger_id);
    read->base = g_strdup(base);
    read->token = g_strdup(token);
    read->organization_id = organization_id;
    read->baseline = (after == NULL);
    read->limit = read->baseline ? REPLAY_BASELINE_PAGE : REPLAY_PAGE;

    url = clawt_venture_business_events_url(base, organization_id, after,
                                            read->limit);
    read->message = url != NULL ? soup_message_new("GET", url) : NULL;

    if (read->message == NULL) {
        replay_say(replay, trigger_id,
                   "the replay connector's instance is not a URL "
                   "clawtilla can dial");
        replay_read_free(read);
        return;
    }

    /* The credential goes in a header and nowhere else, as for the bridge. */
    value = g_strconcat("Bearer ", token, NULL);
    soup_message_headers_replace(
        soup_message_get_request_headers(read->message), "Authorization",
        value);
    memset((void *volatile)value, 0, strlen(value));

    g_hash_table_add(replay->in_flight, g_strdup(trigger_id));

    /* Pushed for the reason venture_send() pushes it. */
    if (context != NULL)
        g_main_context_push_thread_default(context);

    soup_session_send_and_read_async(replay->session, read->message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     on_replay_page, read);

    if (context != NULL)
        g_main_context_pop_thread_default(context);
}

/*
 * The binding a trigger's `replay_connector` names, on the trigger's
 * own agent -- the one whose credential is entitled to read the feed.
 * Resolved through the same binding machinery as everything else, so a
 * connector the agent is not scoped to is not one it can borrow.
 */
static ClawtIntegrationBinding *
replay_binding_for(ClawtDaemon *self, ClawtTrigger *trigger,
                   const gchar *connector, gchar **out_reason)
{
    const gchar *agent_id = clawt_trigger_get_string(trigger, "agent");
    ClawtAgentConfig *agent;
    g_autoptr(GPtrArray) bindings = NULL;
    guint i;

    agent = agent_id != NULL ? clawt_config_get_agent(self->config, agent_id)
                             : NULL;

    if (agent == NULL) {
        *out_reason = g_strdup("its agent does not exist");
        return NULL;
    }

    bindings = clawt_integration_resolve_for_agent(self->config, agent);

    for (i = 0; bindings != NULL && i < bindings->len; i++) {
        ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

        if (g_strcmp0(clawt_integration_binding_get_name(binding),
                      connector) != 0)
            continue;

        if (g_strcmp0(clawt_integration_binding_get_info(binding)->id,
                      "connector") != 0 ||
            g_strcmp0(clawt_integration_binding_get_string(binding,
                                                           "provider"),
                      "venture") != 0) {
            *out_reason = g_strdup_printf("'%s' is not a venture connector",
                                          connector);
            return NULL;
        }

        return clawt_integration_binding_ref(binding);
    }

    *out_reason = g_strdup_printf("'%s' is not bound to agent '%s'",
                                  connector, agent_id);

    return NULL;
}

void
clawt_daemon_venture_replay_tick(ClawtDaemon *self)
{
    struct _ClawtVentureReplay *replay;
    GPtrArray *triggers;
    g_autoptr(GPtrArray) catalog = NULL;
    g_autofree gchar *overlay_dir = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    replay = self->venture_replay;

    if (replay == NULL || self->trigger_store == NULL)
        return;

    triggers = clawt_config_get_triggers(self->config);

    for (i = 0; triggers != NULL && i < triggers->len; i++) {
        ClawtTrigger *trigger = g_ptr_array_index(triggers, i);
        const gchar *id = clawt_trigger_get_id(trigger);
        const gchar *connector =
            clawt_trigger_get_string(trigger, "replay_connector");
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autofree gchar *reason = NULL;
        g_autofree gchar *base = NULL;
        g_autofree gchar *token = NULL;
        g_autofree gchar *cursor = NULL;
        const ClawtConnectorInfo *info;

        if (connector == NULL || *connector == '\0' ||
            clawt_trigger_get_provider(trigger) !=
                CLAWT_TRIGGER_PROVIDER_VENTURE)
            continue;

        /*
         * A switched-off trigger is not caught up, and neither is one
         * still waiting for its handshake: the first delivery it sees
         * must be one a person looks at, not a page of history.
         */
        if (!clawt_trigger_get_boolean(trigger, "enabled") ||
            clawt_trigger_store_is_pending_verification(self->trigger_store,
                                                        id))
            continue;

        binding = replay_binding_for(self, trigger, connector, &reason);

        if (binding == NULL) {
            replay_say(replay, id, reason);
            continue;
        }

        if (catalog == NULL) {
            overlay_dir = clawt_config_get_path_value(self->config,
                                                      "connectors.dir");
            catalog = clawt_connector_catalog_load(overlay_dir, NULL);
        }

        info = clawt_connector_catalog_find(catalog, "venture");
        base = info != NULL
            ? clawt_connector_resolve_url(
                  info, "",
                  clawt_integration_binding_get_string(binding, "instance"))
            : NULL;
        token = venture_token_for(binding);

        if (base == NULL || token == NULL) {
            g_autofree gchar *message = g_strdup_printf(
                "'%s' has no %s yet", connector,
                base == NULL ? "instance" : "credential");

            replay_say(replay, id, message);
            continue;
        }

        cursor = clawt_trigger_store_get_cursor(self->trigger_store, id);

        replay_read_page(replay, id, base, token,
                         clawt_trigger_get_int(trigger, "organization_id"),
                         cursor);

        memset((void *volatile)token, 0, strlen(token));
    }
}

static gboolean
on_replay_tick(gpointer user_data)
{
    clawt_daemon_venture_replay_tick(user_data);

    return G_SOURCE_CONTINUE;
}

static void
venture_replay_start(ClawtDaemon *self)
{
    struct _ClawtVentureReplay *replay = g_new0(struct _ClawtVentureReplay, 1);

    replay->refs = 1;
    replay->daemon = self;
    replay->session = soup_session_new_with_options(
        "user-agent", USER_AGENT, "timeout", VENTURE_TIMEOUT_SECONDS, NULL);
    replay->in_flight = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    replay->last_error = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);

    /*
     * The first read is one interval away, never during start: no
     * network on the path that brings the daemon up.  Attached to the
     * daemon's context rather than the global default, which an
     * embedded daemon never runs.
     */
    replay->timer = g_timeout_source_new_seconds(REPLAY_INTERVAL_SECONDS);
    g_source_set_callback(replay->timer, on_replay_tick, self, NULL);
    g_source_attach(replay->timer, self->main_context);

    self->venture_replay = replay;
}

static void
venture_replay_stop(ClawtDaemon *self)
{
    struct _ClawtVentureReplay *replay = self->venture_replay;

    if (replay == NULL)
        return;

    if (replay->timer != NULL) {
        g_source_destroy(replay->timer);
        g_source_unref(replay->timer);
    }

    /*
     * Marked before the abort, so a completion delivered afterwards --
     * and libsoup does deliver one -- frees its read and touches
     * nothing else.  The reads hold the remaining references.
     */
    replay->stopped = TRUE;
    replay->timer = NULL;
    soup_session_abort(replay->session);

    self->venture_replay = NULL;
    replay_unref(replay);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
on_venture_decision_raised(ClawtVentureBridge *bridge,
                           const gchar        *decision_id,
                           const gchar        *agent_id,
                           gpointer            user_data)
{
    ClawtDaemon *self = user_data;

    (void)bridge;
    (void)decision_id;

    /*
     * The same event an agent's own `clawtilla_ask` raises, because to
     * every client this *is* the same thing: something is waiting on
     * the person.  A staged write that arrived through a different door
     * must not need a different badge.
     */
    clawt_event_bus_emit(self->bus, "decision.asked", agent_id);
}

void
clawt_daemon_venture_start(ClawtDaemon *self)
{
    VentureTransport *transport;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    self->venture = clawt_venture_bridge_new(self->decisions,
                                             self->main_context);

    transport = g_new0(VentureTransport, 1);
    transport->daemon = self;
    transport->session = soup_session_new_with_options(
        "user-agent", USER_AGENT,
        "timeout", VENTURE_TIMEOUT_SECONDS,
        NULL);

    clawt_venture_bridge_set_request_func(self->venture, venture_send,
                                          transport, venture_transport_free);

    g_signal_connect(self->venture, "decision-raised",
                     G_CALLBACK(on_venture_decision_raised), self);

    /*
     * Sources now, requests later.  clawt_venture_bridge_start() arms a
     * timer whose first tick is one interval away and sends nothing in
     * the meantime -- which is the rule: no network on the path that
     * brings the daemon up, or every test fixture reaches for a server
     * that is not there.
     */
    clawt_daemon_venture_sync(self);

    venture_replay_start(self);
}

void
clawt_daemon_venture_stop(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    venture_replay_stop(self);

    if (self->venture == NULL)
        return;

    clawt_venture_bridge_stop(self->venture);
    g_clear_object(&self->venture);
}

gboolean
clawt_daemon_venture_answer(ClawtDaemon *self, ClawtDecision *decision)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    if (self->venture == NULL || decision == NULL)
        return FALSE;

    return clawt_venture_bridge_answer(self->venture,
                                       clawt_decision_get_id(decision),
                                       clawt_decision_get_answer(decision));
}
