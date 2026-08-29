/*
 * clawt-handoff-store.h - The handoff queue, and what became of each one
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Durable for two separate reasons, and it is worth keeping them apart:
 *
 *   **The queue.**  A handoff runs when the turn that asked for it
 *   settles.  A daemon restarted between those two moments would
 *   otherwise lose work that an agent has already been told is on its
 *   way -- it stopped being the source's problem the instant the tool
 *   answered.
 *
 *   **The receipts.**  #ClawtTaskManager is in memory and says so, so
 *   after a restart clawtilla cannot answer "what happened to the thing
 *   I handed over".  An agent reads that silence as "it never
 *   happened", hands the same work over again, and now there are two of
 *   it.  A receipt per handoff, kept for
 *   orchestration.handoff_receipt_days and bounded by row count, is what
 *   makes clawtilla_task_status answerable across a restart.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "task/clawt-handoff.h"

G_BEGIN_DECLS

/**
 * CLAWT_HANDOFF_STORE_MAX_RECEIPTS:
 *
 * How many settled receipts are kept whatever the retention window says.
 *
 * A bound as well as an age, because a fleet that hands work around all
 * day fills two days' worth faster than a quiet one fills two years',
 * and the file lives in the same state directory as everything else.
 */
#define CLAWT_HANDOFF_STORE_MAX_RECEIPTS (2000)

#define CLAWT_TYPE_HANDOFF_STORE (clawt_handoff_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtHandoffStore, clawt_handoff_store, CLAWT,
                     HANDOFF_STORE, GObject)

/**
 * clawt_handoff_store_new:
 * @path: where to keep them
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the store
 */
ClawtHandoffStore *clawt_handoff_store_new(const gchar *path,
                                           GError     **error);

/**
 * clawt_handoff_store_queue:
 * @self: a #ClawtHandoffStore
 * @handoff: (transfer none): the handoff, which must still be queued
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a handoff into the queue.
 *
 * Returns: %TRUE if it was written
 */
gboolean clawt_handoff_store_queue(ClawtHandoffStore *self,
                                   ClawtHandoff      *handoff,
                                   GError           **error);

/**
 * clawt_handoff_store_update:
 * @self: a #ClawtHandoffStore
 * @handoff: (transfer none): the handoff as it now stands
 * @error: (out) (optional): return location for a #GError
 *
 * Writes back the fields that change while a handoff waits and when it
 * settles: state, attempts, verdict and the settle stamp.
 *
 * Returns: %TRUE if a row was updated
 */
gboolean clawt_handoff_store_update(ClawtHandoffStore *self,
                                    ClawtHandoff      *handoff,
                                    GError           **error);

/**
 * clawt_handoff_store_get:
 * @self: a #ClawtHandoffStore
 * @id: which handoff
 *
 * Returns: (transfer full) (nullable): the handoff
 */
ClawtHandoff *clawt_handoff_store_get(ClawtHandoffStore *self,
                                      const gchar       *id);

/**
 * clawt_handoff_store_queued_from:
 * @self: a #ClawtHandoffStore
 * @from_agent: (nullable): only this agent's, or %NULL for every queued one
 *
 * Oldest first, because a chief that queued three handoffs meant them in
 * that order and the recipients see them in that order.
 *
 * Returns: (transfer full) (element-type ClawtHandoff): the queued ones
 */
GPtrArray *clawt_handoff_store_queued_from(ClawtHandoffStore *self,
                                           const gchar       *from_agent);

/**
 * clawt_handoff_store_count_queued:
 * @self: a #ClawtHandoffStore
 * @from_agent: (nullable): only this agent's, or %NULL for all
 *
 * What orchestration.handoff_max_per_turn is compared against.
 *
 * A count of what is *queued* rather than of what one turn asked for.
 * Usually the same number -- an agent's queue drains when its turn
 * settles -- and deliberately not the same when it does not: a transfer
 * still waiting for a busy recipient keeps counting, so an agent with
 * three stuck cannot queue four more on top of them.
 *
 * Counting the queue rather than keeping a per-turn tally also means
 * there is no counter to forget to clear, which is the shape that
 * produces a limit nothing ever reaches.
 *
 * Returns: how many are waiting
 */
guint clawt_handoff_store_count_queued(ClawtHandoffStore *self,
                                       const gchar       *from_agent);

/**
 * clawt_handoff_store_for_task:
 * @self: a #ClawtHandoffStore
 * @task_id: which task
 *
 * Every handoff of one task, oldest first -- which is its ownership
 * history, and survives the restart that empties #ClawtTaskManager.
 *
 * Returns: (transfer full) (element-type ClawtHandoff): the handoffs
 */
GPtrArray *clawt_handoff_store_for_task(ClawtHandoffStore *self,
                                        const gchar       *task_id);

/**
 * clawt_handoff_store_prune:
 * @self: a #ClawtHandoffStore
 * @keep_seconds: how long a settled receipt is kept, or 0 to keep by count only
 *
 * Drops settled receipts past the retention window, then past
 * %CLAWT_HANDOFF_STORE_MAX_RECEIPTS.
 *
 * A **queued** handoff is never pruned however old it is.  Age means
 * "this finished a while ago" for a receipt and "nothing has drained
 * this yet" for a queue entry, and deleting the second would silently
 * throw away work an agent was told was on its way.
 *
 * Returns: how many rows were removed
 */
guint clawt_handoff_store_prune(ClawtHandoffStore *self,
                                gint64             keep_seconds);

G_END_DECLS
