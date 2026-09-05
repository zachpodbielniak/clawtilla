/*
 * clawt-steer-queue.h - Talking to an agent that is already mid-turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Typing a correction at an agent that is working is the ordinary way to
 * steer it, and the obvious implementation is wrong in a way that is
 * hard to see afterwards: **appending the message to the transcript
 * straight away makes the queued line the active leaf**, so the rest of
 * the turn already in flight hangs off a line the model was never shown.
 * The transcript then reads as though the agent answered something
 * nobody had said yet.
 *
 * So a steer is held here instead, out of the transcript, and delivered
 * when the turn settles.
 *
 * Three properties this depends on:
 *
 *   - **Keyed by thread, storing the agent id.**  The settle that frees
 *     an agent can happen on a different thread from the one the steer
 *     was typed into -- a delegation answering in a peer room, for
 *     instance -- so draining has to be answerable from the agent.
 *
 *   - **The entry leaves the queue before anything runs.**  Two settles
 *     arriving together -- the link's own typing frame and the daemon's
 *     interrupt both lower the same flag -- must not each deliver the
 *     same correction.
 *
 *   - **It survives an interrupt.**  Queue a correction, press stop, the
 *     correction runs.  That is the feature rather than a leak: pressing
 *     stop is how somebody says "not that, this", and dropping the
 *     "this" leaves them having only cancelled.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_STEER_QUEUE (clawt_steer_queue_get_type())

G_DECLARE_FINAL_TYPE(ClawtSteerQueue, clawt_steer_queue,
                     CLAWT, STEER_QUEUE, GObject)

/**
 * clawt_steer_queue_new:
 *
 * Returns: (transfer full): a new #ClawtSteerQueue
 */
ClawtSteerQueue *clawt_steer_queue_new(void);

/**
 * clawt_steer_queue_add:
 * @self: a #ClawtSteerQueue
 * @thread_id: the conversation the message was typed into
 * @agent_id: who is meant to read it
 * @text: what was typed
 *
 * Holds one message. Several in the same thread accumulate in order and
 * become one follow-up turn, because two turns for two sentences typed
 * three seconds apart costs twice and answers the first one blind.
 *
 * Returns: how many are now held for @thread_id
 */
guint clawt_steer_queue_add(ClawtSteerQueue *self,
                            const gchar     *thread_id,
                            const gchar     *agent_id,
                            const gchar     *text);

/**
 * clawt_steer_queue_snapshot:
 * @self: a #ClawtSteerQueue
 * @agent_id: the recipient to list
 *
 * Copies each held message, including its room and body, in thread order
 * and send order within each thread. Reading does not drain the queue.
 *
 * Returns: (transfer full): an array of queued message objects
 */
JsonArray *clawt_steer_queue_snapshot(ClawtSteerQueue *self,
                                     const gchar *agent_id);

/**
 * clawt_steer_queue_drain:
 * @self: a #ClawtSteerQueue
 * @agent_id: the agent whose turn has settled
 * @thread_id_out: (out) (optional) (transfer full): where the messages were
 *   typed, so the follow-up lands in the same conversation
 *
 * Takes the oldest held thread for @agent_id out of the queue and
 * returns its messages joined by newlines. Call it again for anything
 * else that is held; each call is one follow-up turn.
 *
 * The entry is removed **before** this returns, so two settles arriving
 * together cannot both deliver it.
 *
 * Returns: (transfer full) (nullable): the joined text, or %NULL when
 *   nothing is held for @agent_id
 */
gchar *clawt_steer_queue_drain(ClawtSteerQueue  *self,
                               const gchar      *agent_id,
                               gchar           **thread_id_out);

/**
 * clawt_steer_queue_pending:
 * @self: a #ClawtSteerQueue
 * @agent_id: (nullable): an agent, or %NULL for every agent
 *
 * Returns: how many messages are held
 */
guint clawt_steer_queue_pending(ClawtSteerQueue *self, const gchar *agent_id);

/**
 * clawt_steer_queue_pending_in_thread:
 * @self: a #ClawtSteerQueue
 * @thread_id: a conversation
 *
 * Returns: how many messages are held for @thread_id
 */
guint clawt_steer_queue_pending_in_thread(ClawtSteerQueue *self,
                                          const gchar     *thread_id);

/**
 * clawt_steer_queue_forget_agent:
 * @self: a #ClawtSteerQueue
 * @agent_id: an agent that is going away
 *
 * Drops everything held for an agent that has been removed. Not called
 * on an interrupt -- surviving one is the point.
 */
void clawt_steer_queue_forget_agent(ClawtSteerQueue *self,
                                    const gchar     *agent_id);

/**
 * clawt_steer_queue_reset:
 * @self: a #ClawtSteerQueue
 *
 * Drops everything. For tests, and for a daemon shutting down.
 */
void clawt_steer_queue_reset(ClawtSteerQueue *self);

G_END_DECLS
