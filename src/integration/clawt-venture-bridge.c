/*
 * clawt-venture-bridge.c - VENTURE's queue, in the operator's inbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-venture-bridge.h"

#include <string.h>

/*
 * One venture server, as this bridge knows it.
 *
 * The token is held because an `Authorization` header is the only place
 * venture takes one; it is wiped when the source is replaced or
 * dropped, so a connector somebody removed does not leave a live
 * credential in the daemon's heap for the rest of the day.
 */
typedef struct {
    gchar *name;
    gchar *base_url;
    gchar *token;
    gchar *agent_id;
} Source;

/*
 * An answer somebody gave, still owed to venture.
 *
 * It carries the source name rather than the token so that a source
 * replaced between the answer and its delivery is followed rather than
 * frozen -- a rotated token must not mean an answer that can never be
 * sent.
 */
typedef struct {
    gchar    *url;
    gchar    *source;
    gchar    *decision_id;
    gboolean  in_flight;
    guint     attempts;
} Answer;

struct _ClawtVentureBridge {
    GObject parent_instance;

    ClawtDecisionStore *decisions;
    GMainContext       *context;

    GHashTable *sources;      /* name -> Source*, owned */
    GPtrArray  *answers;      /* Answer*, owned */

    GSource *timer;

    ClawtVentureRequestFunc  request;
    gpointer                 request_data;
    GDestroyNotify           request_notify;
    gboolean                 warned_no_transport;
};

G_DEFINE_FINAL_TYPE(ClawtVentureBridge, clawt_venture_bridge, G_TYPE_OBJECT)

enum {
    SIGNAL_DECISION_RAISED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ── Small owned things ──────────────────────────────────────────── */

static void
wipe(gchar *secret)
{
    if (secret == NULL)
        return;

    memset((void *volatile)secret, 0, strlen(secret));
}

static void
source_free(Source *self)
{
    if (self == NULL)
        return;

    wipe(self->token);

    g_free(self->name);
    g_free(self->base_url);
    g_free(self->token);
    g_free(self->agent_id);
    g_free(self);
}

static void
answer_free(Answer *self)
{
    if (self == NULL)
        return;

    g_free(self->url);
    g_free(self->source);
    g_free(self->decision_id);
    g_free(self);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
clawt_venture_bridge_dispose(GObject *object)
{
    ClawtVentureBridge *self = CLAWT_VENTURE_BRIDGE(object);

    clawt_venture_bridge_stop(self);

    if (self->request_notify != NULL) {
        self->request_notify(self->request_data);
        self->request_notify = NULL;
    }

    self->request = NULL;
    self->request_data = NULL;

    g_clear_pointer(&self->sources, g_hash_table_unref);
    g_clear_pointer(&self->answers, g_ptr_array_unref);
    g_clear_object(&self->decisions);
    g_clear_pointer(&self->context, g_main_context_unref);

    G_OBJECT_CLASS(clawt_venture_bridge_parent_class)->dispose(object);
}

static void
clawt_venture_bridge_class_init(ClawtVentureBridgeClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_venture_bridge_dispose;

    /**
     * ClawtVentureBridge::decision-raised:
     * @self: the bridge
     * @decision_id: the id it was stored under
     * @agent_id: who it was filed against
     *
     * A staged change has become a question in the inbox.
     *
     * Emitted rather than left to the caller counting the return of
     * clawt_venture_bridge_ingest(), because a client's inbox only
     * refreshes on an event and a count cannot say which agent's badge
     * to move.
     */
    signals[SIGNAL_DECISION_RAISED] = g_signal_new(
        "decision-raised", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void
clawt_venture_bridge_init(ClawtVentureBridge *self)
{
    self->sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)source_free);
    self->answers = g_ptr_array_new_with_free_func(
        (GDestroyNotify)answer_free);
}

ClawtVentureBridge *
clawt_venture_bridge_new(ClawtDecisionStore *decisions,
                         GMainContext       *context)
{
    ClawtVentureBridge *self = g_object_new(CLAWT_TYPE_VENTURE_BRIDGE, NULL);

    if (decisions != NULL)
        self->decisions = g_object_ref(decisions);

    /*
     * Captured here rather than at start, because start may be reached
     * from a source dispatch -- and dispatching a source does not push
     * the source's own context, so the thread-default there is whatever
     * loop happened to be running.
     */
    if (context != NULL)
        self->context = g_main_context_ref(context);

    return self;
}

void
clawt_venture_bridge_set_request_func(ClawtVentureBridge      *self,
                                      ClawtVentureRequestFunc  func,
                                      gpointer                 user_data,
                                      GDestroyNotify           notify)
{
    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));

    if (self->request_notify != NULL)
        self->request_notify(self->request_data);

    self->request = func;
    self->request_data = user_data;
    self->request_notify = notify;
    self->warned_no_transport = FALSE;
}

/* ── Sources ─────────────────────────────────────────────────────── */

void
clawt_venture_bridge_set_source(ClawtVentureBridge *self,
                                const gchar        *name,
                                const gchar        *base_url,
                                const gchar        *token,
                                const gchar        *agent_id)
{
    Source *source;

    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));
    g_return_if_fail(name != NULL);
    g_return_if_fail(base_url != NULL);

    source = g_new0(Source, 1);
    source->name = g_strdup(name);
    source->base_url = g_strdup(base_url);
    source->token = g_strdup(token);
    source->agent_id = g_strdup(agent_id);

    g_hash_table_insert(self->sources, g_strdup(name), source);
}

void
clawt_venture_bridge_clear_sources(ClawtVentureBridge *self)
{
    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));

    g_hash_table_remove_all(self->sources);
}

gboolean
clawt_venture_bridge_has_source(ClawtVentureBridge *self, const gchar *name)
{
    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), FALSE);

    if (name == NULL)
        return FALSE;

    return g_hash_table_contains(self->sources, name);
}

guint
clawt_venture_bridge_source_count(ClawtVentureBridge *self)
{
    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), 0);

    return g_hash_table_size(self->sources);
}

/* ── The timer ───────────────────────────────────────────────────── */

static gboolean
on_tick(gpointer user_data)
{
    clawt_venture_bridge_poll(user_data);

    return G_SOURCE_CONTINUE;
}

void
clawt_venture_bridge_stop(ClawtVentureBridge *self)
{
    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));

    if (self->timer == NULL)
        return;

    g_source_destroy(self->timer);
    g_clear_pointer(&self->timer, g_source_unref);
}

void
clawt_venture_bridge_start(ClawtVentureBridge *self, guint poll_seconds)
{
    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));

    clawt_venture_bridge_stop(self);

    /*
     * Nothing to poll is not an error and must not be a timer: a fleet
     * with no venture connector should have no periodic work for one.
     */
    if (g_hash_table_size(self->sources) == 0)
        return;

    if (poll_seconds == 0)
        return;

    /*
     * Attached to the context this bridge was built with, not to
     * whichever loop is running now.  g_timeout_add_seconds() takes the
     * global default, which for an embedded daemon is a loop nobody
     * runs -- a timer that never fires looks exactly like a venture
     * server with nothing in its queue.
     */
    self->timer = g_timeout_source_new_seconds(poll_seconds);
    g_source_set_callback(self->timer, on_tick, self, NULL);
    g_source_attach(self->timer, self->context);
}

/* ── Sending ─────────────────────────────────────────────────────── */

static void
send_request(ClawtVentureBridge *self, const gchar *method, const gchar *url,
             const gchar *token)
{
    if (self->request == NULL) {
        /*
         * Said once, and said at all: a bridge that quietly dropped
         * every request would be indistinguishable from a venture
         * server whose queue is empty, and somebody would go looking in
         * venture for a card clawtilla never asked about.
         */
        if (!self->warned_no_transport) {
            g_warning("venture: nothing is wired up to send requests, so "
                      "no confirmation will be read and no answer will be "
                      "delivered");
            self->warned_no_transport = TRUE;
        }

        return;
    }

    self->request(self, method, url, token, self->request_data);
}

static Source *
source_for(ClawtVentureBridge *self, const gchar *name)
{
    if (name == NULL)
        return NULL;

    return g_hash_table_lookup(self->sources, name);
}

static Answer *
answer_for_url(ClawtVentureBridge *self, const gchar *url, guint *out_index)
{
    guint i;

    for (i = 0; i < self->answers->len; i++) {
        Answer *answer = g_ptr_array_index(self->answers, i);

        if (g_strcmp0(answer->url, url) == 0) {
            if (out_index != NULL)
                *out_index = i;

            return answer;
        }
    }

    return NULL;
}

/*
 * Sent from a snapshot, never while walking the queue.
 *
 * A transport is allowed to answer synchronously -- a test's does, and
 * a cached failure would -- which reaches clawt_venture_bridge_complete()
 * and removes the entry that was just sent.  Removing from a #GPtrArray
 * shifts everything after it, so a loop indexing the live array would
 * skip the next answer every time one succeeded: the queue would
 * deliver half of itself and look, from the outside, like an answer
 * that was simply lost.
 */
static void
flush_answers(ClawtVentureBridge *self)
{
    g_autoptr(GPtrArray) ready = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; i < self->answers->len; i++) {
        Answer *answer = g_ptr_array_index(self->answers, i);

        if (answer->in_flight)
            continue;

        /*
         * A source that has gone leaves its answers queued rather than
         * dropping them.  A connector removed for an afternoon and put
         * back should still deliver what somebody decided in between --
         * and venture will still be holding the card, because nothing
         * else can answer it.
         */
        if (source_for(self, answer->source) == NULL)
            continue;

        answer->in_flight = TRUE;
        answer->attempts++;

        g_ptr_array_add(ready, g_strdup(answer->url));
    }

    for (i = 0; i < ready->len; i++) {
        const gchar *url = g_ptr_array_index(ready, i);
        Answer *answer = answer_for_url(self, url, NULL);
        Source *source;

        /* It could have been settled by an earlier one's completion. */
        if (answer == NULL)
            continue;

        source = source_for(self, answer->source);

        if (source == NULL)
            continue;

        send_request(self, "POST", url, source->token);
    }
}

void
clawt_venture_bridge_poll(ClawtVentureBridge *self)
{
    GHashTableIter iter;
    gpointer value;

    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));

    /*
     * Answers first.  A poll that raised a decision the operator has
     * already answered would be re-queued by ingest anyway, but sending
     * what is owed before asking for more keeps the queue from growing
     * while venture is unreachable.
     */
    flush_answers(self);

    g_hash_table_iter_init(&iter, self->sources);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Source *source = value;
        g_autofree gchar *url =
            clawt_venture_confirmations_url(source->base_url);

        if (url == NULL)
            continue;

        send_request(self, "GET", url, source->token);
    }
}

/* ── Raising decisions ───────────────────────────────────────────── */

/*
 * Which source a URL belongs to, for a poll answering back.
 *
 * Matched on the confirmations URL rather than remembered per request,
 * because the transport is asynchronous and several sources may be in
 * flight at once -- a single "the poll I sent" pointer breaks the
 * moment there are two venture servers, which is exactly the shape a
 * shared #SoupSession has already been caught in.
 */
static Source *
source_for_poll_url(ClawtVentureBridge *self, const gchar *url)
{
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, self->sources);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Source *source = value;
        g_autofree gchar *candidate =
            clawt_venture_confirmations_url(source->base_url);

        if (g_strcmp0(candidate, url) == 0)
            return source;
    }

    return NULL;
}

static void
queue_answer(ClawtVentureBridge *self, Source *source,
             const gchar *decision_id, const gchar *confirmation_id,
             gboolean approve)
{
    Answer *answer;
    g_autofree gchar *url = clawt_venture_answer_url(source->base_url,
                                                     confirmation_id,
                                                     approve);

    if (url == NULL)
        return;

    /* Already owed; queueing it twice would post it twice. */
    if (answer_for_url(self, url, NULL) != NULL)
        return;

    answer = g_new0(Answer, 1);
    answer->url = g_steal_pointer(&url);
    answer->source = g_strdup(source->name);
    answer->decision_id = g_strdup(decision_id);

    g_ptr_array_add(self->answers, answer);
}

guint
clawt_venture_bridge_ingest(ClawtVentureBridge  *self,
                            const gchar         *source_name,
                            const gchar         *json,
                            gssize               length,
                            GError             **error)
{
    g_autoptr(GPtrArray) cards = NULL;
    Source *source;
    guint raised = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), 0);
    g_return_val_if_fail(json != NULL, 0);

    source = source_for(self, source_name);

    if (source == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no venture connector named '%s' is bound",
                    source_name != NULL ? source_name : "(none)");
        return 0;
    }

    if (self->decisions == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "this daemon keeps no decisions, so a staged "
                            "VENTURE change has nowhere to be answered");
        return 0;
    }

    cards = clawt_venture_confirmations_parse(json, length, error);

    if (cards == NULL)
        return 0;

    for (i = 0; i < cards->len; i++) {
        ClawtVentureConfirmation *card = g_ptr_array_index(cards, i);
        g_autoptr(ClawtDecision) existing = NULL;
        g_autoptr(ClawtDecision) raised_one = NULL;
        g_autoptr(GError) post_error = NULL;
        g_autofree gchar *id = NULL;
        g_autofree gchar *stored = NULL;

        /*
         * venture lists only pending cards today, but it says so in a
         * field rather than in its route's name -- and a queue that
         * grew an `approved` state would otherwise raise a decision
         * about a change that has already been made.
         */
        if (card->state != NULL && g_strcmp0(card->state, "pending") != 0)
            continue;

        id = clawt_venture_decision_id(source->name, card->id);
        existing = clawt_decision_store_get(self->decisions, id);

        if (existing != NULL) {
            /*
             * Seen before.  If somebody answered it and venture is
             * still holding the card, the answer never arrived -- so it
             * is queued again from what the store remembers.  That is
             * what makes an answer given while venture was down survive
             * a restart of the daemon: an in-memory retry list would
             * not have, and the operator would have no reason to look
             * at it twice.
             *
             * A dismissal is left alone on purpose.  "I am not deciding
             * this" is not an answer to post, and venture drops the
             * card on its own clock.
             */
            if (clawt_decision_get_state(existing) == CLAWT_DECISION_ANSWERED)
                queue_answer(self, source, id, card->id,
                             clawt_venture_answer_is_approval(
                                 clawt_decision_get_answer(existing)));

            continue;
        }

        raised_one = clawt_venture_decision_for(card, source->name,
                                                source->agent_id);
        stored = clawt_decision_store_post(self->decisions, raised_one,
                                           &post_error);

        if (stored == NULL) {
            g_warning("venture: could not file a decision for confirmation "
                      "%s: %s", card->id,
                      post_error != NULL ? post_error->message : "unknown");
            continue;
        }

        raised++;

        g_signal_emit(self, signals[SIGNAL_DECISION_RAISED], 0, stored,
                      source->agent_id);
    }

    /*
     * Anything this poll discovered is still owed goes now rather than
     * on the next tick.  A card venture is still holding against a
     * decision somebody already answered is proof the answer never
     * landed, and waiting a whole interval to try again would be a
     * minute of an operator believing they had decided something.
     */
    flush_answers(self);

    return raised;
}

/* ── Answering ───────────────────────────────────────────────────── */

/*
 * The confirmation id back out of a decision id.
 *
 * clawt_venture_decision_id() is `venture-<instance>-<confirmation>`,
 * and the instance is a name this bridge already holds -- so the split
 * is done against the sources rather than on the last hyphen, which
 * would be wrong for every instance name containing one.
 */
static Source *
split_decision_id(ClawtVentureBridge *self, const gchar *decision_id,
                  gchar **out_confirmation)
{
    GHashTableIter iter;
    gpointer value;

    if (decision_id == NULL)
        return NULL;

    g_hash_table_iter_init(&iter, self->sources);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Source *source = value;
        g_autofree gchar *prefix = clawt_venture_decision_id(source->name, "");

        if (!g_str_has_prefix(decision_id, prefix))
            continue;

        if (out_confirmation != NULL)
            *out_confirmation = g_strdup(decision_id + strlen(prefix));

        return source;
    }

    return NULL;
}

gboolean
clawt_venture_bridge_answer(ClawtVentureBridge *self,
                            const gchar        *decision_id,
                            const gchar        *answer)
{
    g_autofree gchar *confirmation = NULL;
    Source *source;

    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), FALSE);

    source = split_decision_id(self, decision_id, &confirmation);

    if (source == NULL || confirmation == NULL || *confirmation == '\0')
        return FALSE;

    queue_answer(self, source, decision_id, confirmation,
                 clawt_venture_answer_is_approval(answer));

    flush_answers(self);

    return TRUE;
}

guint
clawt_venture_bridge_pending_answers(ClawtVentureBridge *self)
{
    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), 0);

    return self->answers->len;
}

const gchar *
clawt_venture_bridge_pending_answer_url(ClawtVentureBridge *self, guint n)
{
    Answer *answer;

    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), NULL);

    if (n >= self->answers->len)
        return NULL;

    answer = g_ptr_array_index(self->answers, n);

    return answer->url;
}

void
clawt_venture_bridge_complete(ClawtVentureBridge *self,
                              const gchar        *url,
                              const gchar        *body,
                              gssize              length,
                              const GError       *error)
{
    Answer *answer;
    Source *source;
    guint index = 0;

    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));
    g_return_if_fail(url != NULL);

    answer = answer_for_url(self, url, &index);

    if (answer != NULL) {
        answer->in_flight = FALSE;

        if (error != NULL) {
            /*
             * Kept, and retried on the next poll.  An answer that
             * vanished would leave the change waiting in venture until
             * its TTL dropped it, and the operator -- having answered
             * -- has no reason to look again.
             */
            g_warning("venture: an answer for decision %s could not be "
                      "delivered (attempt %u): %s; it stays queued",
                      answer->decision_id, answer->attempts, error->message);
            return;
        }

        g_ptr_array_remove_index(self->answers, index);
        return;
    }

    source = source_for_poll_url(self, url);

    if (source == NULL)
        return;

    if (error != NULL) {
        /*
         * Dropped rather than retried: the next tick asks the same
         * question, and a queue of stale polls would ask it several
         * times at once the moment venture came back.
         */
        g_debug("venture: could not read '%s' queue: %s", source->name,
                error->message);
        return;
    }

    if (body != NULL) {
        g_autoptr(GError) ingest_error = NULL;

        clawt_venture_bridge_ingest(self, source->name, body, length,
                                    &ingest_error);

        if (ingest_error != NULL)
            g_warning("venture: '%s': %s", source->name,
                      ingest_error->message);
    }
}
