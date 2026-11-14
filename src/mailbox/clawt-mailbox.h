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
 * clawt_mailbox_lease_id:
 * @self: a #ClawtMailbox
 * @id: the item to lease
 * @lease_seconds: how long the caller has, or 0 for the configured default
 *
 * Leases one particular item, if it is deliverable now: pending, past
 * its not_before and not expired -- the same conditions
 * clawt_mailbox_lease() applies, so naming an item cannot take it out of
 * its backoff early.
 *
 * For the router's merge, which finds the other results waiting in the
 * same room with clawt_mailbox_list() and then takes exactly those.
 *
 * Returns: (transfer full) (nullable): the item, or %NULL if it is not
 *   deliverable
 */
ClawtMailboxItem *clawt_mailbox_lease_id(ClawtMailbox *self,
                                         const gchar  *id,
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

/**
 * ClawtTurnRecord: (skip)
 * @id: the record's own id
 * @room: (nullable): the room the turn ran in
 * @items: (array zero-terminated=1): the mailbox items the delivery
 *   carried -- one, or every part of a merged delivery
 * @replays: how many times those items had already been handed back
 * @outward: (nullable): what the turn did outside the fleet, comma
 *   separated, or %NULL when nothing outward is known
 * @opened_at: when the delivery was handed over, in microseconds
 * @redelivered: set by clawt_mailbox_turn_recover(): %TRUE when the
 *   items went back into the queue
 * @reason: (nullable): set by clawt_mailbox_turn_recover() when they did
 *   not: why the turn ends as interrupted instead
 * @from: (nullable): who sent the first item, if it is still queued
 * @body: (nullable): the first item's body, if it is still queued
 *
 * One delivery an agent was handed and had not finished, as the
 * mailbox remembers it across a restart.
 */
typedef struct {
	gint64    id;
	gchar    *room;
	gchar   **items;
	guint     replays;
	gchar    *outward;
	gint64    opened_at;
	gboolean  redelivered;
	gchar    *reason;
	gchar    *from;
	gchar    *body;
} ClawtTurnRecord;

/**
 * clawt_turn_record_free: (skip)
 * @record: (nullable): a #ClawtTurnRecord
 *
 * Frees @record.
 */
void clawt_turn_record_free(ClawtTurnRecord *record);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTurnRecord, clawt_turn_record_free)

/**
 * clawt_mailbox_turn_open:
 * @self: a #ClawtMailbox
 * @room_id: (nullable): the room the delivery went into
 * @item_ids: (array zero-terminated=1): the items it carried
 * @replays: how many times they had already been handed back
 * @error: return location for a #GError
 *
 * Records a delivery an agent has just been handed, before it has
 * started -- or finished -- the turn it sets up.
 *
 * The queue forgets a delivery once it is acknowledged at the socket,
 * and libreclaw's own queue is in memory, so without this a process
 * that died mid-turn took the turn with it and nothing knew to say so.
 * The record is closed when the turn ends (clawt_mailbox_turn_close())
 * and read back on the next start (clawt_mailbox_turn_recover()).  The
 * items it names are kept from the acknowledged-item sweep while it is
 * open, so they can be handed back.
 *
 * Returns: the record's id, or 0 on failure
 */
gint64 clawt_mailbox_turn_open(ClawtMailbox        *self,
                               const gchar         *room_id,
                               const gchar * const *item_ids,
                               guint                replays,
                               GError             **error);

/**
 * clawt_mailbox_turn_mark_outward:
 * @self: a #ClawtMailbox
 * @turn: a record from clawt_mailbox_turn_open()
 * @what: a short label for what the turn did outside the fleet
 * @error: return location for a #GError
 *
 * Marks an open turn as having acted outward -- sent something to a
 * person, written through a connector, run a command on a machine that
 * is not a sandbox.  A marked turn is never rerun after a restart,
 * because the rerun would do it twice; it ends as interrupted instead.
 * Each label is kept once.
 *
 * Returns: %TRUE if the record was marked
 */
gboolean clawt_mailbox_turn_mark_outward(ClawtMailbox  *self,
                                         gint64         turn,
                                         const gchar   *what,
                                         GError       **error);

/**
 * clawt_mailbox_turn_close:
 * @self: a #ClawtMailbox
 * @turn: a record from clawt_mailbox_turn_open()
 * @error: return location for a #GError
 *
 * Forgets a turn that ended normally.
 *
 * Returns: %TRUE on success, including a record already gone
 */
gboolean clawt_mailbox_turn_close(ClawtMailbox  *self,
                                  gint64         turn,
                                  GError       **error);

/**
 * clawt_mailbox_turn_join:
 * @self: a #ClawtMailbox
 * @into: the record of the turn that took a steered message
 * @from: the steered message's own record
 * @error: return location for a #GError
 *
 * Moves @from's items onto @into and forgets @from, in one transaction.
 * A steered message never starts a turn of its own: its words are part
 * of the turn @into records.  Joined, a restart that cut that turn hands
 * every item back as one turn, or ends it as interrupted, and never
 * reruns the steered words alone after the turn that already read them.
 * @into keeps the higher replay count and gains @from's outward marks.
 *
 * Returns: %TRUE on success, including @from already gone
 *
 * Since: 0.26.0
 */
gboolean clawt_mailbox_turn_join(ClawtMailbox  *self,
                                 gint64         into,
                                 gint64         from,
                                 GError       **error);

/**
 * clawt_mailbox_turn_list_open: (skip)
 * @self: a #ClawtMailbox
 *
 * Returns: (transfer full) (element-type ClawtTurnRecord): every open
 *   turn record, oldest first
 */
GPtrArray *clawt_mailbox_turn_list_open(ClawtMailbox *self);

/**
 * clawt_mailbox_turn_count_open:
 * @self: a #ClawtMailbox
 *
 * Returns: how many turn records are open
 */
guint clawt_mailbox_turn_count_open(ClawtMailbox *self);

/**
 * ClawtTurnVetoFunc: (skip)
 * @mailbox: the mailbox being recovered
 * @record: a turn about to be handed back
 * @user_data: as supplied
 *
 * A last word on a turn clawt_mailbox_turn_recover() would rerun, for a
 * reason only the caller knows -- the task it belonged to has already
 * been failed, say.
 *
 * Returns: (transfer none) (nullable): why it must not be rerun, or
 *   %NULL to let it be
 */
typedef const gchar *(*ClawtTurnVetoFunc)(ClawtMailbox    *mailbox,
                                          ClawtTurnRecord *record,
                                          gpointer         user_data);

/**
 * clawt_mailbox_turn_recover: (skip)
 * @self: a #ClawtMailbox
 * @replay: %FALSE to end every open turn as interrupted, for a stop
 *   somebody asked for
 * @reason: (nullable): why, when @replay is %FALSE
 * @veto: (nullable) (scope call): asked about each turn that would be
 *   handed back
 * @veto_data: data for @veto
 * @error: return location for a #GError
 *
 * Settles every open turn record, once, after the process that held
 * them has gone.
 *
 * A turn is handed back -- its items returned to the queue with their
 * replay count raised -- only when nothing outward is known of it, it
 * was not itself a rerun, and its items are still there.  Every other
 * turn ends as interrupted, with @reason set on its record for the
 * notice the caller posts.  Either way the record is deleted in the
 * same transaction, so no turn is handed back twice however many
 * restarts follow.
 *
 * Returns: (transfer full) (element-type ClawtTurnRecord) (nullable):
 *   the settled records, with @redelivered or @reason filled in, or
 *   %NULL on failure
 */
GPtrArray *clawt_mailbox_turn_recover(ClawtMailbox       *self,
                                      gboolean            replay,
                                      const gchar        *reason,
                                      ClawtTurnVetoFunc   veto,
                                      gpointer            veto_data,
                                      GError            **error);

G_END_DECLS
