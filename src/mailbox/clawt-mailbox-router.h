/*
 * clawt-mailbox-router.h - Getting a message to the right mailboxes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "agent/clawt-agent-manager.h"
#include "chat/clawt-loop-guard.h"
#include "chat/clawt-message.h"
#include "chat/clawt-room-manager.h"
#include "core/clawt-event-bus.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MAILBOX_ROUTER (clawt_mailbox_router_get_type())

G_DECLARE_FINAL_TYPE(ClawtMailboxRouter, clawt_mailbox_router, CLAWT,
                     MAILBOX_ROUTER, GObject)

/**
 * clawt_mailbox_router_new:
 * @agents: (transfer none): the fleet
 * @rooms: (transfer none): the rooms
 * @guard: (transfer none) (nullable): loop safety, or %NULL for none
 *
 * Returns: (transfer full): a new #ClawtMailboxRouter
 */
ClawtMailboxRouter *clawt_mailbox_router_new(ClawtAgentManager *agents,
                                             ClawtRoomManager  *rooms,
                                             ClawtLoopGuard    *guard);

/**
 * clawt_mailbox_router_set_event_bus:
 * @self: a #ClawtMailboxRouter
 * @bus: (transfer none) (nullable): where to publish delivery events
 */
void clawt_mailbox_router_set_event_bus(ClawtMailboxRouter *self,
                                        ClawtEventBus      *bus);

/**
 * clawt_mailbox_router_send:
 * @self: a #ClawtMailboxRouter
 * @message: (transfer none): the message; its room id names the destination
 * @error: (out) (optional): return location for a #GError
 *
 * Enqueues @message into every recipient's mailbox.
 *
 * The destination may be a room id or an agent id; an agent id is treated
 * as the direct room between sender and recipient, so there is one path
 * rather than two.
 *
 * Loop safety is applied here, before anything is enqueued.  Refusing at
 * the source is what stops a runaway fan-out: checking at delivery time
 * would already have written the messages.
 *
 * Returns: how many mailboxes it was queued into, or -1 on refusal
 */
gint clawt_mailbox_router_send(ClawtMailboxRouter  *self,
                               ClawtMessage        *message,
                               GError             **error);

/**
 * clawt_mailbox_router_send_to:
 * @self: a #ClawtMailboxRouter
 * @from: the sending agent, or "user" for a person
 * @target: a room id or an agent id
 * @body: what to say
 * @task_id: (nullable): the task this belongs to
 * @depth: how many hops the message has already travelled
 * @error: (out) (optional): return location for a #GError
 *
 * Convenience wrapper around clawt_mailbox_router_send().
 *
 * Returns: how many mailboxes it was queued into, or -1 on refusal
 */
gint clawt_mailbox_router_send_to(ClawtMailboxRouter  *self,
                                  const gchar         *from,
                                  const gchar         *target,
                                  const gchar         *body,
                                  const gchar         *task_id,
                                  gint                 depth,
                                  GError             **error);

/**
 * clawt_mailbox_router_drain:
 * @self: a #ClawtMailboxRouter
 * @agent_id: the agent to deliver to
 *
 * Hands as much of an agent's queue as it will take to its link.
 *
 * Stops at the first item that cannot be delivered and leaves the rest
 * pending, so a link that drops halfway through loses nothing and resumes
 * in order rather than skipping ahead.
 *
 * Returns: how many items were delivered
 */
guint clawt_mailbox_router_drain(ClawtMailboxRouter *self,
                                 const gchar        *agent_id);

/**
 * clawt_mailbox_router_drain_all:
 * @self: a #ClawtMailboxRouter
 *
 * Drains every connected agent.
 *
 * Returns: how many items were delivered in total
 */
guint clawt_mailbox_router_drain_all(ClawtMailboxRouter *self);

/**
 * clawt_mailbox_router_sweep:
 * @self: a #ClawtMailboxRouter
 *
 * Expires old items and returns abandoned leases to the queue.
 *
 * Returns: how many items were affected
 */
guint clawt_mailbox_router_sweep(ClawtMailboxRouter *self);

G_END_DECLS
