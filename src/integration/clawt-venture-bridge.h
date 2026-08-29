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
 * @method: `GET` or `POST`
 * @url: where to send it
 * @token: the bearer token for that instance
 * @user_data: data passed to clawt_venture_bridge_set_request_func()
 *
 * Sends one request on the bridge's behalf.
 *
 * It must not block: it is called from a timer on the daemon's own
 * context, and a synchronous HTTP call there stalls every agent's
 * messages for as long as venture takes to answer.  Whenever it
 * finishes, it calls clawt_venture_bridge_complete() with the same
 * @url.
 *
 * @token is live credential material.  It must not be logged, stored,
 * or put anywhere a client can read -- it exists in this signature
 * because an `Authorization` header is the only place venture accepts
 * it, and an argv would be world-readable.
 */
typedef void (*ClawtVentureRequestFunc)(ClawtVentureBridge *bridge,
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
 * Adds or replaces one venture server to poll.
 *
 * Replacing rather than adding a second entry, because the instance
 * name is what the decision ids are keyed by: two sources under one
 * name would raise one another's cards and answer to whichever was
 * asked last.
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
 * Asks every source for its queue now, and retries any answer that has
 * not landed yet.
 *
 * Public so the timer is not the only way in -- a reload wants a poll
 * without waiting out the interval.
 */
void clawt_venture_bridge_poll(ClawtVentureBridge *self);

/**
 * clawt_venture_bridge_ingest:
 * @self: a #ClawtVentureBridge
 * @source: the instance the body came from
 * @json: the body of `GET /api/v1/confirmations`
 * @length: how many bytes of @json, or -1 if NUL-terminated
 * @error: (out) (optional): return location for a #GError
 *
 * Turns one poll's answer into decisions.
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
 * of ours.
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
 * @url: the request that finished
 * @body: (nullable): what came back, for a poll
 * @length: how many bytes of @body, or -1 if NUL-terminated
 * @error: (nullable): why it failed, or %NULL if it did not
 *
 * The transport reporting back.
 *
 * A failed poll is dropped -- the next one asks again.  A failed answer
 * stays queued.  The two are different because a poll is a question
 * whose answer changes anyway, and an answer is a thing somebody said
 * once.
 */
void clawt_venture_bridge_complete(ClawtVentureBridge *self,
                                   const gchar        *url,
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

G_END_DECLS
