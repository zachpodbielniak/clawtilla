/*
 * clawt-mailbox.h - An agent's durable message queue
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every agent has one.  It is what lets you message an agent that is
 * switched off: the message waits, and is delivered when the agent starts.
 * Without it, "send a message" would only work against agents that happen
 * to be running, and a chief-of-staff could not fan work out faster than
 * the workers drain it.
 *
 * Backed by SQLite so the queue survives the daemon being restarted --
 * which happens whenever the configuration changes.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "mailbox/clawt-mailbox-item.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MAILBOX (clawt_mailbox_get_type())

G_DECLARE_FINAL_TYPE(ClawtMailbox, clawt_mailbox, CLAWT, MAILBOX, GObject)

/**
 * ClawtMailboxFilter:
 * @state: only items in this state, or -1 for any
 * @limit: at most this many, or 0 for no limit
 * @include_future: whether to include items whose not_before has not passed
 *
 * Narrows a listing.
 */
struct _ClawtMailboxFilter {
    gint     state;
    guint    limit;
    gboolean include_future;
};

/**
 * clawt_mailbox_new:
 * @agent_id: the agent this belongs to
 * @db_path: where to keep the queue
 * @error: (out) (optional): return location for a #GError
 *
 * Opens or creates an agent's mailbox.
 *
 * A database that cannot be read is moved aside and recreated rather than
 * failing: a corrupt queue should cost the messages in it, not the agent.
 *
 * Returns: (transfer full) (nullable): the mailbox, or %NULL
 */
ClawtMailbox *clawt_mailbox_new(const gchar  *agent_id,
                                const gchar  *db_path,
                                GError      **error);

/**
 * clawt_mailbox_set_policy:
 * @self: a #ClawtMailbox
 * @max_depth: how many undelivered items to hold, or 0 for no limit
 * @overflow: what to do when full
 * @max_attempts: deliveries before dead-lettering
 * @lease_seconds: how long an agent has to acknowledge
 * @backoff_seconds: base for the retry backoff
 * @default_ttl_seconds: how long an item lives, or 0 for forever
 *
 * Sets the queue's limits.
 */
void clawt_mailbox_set_policy(ClawtMailbox        *self,
                              guint                max_depth,
                              ClawtOverflowPolicy  overflow,
                              guint                max_attempts,
                              guint                lease_seconds,
                              guint                backoff_seconds,
                              guint                default_ttl_seconds);

/**
 * clawt_mailbox_post:
 * @self: a #ClawtMailbox
 * @item: (transfer none): the message
 * @error: (out) (optional): return location for a #GError
 *
 * Enqueues a message.
 *
 * Returns: (transfer full) (nullable): the item's id, or %NULL if it was
 *   refused
 */
gchar *clawt_mailbox_post(ClawtMailbox      *self,
                          ClawtMailboxItem  *item,
                          GError           **error);

/**
 * clawt_mailbox_lease:
 * @self: a #ClawtMailbox
 * @lease_seconds: how long the caller has, or 0 for the configured default
 *
 * Takes the next deliverable item and marks it leased.
 *
 * The lease is what makes delivery survive an agent dying mid-turn: the
 * item returns to the queue when the lease expires, rather than being lost
 * or silently delivered twice.
 *
 * Returns: (transfer full) (nullable): the item, or %NULL if none is ready
 */
ClawtMailboxItem *clawt_mailbox_lease(ClawtMailbox *self,
                                      guint         lease_seconds);

/**
 * clawt_mailbox_ack:
 * @self: a #ClawtMailbox
 * @id: the item's id
 * @error: (out) (optional): return location for a #GError
 *
 * Marks a leased item done.
 *
 * Returns: %TRUE if the item was leased and is now acknowledged
 */
gboolean clawt_mailbox_ack(ClawtMailbox  *self,
                           const gchar   *id,
                           GError       **error);

/**
 * clawt_mailbox_nack:
 * @self: a #ClawtMailbox
 * @id: the item's id
 * @reason: (nullable): why it failed, kept for the dead-letter view
 * @error: (out) (optional): return location for a #GError
 *
 * Returns a leased item to the queue, or dead-letters it if its attempts
 * are exhausted.  The retry is delayed by an exponential backoff with
 * jitter, so a failing dependency is not hammered in lockstep.
 *
 * Returns: %TRUE if the item was leased and has been handled
 */
gboolean clawt_mailbox_nack(ClawtMailbox  *self,
                            const gchar   *id,
                            const gchar   *reason,
                            GError       **error);

/**
 * clawt_mailbox_requeue:
 * @self: a #ClawtMailbox
 * @id: a dead-lettered item's id
 * @error: (out) (optional): return location for a #GError
 *
 * Puts a dead letter back in the queue with its attempts reset.
 *
 * Returns: %TRUE if the item was dead and is now pending
 */
gboolean clawt_mailbox_requeue(ClawtMailbox  *self,
                               const gchar   *id,
                               GError       **error);

/**
 * clawt_mailbox_list:
 * @self: a #ClawtMailbox
 * @filter: (nullable): what to include, or %NULL for everything pending
 *
 * Returns: (transfer full) (element-type ClawtMailboxItem): matching items,
 *   in delivery order
 */
GPtrArray *clawt_mailbox_list(ClawtMailbox       *self,
                              ClawtMailboxFilter *filter);

/**
 * clawt_mailbox_get:
 * @self: a #ClawtMailbox
 * @id: an item's id
 *
 * Returns: (transfer full) (nullable): the item, or %NULL
 */
ClawtMailboxItem *clawt_mailbox_get(ClawtMailbox *self, const gchar *id);

/**
 * clawt_mailbox_dead_letters:
 * @self: a #ClawtMailbox
 *
 * Returns: (transfer full) (element-type ClawtMailboxItem): items that
 *   exhausted their attempts
 */
GPtrArray *clawt_mailbox_dead_letters(ClawtMailbox *self);

/**
 * clawt_mailbox_purge_expired:
 * @self: a #ClawtMailbox
 *
 * Removes items past their expiry, logging each.
 *
 * Returns: how many were removed
 */
guint clawt_mailbox_purge_expired(ClawtMailbox *self);

/**
 * clawt_mailbox_reclaim_expired_leases:
 * @self: a #ClawtMailbox
 *
 * Returns items whose lease ran out to the pending queue.
 *
 * This is what recovers work from an agent that died holding it.
 *
 * Returns: how many were reclaimed
 */
guint clawt_mailbox_reclaim_expired_leases(ClawtMailbox *self);

/**
 * clawt_mailbox_depth:
 * @self: a #ClawtMailbox
 *
 * Returns: how many items are waiting to be delivered
 */
guint clawt_mailbox_depth(ClawtMailbox *self);

/**
 * clawt_mailbox_get_agent_id:
 * @self: a #ClawtMailbox
 *
 * Returns: (transfer none): the agent this belongs to
 */
const gchar *clawt_mailbox_get_agent_id(ClawtMailbox *self);

G_END_DECLS
