/*
 * clawt-venture-bridge.h - VENTURE's queue, in the operator's inbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Polling, not events.  VENTURE raises `on_created` and its siblings
 * into its *own* podomation engine, in its own process; there is no
 * change stream out of it and no webhook sender.  So the bridge asks,
 * on a slow timer attached to the daemon's context -- never from an IPC
 * handler, where the client would wait on somebody else's network, and
 * never at daemon start, where every test fixture would reach for a
 * server that is not there.
 *
 * A confirmation already raised is matched on its own id, so the same
 * card polled every minute for an hour is one decision and not sixty.
 * The match is on the decision *store* rather than on a set held here,
 * because a daemon that restarted still must not raise everything again.
 *
 * Several connectors can name one server -- one per agent, each with its
 * own token -- and VENTURE's queue is install-wide: every viewer sees
 * every card.  So sources that share a base URL are one *instance*, its
 * queue is read once per tick, and each card is filed with the source
 * that staged it: the one whose `GET /api/v1/whoami` actor (or username)
 * is the card's `origin.name`.  Its answer goes out with that source's
 * token and no other.
 *
 * The transport is a function the owner installs.  That is not a test
 * seam bolted on: the daemon is the only thing in the tree that holds a
 * #SoupSession on the main context, and a bridge that dialled for
 * itself would be a second answer to "where does clawtilla make an
 * outbound request".  It also means every rule in here -- the mapping,
 * the deduplication, the retry -- is exercised against a fixture with
 * no socket anywhere.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "decision/clawt-decision-store.h"
#include "integration/clawt-venture.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_VENTURE_BRIDGE (clawt_venture_bridge_get_type())

G_DECLARE_FINAL_TYPE(ClawtVentureBridge, clawt_venture_bridge,
                     CLAWT, VENTURE_BRIDGE, GObject)

/**
 * ClawtVentureRequestFunc:
 * @bridge: the bridge that wants it sent
 * @request_id: which request this is, never 0
 * @method: `GET` or `POST`
 * @url: where to send it
 * @token: the bearer token for the source sending it
 * @user_data: data passed to clawt_venture_bridge_set_request_func()
 *
 * Sends one request on the bridge's behalf.
 *
 * It must not block: it is called from a timer on the daemon's own
 * context, and a synchronous HTTP call there stalls every agent's
 * messages for as long as venture takes to answer.  Whenever it
 * finishes, it calls clawt_venture_bridge_complete() with the same
 * @request_id -- once, and from the bridge's context.  It may do so
 * before it returns.
 *
 * The id rather than the URL, because a URL does not say who asked:
 * fifteen connectors on one server send the same `GET` with fifteen
 * tokens, and a response matched by URL belongs to whichever source a
 * hash table happened to list first.
 *
 * @token is live credential material.  It must not be logged, stored,
 * or put anywhere a client can read -- it exists in this signature
 * because an `Authorization` header is the only place venture accepts
 * it, and an argv would be world-readable.
 */
typedef void (*ClawtVentureRequestFunc)(ClawtVentureBridge *bridge,
                                        guint               request_id,
                                        const gchar        *method,
                                        const gchar        *url,
                                        const gchar        *token,
                                        gpointer            user_data);

/**
 * clawt_venture_bridge_new:
 * @decisions: (nullable): where raised decisions are kept
 * @context: (nullable): the context the poll timer attaches to
 *
 * @context is taken here rather than at start, because
 * clawt_venture_bridge_start() may be reached from a source dispatch --
 * and dispatching a source does not push the source's own context, so
 * asking for the thread-default there is asking the wrong loop.
 *
 * A %NULL @decisions is legitimate: a daemon whose decision store
 * failed to open still runs, and the bridge then reports every poll as
 * having nowhere to put its answer rather than crashing on it.
 *
 * Returns: (transfer full): the bridge, not yet polling anything
 */
ClawtVentureBridge *clawt_venture_bridge_new(ClawtDecisionStore *decisions,
                                             GMainContext       *context);

/**
 * clawt_venture_bridge_set_request_func:
 * @self: a #ClawtVentureBridge
 * @func: (scope notified) (nullable): how to send a request
 * @user_data: data for @func
 * @notify: (nullable): called when @func is replaced or the bridge goes
 *
 * With none set the bridge queues work and sends nothing, saying so
 * once -- a bridge that silently dropped every request would look
 * exactly like a venture server with an empty queue.
 */
void clawt_venture_bridge_set_request_func(ClawtVentureBridge      *self,
                                           ClawtVentureRequestFunc  func,
                                           gpointer                 user_data,
                                           GDestroyNotify           notify);

/**
 * clawt_venture_bridge_set_source:
 * @self: a #ClawtVentureBridge
 * @name: the integration instance's name
 * @base_url: the venture instance, already resolved
 * @token: (nullable): its bearer token
 * @agent_id: (nullable): who decisions from it are filed against
 *
 * Adds or replaces one venture connector to poll.
 *
 * Replacing rather than adding a second entry, because the instance
 * name is what the decision ids are keyed by: two sources under one
 * name would raise one another's cards and answer to whichever was
 * asked last.
 *
 * Several sources may share @base_url; they are one instance, polled
 * once, and each is asked `whoami` so the cards on it can be told
 * apart.  A replaced source is asked again, because a new token can be
 * somebody else.
 */
void clawt_venture_bridge_set_source(ClawtVentureBridge *self,
                                     const gchar        *name,
                                     const gchar        *base_url,
                                     const gchar        *token,
                                     const gchar        *agent_id);

/**
 * clawt_venture_bridge_clear_sources:
 * @self: a #ClawtVentureBridge
 *
 * Forgets every server, wiping the tokens as it goes.
 *
 * Called on reload before the sources are rebuilt, so a connector
 * somebody removed stops being polled rather than being polled with a
 * credential that is no longer configured anywhere.
 */
void clawt_venture_bridge_clear_sources(ClawtVentureBridge *self);

/**
 * clawt_venture_bridge_has_source:
 * @self: a #ClawtVentureBridge
 * @name: an integration instance's name
 *
 * Returns: %TRUE if that server is already bound
 */
gboolean clawt_venture_bridge_has_source(ClawtVentureBridge *self,
                                         const gchar        *name);

/**
 * clawt_venture_bridge_source_count:
 * @self: a #ClawtVentureBridge
 *
 * Returns: how many venture servers it would poll
 */
guint clawt_venture_bridge_source_count(ClawtVentureBridge *self);

/**
 * clawt_venture_bridge_start:
 * @self: a #ClawtVentureBridge
 * @poll_seconds: how long between polls
 *
 * Arms the timer, replacing any earlier one.
 *
 * The first poll is one interval away, never immediate: a poll at start
 * would be a network request on the path that brings the daemon up,
 * which is the rule `model.list` already broke once.
 *
 * With no sources the timer is not armed at all, so a fleet that has
 * never heard of venture pays nothing for this.
 */
void clawt_venture_bridge_start(ClawtVentureBridge *self,
                                guint               poll_seconds);

/**
 * clawt_venture_bridge_stop:
 * @self: a #ClawtVentureBridge
 *
 * Disarms the timer.  Queued answers are kept: an answer somebody gave
 * is owed to venture whether or not clawtilla is currently polling it.
 */
void clawt_venture_bridge_stop(ClawtVentureBridge *self);

/**
 * clawt_venture_bridge_poll:
 * @self: a #ClawtVentureBridge
 *
 * Asks every instance for its queue now -- once per server, however
 * many connectors share it -- asks any source not yet identified who it
 * is, and retries any answer that has not landed yet.
 *
 * Public so the timer is not the only way in -- a reload wants a poll
 * without waiting out the interval.
 */
void clawt_venture_bridge_poll(ClawtVentureBridge *self);

/**
 * clawt_venture_bridge_ingest:
 * @self: a #ClawtVentureBridge
 * @source: any source on the instance the body came from
 * @json: the body of `GET /api/v1/confirmations`
 * @length: how many bytes of @json, or -1 if NUL-terminated
 * @error: (out) (optional): return location for a #GError
 *
 * Turns one poll's answer into decisions.
 *
 * @source names the server, not the owner: the queue is install-wide,
 * so every card is routed to the source on that instance whose `whoami`
 * actor (or username) is the card's `origin.name`, and filed against
 * that source's agent under that source's decision id.  A card nobody
 * here staged -- a person's, the operator's own service token's, or one
 * whose origin matches no source or more than one -- is not filed at
 * all; see the comment on owner_for() in the source for why.
 *
 * A source that has not answered `whoami` yet holds up only its own
 * cards: a card whose actor names an identified source is routed at
 * once, since an actor names one token.  A card that matches nobody
 * while any source on the instance is still unidentified is held
 * quietly (it may be that source's), and is called nobody's only once
 * every source has answered.
 *
 * A card whose decision is already in the store is skipped, so polling
 * is idempotent.  A card whose decision was *answered* while venture
 * still lists it as pending is re-queued instead: venture's own queue
 * is the proof that the answer never landed, and re-queueing from it is
 * what makes an answer given while the server was down survive a daemon
 * restart, which a retry list held only in memory would not.
 *
 * Returns: how many decisions were raised, or 0 with @error set
 */
guint clawt_venture_bridge_ingest(ClawtVentureBridge  *self,
                                  const gchar         *source,
                                  const gchar         *json,
                                  gssize               length,
                                  GError             **error);

/**
 * clawt_venture_bridge_answer:
 * @self: a #ClawtVentureBridge
 * @decision_id: the decision that was answered
 * @answer: what the person said
 *
 * Sends an operator's answer back to venture, if that decision was one
 * of ours -- with the token of the source that staged the card, which
 * the last poll of its instance established.  If no poll has yet (the
 * daemon has just started), nothing is sent now: the next poll finds
 * the decision answered while venture still holds the card, and sends
 * it then, with the right token.
 *
 * Queued rather than sent and forgotten: a POST that fails is retried
 * on the next poll, because an answer that vanished would leave the
 * change waiting in venture until its TTL dropped it -- and the
 * operator, having answered, would have no reason to look again.
 *
 * Returns: %TRUE if the decision belonged to a venture connector
 */
gboolean clawt_venture_bridge_answer(ClawtVentureBridge *self,
                                     const gchar        *decision_id,
                                     const gchar        *answer);

/**
 * clawt_venture_bridge_complete:
 * @self: a #ClawtVentureBridge
 * @request_id: the id the #ClawtVentureRequestFunc was given
 * @body: (nullable): what came back, for a `GET`
 * @length: how many bytes of @body, or -1 if NUL-terminated
 * @error: (nullable): why it failed, or %NULL if it did not
 *
 * The transport reporting back.
 *
 * The id finds what was asked and by whom, so responses may arrive in
 * any order.  An id the bridge no longer knows -- completed twice, or
 * never issued -- is ignored.  A source replaced or removed while its
 * request was out is followed by name, and a `whoami` or health answer
 * meant for the token it had before is dropped rather than credited to
 * the new one.
 *
 * A failed poll is dropped -- the next one asks again.  A failed answer
 * stays queued.  The two are different because a poll is a question
 * whose answer changes anyway, and an answer is a thing somebody said
 * once.
 */
void clawt_venture_bridge_complete(ClawtVentureBridge *self,
                                   guint               request_id,
                                   const gchar        *body,
                                   gssize              length,
                                   const GError       *error);

/**
 * clawt_venture_bridge_pending_answers:
 * @self: a #ClawtVentureBridge
 *
 * Returns: how many answers are still owed to a venture server
 */
guint clawt_venture_bridge_pending_answers(ClawtVentureBridge *self);

/**
 * clawt_venture_bridge_pending_answer_url:
 * @self: a #ClawtVentureBridge
 * @n: which one
 *
 * Returns: (transfer none) (nullable): where the @n th queued answer
 *   goes, or %NULL if there is no such one
 */
const gchar *clawt_venture_bridge_pending_answer_url(ClawtVentureBridge *self,
                                                     guint               n);

/**
 * clawt_venture_bridge_get_staging:
 * @self: a #ClawtVentureBridge
 * @name: the source, by connector instance name
 *
 * What the server said, on the first poll, about staging writes.
 *
 * Returns: %CLAWT_VENTURE_STAGING_UNKNOWN until it has answered
 */
ClawtVentureStaging clawt_venture_bridge_get_staging(ClawtVentureBridge *self,
                                                     const gchar        *name);

/**
 * clawt_venture_bridge_get_actor:
 * @self: a #ClawtVentureBridge
 * @name: the source, by connector instance name
 *
 * Who VENTURE says a source's token is, as `GET /api/v1/whoami` put it
 * (e.g. `API token #12`).
 *
 * Returns: (transfer none) (nullable): the actor, or %NULL until whoami
 *   has answered or if it named none
 */
const gchar *clawt_venture_bridge_get_actor(ClawtVentureBridge *self,
                                            const gchar        *name);

G_END_DECLS
