/*
 * clawt-trigger-store.h - What a trigger has been sent, and what came of it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two jobs, and they are the same job: remembering deliveries.
 *
 * *Deduplication.* Every forge retries. A delivery already accepted must
 * not run twice however many times it arrives, so the sender's own
 * delivery id is the key -- and the duplicate check happens before the
 * pending-run cap, so a retry of accepted work stays idempotent even
 * when the queue is full. Only new work consumes a slot.
 *
 * *Receipts.* "Nothing happened" has four causes -- the endpoint is
 * wrong, the secret is wrong, the event was filtered out, or the run
 * failed -- and without a record they are indistinguishable from each
 * other and from a forge that never called. A receipt per delivery is
 * what makes `clawtilla trigger deliveries` an answer.
 *
 * It also holds the endpoint and the verification handshake, because
 * those are state rather than configuration: writing them into
 * clawtilla.yaml would rewrite somebody's file on the first delivery,
 * and a file that rewrites itself is one people stop keeping in git.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "trigger/clawt-trigger-event.h"

G_BEGIN_DECLS

/**
 * ClawtDeliveryOutcome:
 * @CLAWT_DELIVERY_RAN: it started a run
 * @CLAWT_DELIVERY_DUPLICATE: the same delivery id had been seen before
 * @CLAWT_DELIVERY_IGNORED: it was outside the event list or the filters
 * @CLAWT_DELIVERY_CAPTURED: it was the handshake delivery, held to be shown
 * @CLAWT_DELIVERY_REFUSED: it did not authenticate, or was too large
 * @CLAWT_DELIVERY_FAILED: it should have run and could not
 *
 * What became of one delivery.
 *
 * Five of the six are not errors, which is the point of recording them:
 * a forge that retries a delivery clawtilla deliberately ignored will
 * keep retrying it if it is told anything other than "received".
 */
typedef enum {
    CLAWT_DELIVERY_RAN = 0,
    CLAWT_DELIVERY_DUPLICATE,
    CLAWT_DELIVERY_IGNORED,
    CLAWT_DELIVERY_CAPTURED,
    CLAWT_DELIVERY_REFUSED,
    CLAWT_DELIVERY_FAILED
} ClawtDeliveryOutcome;

GType clawt_delivery_outcome_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_DELIVERY_OUTCOME (clawt_delivery_outcome_get_type())

#define CLAWT_TYPE_TRIGGER_STORE (clawt_trigger_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtTriggerStore, clawt_trigger_store,
                     CLAWT, TRIGGER_STORE, GObject)

/**
 * clawt_trigger_store_new:
 * @path: where the database lives
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the store
 */
ClawtTriggerStore *clawt_trigger_store_new(const gchar  *path,
                                           GError      **error);

/**
 * clawt_trigger_store_endpoint_for:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 * @create: whether to mint one if there is none
 * @error: (out) (optional): return location for a #GError
 *
 * The endpoint id this trigger answers on.
 *
 * Minted once and then stable: a trigger whose address changed every
 * time the daemon restarted would have to be re-registered with the
 * forge every time, and the symptom would be deliveries that stop
 * arriving with nothing anywhere saying why.
 *
 * Returns: (transfer full) (nullable): the endpoint id
 */
gchar *clawt_trigger_store_endpoint_for(ClawtTriggerStore  *self,
                                        const gchar        *trigger_id,
                                        gboolean            create,
                                        GError            **error);

/**
 * clawt_trigger_store_trigger_for_endpoint:
 * @self: a #ClawtTriggerStore
 * @endpoint: the endpoint id from the request path
 *
 * Which trigger answers on @endpoint.
 *
 * Returns: (transfer full) (nullable): the trigger id
 */
gchar *clawt_trigger_store_trigger_for_endpoint(ClawtTriggerStore *self,
                                                const gchar       *endpoint);

/**
 * clawt_trigger_store_rotate_endpoint:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 * @error: (out) (optional): return location for a #GError
 *
 * Gives the trigger a new address and forgets the old one at once.
 *
 * Returns: (transfer full) (nullable): the new endpoint id
 */
gchar *clawt_trigger_store_rotate_endpoint(ClawtTriggerStore  *self,
                                           const gchar        *trigger_id,
                                           GError            **error);

/**
 * clawt_trigger_store_is_pending_verification:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 *
 * Whether this trigger is still waiting for its first delivery.
 *
 * A new trigger starts unverified and the first delivery that
 * authenticates is *captured and shown* rather than run, so somebody can
 * see what the caller actually sends before an agent acts on it.
 *
 * Returns: %TRUE if the handshake has not happened
 */
gboolean clawt_trigger_store_is_pending_verification(ClawtTriggerStore *self,
                                                     const gchar *trigger_id);

/**
 * clawt_trigger_store_capture:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 * @event: the delivery to hold
 * @error: (out) (optional): return location for a #GError
 *
 * Records the handshake delivery and clears the pending flag.
 *
 * Returns: %TRUE if it was recorded
 */
gboolean clawt_trigger_store_capture(ClawtTriggerStore  *self,
                                     const gchar        *trigger_id,
                                     ClawtTriggerEvent  *event,
                                     GError            **error);

/**
 * clawt_trigger_store_get_capture:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 *
 * The body of the handshake delivery, for somebody to look at.
 *
 * Returns: (transfer full) (nullable): the captured payload
 */
gchar *clawt_trigger_store_get_capture(ClawtTriggerStore *self,
                                       const gchar       *trigger_id);

/**
 * clawt_trigger_store_reset_verification:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 *
 * Puts the trigger back to waiting for its first delivery.
 */
void clawt_trigger_store_reset_verification(ClawtTriggerStore *self,
                                            const gchar       *trigger_id);

/**
 * clawt_trigger_store_seen_delivery:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 * @delivery_id: (nullable): the sender's id for this delivery
 *
 * Whether this exact delivery has been dealt with before.
 *
 * A %NULL or empty @delivery_id is never a duplicate: a sender that
 * gives no id has told us nothing about whether this is a retry, and
 * treating "we cannot tell" as "already done" would drop real work.
 *
 * Returns: %TRUE if it has been seen
 */
gboolean clawt_trigger_store_seen_delivery(ClawtTriggerStore *self,
                                           const gchar       *trigger_id,
                                           const gchar       *delivery_id);

/**
 * clawt_trigger_store_record:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 * @event: (nullable): the delivery, when it got far enough to be one
 * @outcome: what became of it
 * @detail: (nullable): a sentence for a person
 * @task_id: (nullable): the run it started
 *
 * Writes one receipt.
 */
void clawt_trigger_store_record(ClawtTriggerStore    *self,
                                const gchar          *trigger_id,
                                ClawtTriggerEvent    *event,
                                ClawtDeliveryOutcome  outcome,
                                const gchar          *detail,
                                const gchar          *task_id);

/**
 * clawt_trigger_store_list_deliveries:
 * @self: a #ClawtTriggerStore
 * @trigger_id: (nullable): one trigger, or %NULL for every one
 * @limit: how many at most
 *
 * The receipts, newest first.
 *
 * Returns: (transfer full) (element-type GHashTable): one string table
 *   per delivery
 */
GPtrArray *clawt_trigger_store_list_deliveries(ClawtTriggerStore *self,
                                               const gchar       *trigger_id,
                                               guint              limit);

/**
 * clawt_trigger_store_count_unfinished:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 *
 * How many runs this trigger has started that nothing has finished.
 *
 * Returns: the count
 */
guint clawt_trigger_store_count_unfinished(ClawtTriggerStore *self,
                                           const gchar       *trigger_id);

/**
 * clawt_trigger_store_finish:
 * @self: a #ClawtTriggerStore
 * @task_id: the run that ended
 *
 * Marks the receipt that started @task_id as finished.
 */
void clawt_trigger_store_finish(ClawtTriggerStore *self,
                                const gchar       *task_id);

/**
 * clawt_trigger_store_recent_count:
 * @self: a #ClawtTriggerStore
 * @trigger_id: which trigger
 * @within_seconds: how far back to look
 *
 * How many deliveries this trigger has taken lately, for the rate limit.
 *
 * Returns: the count
 */
guint clawt_trigger_store_recent_count(ClawtTriggerStore *self,
                                       const gchar       *trigger_id,
                                       gint64             within_seconds);

/**
 * clawt_trigger_store_prune:
 * @self: a #ClawtTriggerStore
 * @retain_seconds: how long a receipt is worth keeping
 *
 * Drops receipts older than @retain_seconds.
 *
 * Bounded on purpose: the dedup window is exactly as long as the
 * retention, so a forge that retries a week-old delivery after a prune
 * runs it again. That is the trade, and it is written down here rather
 * than discovered -- an unbounded table would be a database that grows
 * by one row per push for ever.
 */
void clawt_trigger_store_prune(ClawtTriggerStore *self,
                               gint64             retain_seconds);

G_END_DECLS
