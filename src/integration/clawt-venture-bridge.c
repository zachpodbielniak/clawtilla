/*
 * clawt-venture-bridge.c - VENTURE's queue, in the operator's inbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three things are kept apart here that used to be one:
 *
 *   source    one connector -- a name, a token, an agent.  Many sources
 *             can name the same server, one per agent.
 *   instance  one server, by its queue's URL.  Its queue is install-wide
 *             (every viewer sees every card), so it is read once per
 *             tick whichever source's token reads it.
 *   owner     the source that staged a card: the one whose whoami
 *             token id is the card `origin.token_id`, or, when that
 *             id matches no known source, whose actor (or username)
 *             is the card `origin.name`.  The decision is filed with
 *             its agent and answered with its token.
 *
 * And every request carries an id the transport hands back, so a
 * response is matched to what asked for it rather than to a URL that
 * fifteen sources share.
 */

#include "clawtilla.h"
#include "integration/clawt-venture-bridge.h"

#include <string.h>

/*
 * Where a source stands on "who is this token".
 *
 * Asking is its own state so a tick that comes round while the answer
 * is out does not ask again, and a failure goes back to unknown so the
 * next tick does -- the way a failed health probe is retried.
 */
typedef enum {
    IDENTITY_UNKNOWN = 0,
    IDENTITY_ASKING,
    IDENTITY_KNOWN
} IdentityState;

/*
 * One venture connector, as this bridge knows it.
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

    /*
     * The server this is a connector *to*, as the URL its queue is
     * read from.  Two sources with the same one are the same instance,
     * however their base URLs were spelt -- clawt_venture_confirmations_url()
     * is the one normalisation of a base this tree has.
     */
    gchar *instance;

    /*
     * Which binding of this name this is.  A whoami or health answer
     * that comes back after the source was replaced described the token
     * it had *before*, and must not be credited to the new one: a
     * rotated token can belong to somebody else.
     */
    guint serial;

    /*
     * Whether the server would stage what the agent sends it.  Asked
     * once per source, on the first poll, because a server too old for
     * `?stage=1` *applies* the write and answers as if all was well --
     * so a bridge that only read the queue would show an empty inbox
     * while the books changed underneath it.
     */
    ClawtVentureStaging staging;
    gboolean            probed;

    /* What `GET /api/v1/whoami` said this token is. */
    IdentityState identity;
    gchar        *actor;
    gchar        *username;
    gint64        token_id;      /* 0 when the server sent none */
    gboolean      said_identity;   /* a failed ask has been reported */
} Source;

/*
 * What one instance's last poll established.
 *
 * Rebuilt from scratch on every poll of it, so a card that left the
 * queue leaves these too and neither grows for the life of the daemon.
 */
typedef struct {
    gchar      *key;      /* the confirmations URL */
    GHashTable *owners;   /* card id -> owning source's name */
    GHashTable *held;     /* card id -> NULL: unowned, and already said */
} Instance;

/*
 * An answer somebody gave, still owed to venture.
 *
 * It carries the owning source's name rather than its token so that a
 * source replaced between the answer and its delivery is followed rather
 * than frozen -- a rotated token must not mean an answer that can never
 * be sent.  @request_id is non-zero while a POST is out.
 */
typedef struct {
    gchar *url;
    gchar *source;
    gchar *decision_id;
    guint  request_id;
    guint  attempts;
} Answer;

/* What a request was for, so its response can find its way home. */
typedef enum {
    REQUEST_POLL,
    REQUEST_HEALTH,
    REQUEST_WHOAMI,
    REQUEST_ANSWER
} RequestKind;

/*
 * One request the transport has and has not yet answered.
 *
 * Names and a serial, never a pointer: a reload can replace or drop the
 * source between the send and the answer, and a pointer would be to
 * freed memory by then.
 */
typedef struct {
    RequestKind  kind;
    gchar       *source;     /* who asked: health, whoami */
    guint        serial;     /* which binding of @source asked */
    gchar       *instance;   /* which queue: poll */
} Pending;

struct _ClawtVentureBridge {
    GObject parent_instance;

    ClawtDecisionStore *decisions;
    GMainContext       *context;

    GHashTable *sources;      /* name -> Source*, owned */
    GHashTable *instances;    /* confirmations URL -> Instance*, owned */
    GHashTable *pending;      /* request id -> Pending*, owned */
    GPtrArray  *answers;      /* Answer*, owned */

    guint next_request;
    guint next_serial;

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
    g_free(self->instance);
    g_free(self->actor);
    g_free(self->username);
    g_free(self);
}

static void
instance_free(Instance *self)
{
    if (self == NULL)
        return;

    g_free(self->key);
    g_clear_pointer(&self->owners, g_hash_table_unref);
    g_clear_pointer(&self->held, g_hash_table_unref);
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

static void
pending_free(Pending *self)
{
    if (self == NULL)
        return;

    g_free(self->source);
    g_free(self->instance);
    g_free(self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Pending, pending_free)

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
    g_clear_pointer(&self->instances, g_hash_table_unref);
    g_clear_pointer(&self->pending, g_hash_table_unref);
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
     * @agent_id: who it was filed against -- the agent whose connector
     *   staged the change
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
    self->instances = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                            (GDestroyNotify)instance_free);
    self->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, (GDestroyNotify)pending_free);
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
    source->instance = clawt_venture_confirmations_url(base_url);

    /* 0 is never a serial, so a zeroed Pending matches nothing. */
    if (++self->next_serial == 0)
        self->next_serial = 1;

    source->serial = self->next_serial;

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

static Source *
source_for(ClawtVentureBridge *self, const gchar *name)
{
    if (name == NULL)
        return NULL;

    return g_hash_table_lookup(self->sources, name);
}

const gchar *
clawt_venture_bridge_get_actor(ClawtVentureBridge *self, const gchar *name)
{
    Source *source;

    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), NULL);

    source = source_for(self, name);

    if (source == NULL || source->identity != IDENTITY_KNOWN)
        return NULL;

    return source->actor;
}

static gint
compare_names(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/*
 * Every source's name, sorted.
 *
 * Walked as a copy because a transport may answer synchronously, and
 * sorted because hash order is not an order: which source reads a
 * shared queue, and the order requests go out in, should not change
 * with the table's size.
 */
static GPtrArray *
sorted_source_names(ClawtVentureBridge *self)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter;
    gpointer key;

    g_hash_table_iter_init(&iter, self->sources);

    while (g_hash_table_iter_next(&iter, &key, NULL))
        g_ptr_array_add(names, g_strdup(key));

    g_ptr_array_sort(names, compare_names);

    return names;
}

/*
 * The sources on one instance, in name order.
 *
 * Returns: (transfer container): borrowed #Source pointers, valid until
 *   the sources table next changes
 */
static GPtrArray *
siblings_on(ClawtVentureBridge *self, const gchar *instance)
{
    g_autoptr(GPtrArray) names = sorted_source_names(self);
    GPtrArray *out = g_ptr_array_new();
    guint i;

    for (i = 0; i < names->len; i++) {
        Source *source = source_for(self, g_ptr_array_index(names, i));

        if (source != NULL && source->instance != NULL &&
            g_strcmp0(source->instance, instance) == 0)
            g_ptr_array_add(out, source);
    }

    return out;
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

static Pending *
pending_new(RequestKind kind, Source *source, const gchar *instance)
{
    Pending *pending = g_new0(Pending, 1);

    pending->kind = kind;

    if (source != NULL) {
        pending->source = g_strdup(source->name);
        pending->serial = source->serial;
    }

    pending->instance = g_strdup(instance);

    return pending;
}

/*
 * Hands one request to the transport, under a fresh id.
 *
 * The id is registered, and written to @out_id, *before* the transport
 * is called: a transport may answer synchronously -- a test's does, and
 * a refused URL does -- and that answer must find its request already
 * on file.  Nothing here touches @out_id after the call, because by
 * then the answer may have freed whatever held it.
 *
 * Returns: the id, or 0 if there is no transport and nothing was sent
 */
static guint
send_request(ClawtVentureBridge *self, Pending *pending, const gchar *method,
             const gchar *url, const gchar *token, guint *out_id)
{
    guint id;

    if (out_id != NULL)
        *out_id = 0;

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

        pending_free(pending);
        return 0;
    }

    /* 0 means "not in flight" to an Answer, so it is never an id. */
    if (++self->next_request == 0)
        self->next_request = 1;

    id = self->next_request;

    g_hash_table_insert(self->pending, GUINT_TO_POINTER(id), pending);

    if (out_id != NULL)
        *out_id = id;

    self->request(self, id, method, url, token, self->request_data);

    return id;
}

static Answer *
answer_for_url(ClawtVentureBridge *self, const gchar *url)
{
    guint i;

    for (i = 0; i < self->answers->len; i++) {
        Answer *answer = g_ptr_array_index(self->answers, i);

        if (g_strcmp0(answer->url, url) == 0)
            return answer;
    }

    return NULL;
}

static Answer *
answer_for_request(ClawtVentureBridge *self, guint request_id,
                   guint *out_index)
{
    guint i;

    for (i = 0; i < self->answers->len; i++) {
        Answer *answer = g_ptr_array_index(self->answers, i);

        if (answer->request_id == request_id) {
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

        if (answer->request_id != 0)
            continue;

        /*
         * A source that has gone leaves its answers queued rather than
         * dropping them.  A connector removed for an afternoon and put
         * back should still deliver what somebody decided in between --
         * and venture will still be holding the card, because nothing
         * else here may answer it.
         */
        if (source_for(self, answer->source) == NULL)
            continue;

        g_ptr_array_add(ready, g_strdup(answer->url));
    }

    for (i = 0; i < ready->len; i++) {
        const gchar *url = g_ptr_array_index(ready, i);
        Answer *answer = answer_for_url(self, url);
        Source *source;

        /* It could have been settled by an earlier one's completion. */
        if (answer == NULL || answer->request_id != 0)
            continue;

        source = source_for(self, answer->source);

        if (source == NULL)
            continue;

        answer->attempts++;

        /*
         * The owner's token, and only the owner's.  Any other source on
         * the instance could read this card, and VENTURE might well take
         * the POST from it -- which is exactly how one agent's token
         * came to answer another agent's change.
         */
        send_request(self, pending_new(REQUEST_ANSWER, source, NULL),
                     "POST", url, source->token, &answer->request_id);
    }
}

void
clawt_venture_bridge_poll(ClawtVentureBridge *self)
{
    g_autoptr(GPtrArray) names = NULL;
    g_autoptr(GHashTable) readers = NULL;
    g_autoptr(GHashTable) asked = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));

    /*
     * Answers first.  A poll that raised a decision the operator has
     * already answered would be re-queued by ingest anyway, but sending
     * what is owed before asking for more keeps the queue from growing
     * while venture is unreachable.
     */
    flush_answers(self);

    names = sorted_source_names(self);

    /*
     * Per source: is the server staging, and who is this token.  Both
     * once, both asked again only after a failure -- and both before
     * the queue, so a transport that answers in order has the owners
     * known by the time the cards arrive.  One that does not is why
     * ingest holds a card that matches nobody while anybody here is
     * still unidentified, rather than calling it nobody's.
     */
    for (i = 0; i < names->len; i++) {
        Source *source = source_for(self, g_ptr_array_index(names, i));

        if (source == NULL)
            continue;

        if (!source->probed) {
            g_autofree gchar *health =
                clawt_venture_health_url(source->base_url);

            source->probed = TRUE;

            if (health != NULL &&
                send_request(self, pending_new(REQUEST_HEALTH, source, NULL),
                             "GET", health, source->token, NULL) == 0)
                source->probed = FALSE;
        }

        /* Looked up again: a synchronous answer above may have run. */
        source = source_for(self, g_ptr_array_index(names, i));

        if (source != NULL && source->identity == IDENTITY_UNKNOWN) {
            g_autofree gchar *whoami =
                clawt_venture_whoami_url(source->base_url);

            if (whoami != NULL) {
                source->identity = IDENTITY_ASKING;

                if (send_request(self,
                                 pending_new(REQUEST_WHOAMI, source, NULL),
                                 "GET", whoami, source->token, NULL) == 0) {
                    source = source_for(self, g_ptr_array_index(names, i));

                    if (source != NULL)
                        source->identity = IDENTITY_UNKNOWN;
                }
            }
        }
    }

    /*
     * Per instance: its queue, once.  VENTURE shows every viewer every
     * card, so fifteen connectors on one server asking fifteen times
     * would read the same list fifteen times -- and, matched by URL,
     * file all of it under whichever source answered first.  Who owns
     * each card is decided from the card, not from who asked.
     *
     * Read by the first source by name that has answered whoami, which
     * is a token VENTURE took a moment ago; the first by name at all
     * only while none has (startup).  Always the first by name would be
     * a revoked token reading -- and failing to read -- a queue fourteen
     * live tokens could have read.
     */
    readers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    for (i = 0; i < names->len; i++) {
        const gchar *name = g_ptr_array_index(names, i);
        Source *source = source_for(self, name);
        Source *chosen;

        if (source == NULL || source->instance == NULL)
            continue;

        chosen = source_for(self, g_hash_table_lookup(readers,
                                                      source->instance));

        if (chosen == NULL ||
            (chosen->identity != IDENTITY_KNOWN &&
             source->identity == IDENTITY_KNOWN))
            g_hash_table_replace(readers, g_strdup(source->instance),
                                 g_strdup(name));
    }

    asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; i < names->len; i++) {
        Source *source = source_for(self, g_ptr_array_index(names, i));

        if (source == NULL || source->instance == NULL)
            continue;

        if (g_hash_table_contains(asked, source->instance) ||
            g_strcmp0(g_hash_table_lookup(readers, source->instance),
                      source->name) != 0)
            continue;

        /*
         * Its own copy of the key: the send can answer synchronously,
         * and an ingest that raised a decision can reach a handler that
         * reloads the sources -- after which source->instance is freed.
         */
        g_hash_table_add(asked, g_strdup(source->instance));

        send_request(self, pending_new(REQUEST_POLL, source, source->instance),
                     "GET", source->instance, source->token, NULL);
    }
}

/* ── Health ──────────────────────────────────────────────────────── */

ClawtVentureStaging
clawt_venture_bridge_get_staging(ClawtVentureBridge *self,
                                 const gchar        *name)
{
    Source *source;

    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self),
                         CLAWT_VENTURE_STAGING_UNKNOWN);

    source = source_for(self, name);

    return source != NULL ? source->staging : CLAWT_VENTURE_STAGING_UNKNOWN;
}

/* ── Raising decisions ───────────────────────────────────────────── */

static Instance *
instance_for(ClawtVentureBridge *self, const gchar *key)
{
    Instance *instance = g_hash_table_lookup(self->instances, key);

    if (instance != NULL)
        return instance;

    instance = g_new0(Instance, 1);
    instance->key = g_strdup(key);
    instance->owners = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    instance->held = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, NULL);

    g_hash_table_insert(self->instances, instance->key, instance);

    return instance;
}

/*
 * The one identified source among @siblings that @name is, by actor or
 * by username.
 *
 * Exactly one, or none.  Two sources with the same actor are one token
 * configured twice -- `clawtilla config check` already warns about that
 * -- and picking either would file the card under an agent chosen by
 * the alphabet.  A source that has not answered whoami is nobody yet,
 * and matches nothing.
 */
static Source *
unique_match(GPtrArray *siblings, const gchar *name, gboolean by_actor)
{
    Source *found = NULL;
    guint i;

    for (i = 0; i < siblings->len; i++) {
        Source *source = g_ptr_array_index(siblings, i);
        const gchar *value;

        if (source->identity != IDENTITY_KNOWN)
            continue;

        value = by_actor ? source->actor : source->username;

        if (value == NULL || g_strcmp0(value, name) != 0)
            continue;

        if (found != NULL)
            return NULL;

        found = source;
    }

    return found;
}

/*
 * The one identified source among @siblings whose token id is @token_id.
 *
 * Exactly one, or none, the same rule as unique_match().  A source that
 * has not answered whoami has no id yet and matches nothing.  Two
 * sources reporting the same id are one token configured twice.
 */
static Source *
unique_id_match(GPtrArray *siblings, gint64 token_id)
{
    Source *found = NULL;
    guint i;

    for (i = 0; i < siblings->len; i++) {
        Source *source = g_ptr_array_index(siblings, i);

        if (source->identity != IDENTITY_KNOWN)
            continue;

        if (source->token_id <= 0 || source->token_id != token_id)
            continue;

        if (found != NULL)
            return NULL;

        found = source;
    }

    return found;
}

/*
 * Who staged @card, of the sources on its instance.
 *
 * By token id first, when the card carries one.  The id is stable, and
 * an exact match against a source that has answered whoami is trusted
 * even while other sources on the server have not said who they are:
 * the same rule as an exact actor match.
 *
 * No match by id falls through to the actor string.  That string is
 * the fallback for an older server, which sends no id, and for sources
 * whose whoami predates the server sending token_id: a known source is
 * not asked again, so its token_id stays 0 while new cards carry an
 * id.  The actor is what VENTURE writes into `origin.name` for a token
 * -- `API token #12` -- and it names one token exactly.  An actor
 * match is trusted even while other sources have not said who they
 * are.  The one way two sources share an actor is the same token
 * configured under two connector names, which `clawtilla config check`
 * already warns about; until the second answers whoami, its cards go
 * to the first.
 *
 * By username only after that, only when @all_known -- an unidentified
 * sibling could share the username, and the card could be its -- and
 * never for an origin of kind `user`: a person writing in VENTURE's own
 * interface is named by their username, and an agent whose token was
 * minted under that same person's account would otherwise be handed the
 * person's change.
 *
 * NULL is an answer, not a failure.  A card no connector here staged --
 * a person's, the operator's own service token's, an origin VENTURE left
 * out, one that matches two sources -- is not filed at all, under
 * anybody, and is left to VENTURE's own queue.  Filing it "with the
 * operator" was the alternative, and it is worse: the decision store
 * needs an agent to file it under, and an answer to it would have to go
 * out with *some* token clawtilla holds -- every one of which belongs to
 * an agent that did not stage it, which is the bug this replaces.  A
 * card in the inbox whose answer can never be delivered is an operator
 * believing they decided something.
 */
static Source *
owner_for(GPtrArray *siblings, ClawtVentureConfirmation *card,
          gboolean all_known)
{
    Source *owner;

    if (card->origin_token_id > 0) {
        owner = unique_id_match(siblings, card->origin_token_id);

        if (owner != NULL)
            return owner;
    }

    if (card->origin_name == NULL || *card->origin_name == '\0')
        return NULL;

    owner = unique_match(siblings, card->origin_name, TRUE);

    if (owner != NULL)
        return owner;

    if (!all_known || g_strcmp0(card->origin_kind, "user") == 0)
        return NULL;

    return unique_match(siblings, card->origin_name, FALSE);
}

/*
 * The decision a card already has, if any, and its id.
 *
 * The owner's own id first: that is the only one minted from now on.
 * Then every sibling's -- because before cards were routed, a card was
 * filed under whichever source happened to answer the poll, as
 * `venture-<that source>-<card>`.  A card somebody already answered
 * under such an id must be delivered, not raised a second time as a new
 * question; and of several, an answered one wins, since that is the one
 * carrying something owed.
 */
static ClawtDecision *
existing_decision(ClawtVentureBridge *self, GPtrArray *siblings,
                  Source *owner, const gchar *card_id, gchar **out_id)
{
    g_autofree gchar *own = clawt_venture_decision_id(owner->name, card_id);
    ClawtDecision *found = clawt_decision_store_get(self->decisions, own);
    g_autofree gchar *found_id = NULL;
    guint i;

    if (found != NULL) {
        *out_id = g_steal_pointer(&own);
        return found;
    }

    for (i = 0; i < siblings->len; i++) {
        Source *sibling = g_ptr_array_index(siblings, i);
        g_autofree gchar *legacy = NULL;
        ClawtDecision *candidate;

        if (sibling == owner)
            continue;

        legacy = clawt_venture_decision_id(sibling->name, card_id);
        candidate = clawt_decision_store_get(self->decisions, legacy);

        if (candidate == NULL)
            continue;

        if (found == NULL ||
            (clawt_decision_get_state(found) != CLAWT_DECISION_ANSWERED &&
             clawt_decision_get_state(candidate) == CLAWT_DECISION_ANSWERED)) {
            g_clear_pointer(&found, clawt_decision_free);
            g_free(found_id);
            found = candidate;
            found_id = g_steal_pointer(&legacy);
        } else {
            clawt_decision_free(candidate);
        }
    }

    *out_id = g_steal_pointer(&found_id);

    return found;
}

static void
queue_answer(ClawtVentureBridge *self, Source *owner,
             const gchar *decision_id, const gchar *confirmation_id,
             gboolean approve)
{
    Answer *answer;
    g_autofree gchar *url = clawt_venture_answer_url(owner->base_url,
                                                     confirmation_id,
                                                     approve);

    if (url == NULL)
        return;

    /* Already owed; queueing it twice would post it twice. */
    if (answer_for_url(self, url) != NULL)
        return;

    answer = g_new0(Answer, 1);
    answer->url = g_steal_pointer(&url);
    answer->source = g_strdup(owner->name);
    answer->decision_id = g_strdup(decision_id);

    g_ptr_array_add(self->answers, answer);
}

/*
 * One instance's queue, routed.
 *
 * Everything the poll learns about who owns what is rebuilt here, from
 * this answer alone: the owners are what clawt_venture_bridge_answer()
 * posts with, and a stale one would be a card that left the queue still
 * being answered for.
 */
static guint
ingest_instance(ClawtVentureBridge *self, const gchar *key,
                const gchar *json, gssize length, GError **error)
{
    g_autoptr(GPtrArray) siblings = NULL;
    g_autoptr(GPtrArray) cards = NULL;
    g_autoptr(GHashTable) owners = NULL;
    g_autoptr(GHashTable) held = NULL;
    g_autoptr(GPtrArray) announce = NULL;
    Instance *instance;
    gboolean all_known = TRUE;
    guint raised = 0;
    guint i;

    if (self->decisions == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "this daemon keeps no decisions, so a staged "
                            "VENTURE change has nowhere to be answered");
        return 0;
    }

    siblings = siblings_on(self, key);

    if (siblings->len == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no venture connector is bound to '%s'", key);
        return 0;
    }

    /*
     * Whether every source here has said who it is.  Not a gate: one
     * connector whose token was revoked must not stop every other
     * agent's changes reaching the inbox.  It decides only what a card
     * that matches nobody *means* -- with everybody known it is nobody
     * here's; with somebody still unknown it may be theirs, and is held
     * quietly until they answer.
     */
    for (i = 0; i < siblings->len; i++) {
        Source *source = g_ptr_array_index(siblings, i);

        if (source->identity != IDENTITY_KNOWN) {
            all_known = FALSE;
            break;
        }
    }

    cards = clawt_venture_confirmations_parse(json, length, error);

    if (cards == NULL)
        return 0;

    instance = instance_for(self, key);
    owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    held = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Pairs: decision id, then the agent it was filed against. */
    announce = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < cards->len; i++) {
        ClawtVentureConfirmation *card = g_ptr_array_index(cards, i);
        g_autoptr(ClawtDecision) existing = NULL;
        g_autoptr(ClawtDecision) raised_one = NULL;
        g_autoptr(GError) post_error = NULL;
        g_autofree gchar *existing_id = NULL;
        g_autofree gchar *stored = NULL;
        Source *owner;

        /*
         * venture lists only pending cards today, but it says so in a
         * field rather than in its route's name -- and a queue that
         * grew an `approved` state would otherwise raise a decision
         * about a change that has already been made.
         */
        if (card->state != NULL && g_strcmp0(card->state, "pending") != 0)
            continue;

        owner = owner_for(siblings, card, all_known);

        if (owner == NULL && !all_known) {
            /*
             * Possibly the card of a source that has not answered whoami
             * yet.  Held without a word, and not remembered as unowned:
             * saying "answer it in VENTURE" about a card that will be
             * routed as soon as its owner speaks would send somebody to
             * do by hand what the bridge is about to do.
             */
            g_debug("venture: confirmation %s at %s (from '%s') waits for "
                    "every connector there to say who it is", card->id, key,
                    card->origin_name != NULL ? card->origin_name
                                              : "(nobody named)");
            continue;
        }

        if (owner == NULL) {
            /*
             * Said once per card rather than once per tick: it stays in
             * the queue until somebody answers it in VENTURE or its TTL
             * runs out, and a line a minute about it is noise.
             */
            if (!g_hash_table_contains(instance->held, card->id))
                g_message("venture: confirmation %s at %s was staged by "
                          "'%s', which is no single connector here; it is "
                          "not raised in the inbox and must be answered in "
                          "VENTURE", card->id, key,
                          card->origin_name != NULL ? card->origin_name
                                                    : "(nobody named)");

            g_hash_table_add(held, g_strdup(card->id));
            continue;
        }

        g_hash_table_insert(owners, g_strdup(card->id),
                            g_strdup(owner->name));

        existing = existing_decision(self, siblings, owner, card->id,
                                     &existing_id);

        if (existing != NULL) {
            /*
             * Seen before.  If somebody answered it and venture is
             * still holding the card, the answer never arrived -- so it
             * is queued again from what the store remembers.  That is
             * what makes an answer given while venture was down survive
             * a restart of the daemon: an in-memory retry list would
             * not have, and the operator would have no reason to look
             * at it twice.  It goes with the owner's token whatever id
             * the decision was filed under.
             *
             * A dismissal is left alone on purpose.  "I am not deciding
             * this" is not an answer to post, and venture drops the
             * card on its own clock.
             */
            if (clawt_decision_get_state(existing) == CLAWT_DECISION_ANSWERED)
                queue_answer(self, owner, existing_id, card->id,
                             clawt_venture_answer_is_approval(
                                 clawt_decision_get_answer(existing)));

            continue;
        }

        raised_one = clawt_venture_decision_for(card, owner->name,
                                                owner->agent_id);
        stored = clawt_decision_store_post(self->decisions, raised_one,
                                           &post_error);

        if (stored == NULL) {
            g_warning("venture: could not file a decision for confirmation "
                      "%s: %s", card->id,
                      post_error != NULL ? post_error->message : "unknown");
            continue;
        }

        raised++;

        g_ptr_array_add(announce, g_steal_pointer(&stored));
        g_ptr_array_add(announce, g_strdup(owner->agent_id));
    }

    g_hash_table_unref(instance->owners);
    instance->owners = g_steal_pointer(&owners);
    g_hash_table_unref(instance->held);
    instance->held = g_steal_pointer(&held);

    /*
     * Anything this poll discovered is still owed goes now rather than
     * on the next tick.  A card venture is still holding against a
     * decision somebody already answered is proof the answer never
     * landed, and waiting a whole interval to try again would be a
     * minute of an operator believing they had decided something.
     */
    flush_answers(self);

    /*
     * Announced last, once nothing here still holds a #Source: a
     * handler is free to do anything, a reload included, and the
     * borrowed pointers in @siblings would not survive one.
     */
    for (i = 0; i + 1 < announce->len; i += 2)
        g_signal_emit(self, signals[SIGNAL_DECISION_RAISED], 0,
                      g_ptr_array_index(announce, i),
                      g_ptr_array_index(announce, i + 1));

    return raised;
}

guint
clawt_venture_bridge_ingest(ClawtVentureBridge  *self,
                            const gchar         *source_name,
                            const gchar         *json,
                            gssize               length,
                            GError             **error)
{
    Source *source;
    g_autofree gchar *key = NULL;

    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), 0);
    g_return_val_if_fail(json != NULL, 0);

    source = source_for(self, source_name);

    if (source == NULL || source->instance == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no venture connector named '%s' is bound",
                    source_name != NULL ? source_name : "(none)");
        return 0;
    }

    /* Copied: routing may emit, and a handler may reload the sources. */
    key = g_strdup(source->instance);

    return ingest_instance(self, key, json, length, error);
}

/* ── Answering ───────────────────────────────────────────────────── */

/*
 * The source a decision id was minted from, and the confirmation id.
 *
 * clawt_venture_decision_id() is `venture-<instance>-<confirmation>`,
 * and the instance is a name this bridge already holds -- so the split
 * is done against the sources rather than on the last hyphen, which
 * would be wrong for every instance name containing one.  The longest
 * match wins, so `books` does not claim `books-2`'s decisions.
 */
static Source *
split_decision_id(ClawtVentureBridge *self, const gchar *decision_id,
                  gchar **out_confirmation)
{
    GHashTableIter iter;
    gpointer value;
    Source *best = NULL;
    gsize best_length = 0;

    if (decision_id == NULL)
        return NULL;

    g_hash_table_iter_init(&iter, self->sources);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Source *source = value;
        g_autofree gchar *prefix = clawt_venture_decision_id(source->name, "");
        gsize length = strlen(prefix);

        if (!g_str_has_prefix(decision_id, prefix) || length <= best_length)
            continue;

        best = source;
        best_length = length;
    }

    if (best != NULL && out_confirmation != NULL)
        *out_confirmation = g_strdup(decision_id + best_length);

    return best;
}

gboolean
clawt_venture_bridge_answer(ClawtVentureBridge *self,
                            const gchar        *decision_id,
                            const gchar        *answer)
{
    g_autofree gchar *confirmation = NULL;
    Source *named;
    Source *owner;
    Instance *instance;
    const gchar *owner_name = NULL;

    g_return_val_if_fail(CLAWT_IS_VENTURE_BRIDGE(self), FALSE);

    named = split_decision_id(self, decision_id, &confirmation);

    if (named == NULL || confirmation == NULL || *confirmation == '\0')
        return FALSE;

    /*
     * The id says which server the card is on, not who may answer it:
     * a decision filed before routing carries the name of whichever
     * source read the queue.  Who staged it is what the last poll of
     * the instance established, and that source's token is the only
     * one the answer goes out with.
     */
    instance = named->instance != NULL
        ? g_hash_table_lookup(self->instances, named->instance) : NULL;

    if (instance != NULL)
        owner_name = g_hash_table_lookup(instance->owners, confirmation);

    owner = source_for(self, owner_name);

    if (owner == NULL) {
        /*
         * Ours, but not answerable yet -- or not by anything here.
         * Nothing is lost by waiting: the store has the answer, and the
         * next poll that finds the card still pending queues it with
         * the owner's token, which is the path an answer given while the
         * daemon was down already takes.
         */
        if (instance != NULL &&
            g_hash_table_contains(instance->held, confirmation))
            g_message("venture: decision %s was answered here, but no "
                      "connector here staged that change, so the answer "
                      "cannot be sent; answer it in VENTURE", decision_id);
        else
            g_debug("venture: decision %s is answered; it goes to VENTURE "
                    "once a poll has said who staged it", decision_id);

        return TRUE;
    }

    queue_answer(self, owner, decision_id, confirmation,
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

/* ── Completions ─────────────────────────────────────────────────── */

/*
 * The source a health or whoami answer is about -- the same binding
 * that asked, or nobody.
 */
static Source *
source_that_asked(ClawtVentureBridge *self, Pending *pending)
{
    Source *source = source_for(self, pending->source);

    if (source == NULL || source->serial != pending->serial)
        return NULL;

    return source;
}

static void
complete_answer(ClawtVentureBridge *self, guint request_id,
                const GError *error)
{
    guint index = 0;
    Answer *answer = answer_for_request(self, request_id, &index);

    /* Settled already: it is not owed twice. */
    if (answer == NULL)
        return;

    answer->request_id = 0;

    if (error != NULL) {
        /*
         * Kept, and retried on the next poll.  An answer that vanished
         * would leave the change waiting in venture until its TTL
         * dropped it, and the operator -- having answered -- has no
         * reason to look again.
         */
        g_warning("venture: an answer for decision %s could not be "
                  "delivered (attempt %u): %s; it stays queued",
                  answer->decision_id, answer->attempts, error->message);
        return;
    }

    g_ptr_array_remove_index(self->answers, index);
}

static void
complete_health(ClawtVentureBridge *self, Pending *pending,
                const gchar *body, gssize length, const GError *error)
{
    g_autoptr(GError) parse_error = NULL;
    Source *source = source_that_asked(self, pending);

    if (source == NULL)
        return;

    /*
     * A failed probe is asked again on the next tick rather than
     * remembered as an answer: "could not reach it" must not become "it
     * does not stage".
     */
    if (error != NULL || body == NULL) {
        source->probed = FALSE;
        return;
    }

    source->staging = clawt_venture_health_parse(body, length, &parse_error);

    if (source->staging == CLAWT_VENTURE_STAGING_ABSENT)
        g_warning("venture: '%s' does not stage writes (its health answer "
                  "has no `staged_writes: true`), so what the agent writes "
                  "there lands at once and `confirm_writes` reviews "
                  "nothing; upgrade that VENTURE server", source->name);
    else if (parse_error != NULL)
        g_warning("venture: '%s': %s", source->name, parse_error->message);
}

static void
complete_whoami(ClawtVentureBridge *self, Pending *pending,
                const gchar *body, gssize length, const GError *error)
{
    g_autoptr(GError) parse_error = NULL;
    g_autofree gchar *actor = NULL;
    g_autofree gchar *username = NULL;
    gint64 token_id = 0;
    Source *source = source_that_asked(self, pending);

    /* Replaced since: the new binding asks for itself. */
    if (source == NULL)
        return;

    if (error == NULL && body != NULL &&
        clawt_venture_whoami_parse(body, length, &actor, &username,
                                   &token_id, &parse_error)) {
        g_free(source->actor);
        source->actor = g_steal_pointer(&actor);
        g_free(source->username);
        source->username = g_steal_pointer(&username);
        source->token_id = token_id;
        source->identity = IDENTITY_KNOWN;

        /*
         * Known, and known to be nobody in particular.  The instance is
         * not held up for it -- this source simply owns no card -- but
         * somebody should know why its agent's changes never arrive.
         */
        if (source->actor == NULL && source->username == NULL &&
            source->token_id <= 0)
            g_message("venture: '%s' answered whoami without an actor or "
                      "a username, so no staged change on it can be told "
                      "apart as its own", source->name);

        return;
    }

    /*
     * Asked again next tick.  Said once, because until it answers the
     * changes its own token staged are held -- and an agent whose
     * changes stopped arriving deserves one line saying why.  Every
     * other connector on the server goes on as before.
     */
    source->identity = IDENTITY_UNKNOWN;

    if (!source->said_identity) {
        g_message("venture: could not learn who '%s' is (%s); changes it "
                  "staged are held until it answers, and the other "
                  "connectors on its VENTURE server are not held up",
                  source->name,
                  error != NULL ? error->message
                  : parse_error != NULL ? parse_error->message
                  : "an empty answer");
        source->said_identity = TRUE;
    }
}

static void
complete_poll(ClawtVentureBridge *self, Pending *pending,
              const gchar *body, gssize length, const GError *error)
{
    g_autoptr(GError) ingest_error = NULL;
    g_autoptr(GPtrArray) siblings = NULL;

    if (error != NULL) {
        /*
         * Dropped rather than retried: the next tick asks the same
         * question, and a queue of stale polls would ask it several
         * times at once the moment venture came back.
         */
        g_debug("venture: could not read the queue at %s: %s",
                pending->instance, error->message);
        return;
    }

    if (body == NULL)
        return;

    /*
     * Routed by instance, not by the source that read it: a reload
     * between the send and the answer may have removed that source, and
     * the queue still belongs to everybody else on the server.  Every
     * connector to it gone is a reload that dropped the server, and its
     * last queue is nobody's any more.
     */
    siblings = siblings_on(self, pending->instance);

    if (siblings->len == 0)
        return;

    g_clear_pointer(&siblings, g_ptr_array_unref);

    ingest_instance(self, pending->instance, body, length, &ingest_error);

    if (ingest_error != NULL)
        g_warning("venture: %s: %s", pending->instance,
                  ingest_error->message);
}

void
clawt_venture_bridge_complete(ClawtVentureBridge *self,
                              guint               request_id,
                              const gchar        *body,
                              gssize              length,
                              const GError       *error)
{
    g_autoptr(Pending) pending = NULL;
    gpointer stolen = NULL;

    g_return_if_fail(CLAWT_IS_VENTURE_BRIDGE(self));

    /*
     * Taken off the table before anything runs, so a handler further
     * down that sends a request of its own -- an ingest flushing an
     * answer -- cannot find this one still pending, and a transport
     * that reports twice is told nothing the second time.
     */
    if (!g_hash_table_steal_extended(self->pending,
                                     GUINT_TO_POINTER(request_id), NULL,
                                     &stolen)) {
        g_debug("venture: request %u completed, but nothing is waiting on "
                "it", request_id);
        return;
    }

    pending = stolen;

    switch (pending->kind) {
    case REQUEST_ANSWER:
        complete_answer(self, request_id, error);
        break;
    case REQUEST_HEALTH:
        complete_health(self, pending, body, length, error);
        break;
    case REQUEST_WHOAMI:
        complete_whoami(self, pending, body, length, error);
        break;
    case REQUEST_POLL:
        complete_poll(self, pending, body, length, error);
        break;
    default:
        g_assert_not_reached();
    }
}
