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

#include <libsoup/soup.h>

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
}

void
clawt_daemon_venture_stop(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

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
