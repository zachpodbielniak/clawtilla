/*
 * clawt-venture.h - A staged VENTURE write, as something to decide
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * VENTURE stages a write rather than applying it -- `?stage=1` on any of
 * its four write verbs mints a *confirmation* and answers 202 -- and it
 * offers that queue at `GET /api/v1/confirmations` for exactly this: a
 * decision inbox in another program.
 *
 * clawtilla already has one.  So the whole of this file is a
 * translation: venture's card into a #ClawtDecision, and back again
 * into the two endpoints that answer it.  Nothing here talks to a
 * socket; #ClawtVentureBridge does the asking and this does the
 * reading, which is what lets every rule below be tested against a
 * fixture with no server anywhere.
 *
 * The mapping is deliberately literal.  The *question* is the staged
 * change as venture summarised it; the *default* is reject, because a
 * change nobody looked at must not become a change nobody stopped; and
 * `reversible_until` stays unset, because venture's own soft delete is
 * the undo and a deadline invented here would be a promise clawtilla
 * cannot keep.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "decision/clawt-decision.h"

G_BEGIN_DECLS

/**
 * CLAWT_VENTURE_APPROVE:
 *
 * The answer that applies a staged change.
 */
#define CLAWT_VENTURE_APPROVE "approve"

/**
 * CLAWT_VENTURE_REJECT:
 *
 * The answer that discards one, and the default for every card.
 */
#define CLAWT_VENTURE_REJECT "reject"

/**
 * ClawtVentureConfirmation:
 * @id: venture's own id for the card, which the answer endpoints take
 * @summary: venture's one line about what would change
 * @action: `create`, `update` or `delete`
 * @state: `pending` for anything worth raising
 * @record_type: (nullable): which record type, when there is a record
 * @label: (nullable): what that record is called
 * @record_id: the record's id, or 0 for a creation
 * @origin_kind: (nullable): `user`, `import`, `ai` or `system`
 * @origin_name: (nullable): who asked -- the token's name, for an agent
 * @origin_via: (nullable): how it arrived, e.g. `rest-api`
 * @created_at: (nullable): ISO 8601, as venture wrote it
 * @expires_at: (nullable): when venture will drop it unanswered
 * @diff: (nullable): the staged fields, rendered one per line
 *
 * One change waiting for a person, as `GET /api/v1/confirmations` sent
 * it.
 *
 * A record rather than a class because it is pure description that
 * lives for one poll: it is parsed, turned into a #ClawtDecision, and
 * dropped.
 *
 * @record_id is deliberately 0 rather than -1 for a creation, matching
 * venture, which omits the member entirely when there is no record yet
 * -- a client that read 0 as an id would go looking for a row that does
 * not exist.
 */
typedef struct {
    gchar  *id;
    gchar  *summary;
    gchar  *action;
    gchar  *state;
    gchar  *record_type;
    gchar  *label;
    gint64  record_id;
    gchar  *origin_kind;
    gchar  *origin_name;
    gchar  *origin_via;
    gchar  *created_at;
    gchar  *expires_at;
    gchar  *diff;
} ClawtVentureConfirmation;

#define CLAWT_TYPE_VENTURE_CONFIRMATION \
    (clawt_venture_confirmation_get_type())

GType clawt_venture_confirmation_get_type(void) G_GNUC_CONST;

/**
 * clawt_venture_confirmation_copy:
 * @self: a #ClawtVentureConfirmation
 *
 * Returns: (transfer full): a deep copy
 */
ClawtVentureConfirmation *
clawt_venture_confirmation_copy(ClawtVentureConfirmation *self);

/**
 * clawt_venture_confirmation_free:
 * @self: (transfer full) (nullable): a #ClawtVentureConfirmation
 *
 * Frees one.
 */
void clawt_venture_confirmation_free(ClawtVentureConfirmation *self);

/**
 * clawt_venture_confirmations_parse:
 * @json: the body of `GET /api/v1/confirmations`
 * @length: how many bytes of @json, or -1 if it is NUL-terminated
 * @error: (out) (optional): return location for a #GError
 *
 * Reads venture's queue.
 *
 * A card missing an id is skipped with a warning naming its position,
 * rather than failing the whole poll: one malformed entry must not
 * hide every other change waiting for an answer, and the next poll
 * would fail identically, so a hard error here would mean an inbox that
 * silently stops updating.
 *
 * A body that is not an array at all *is* an error -- that is a proxy,
 * a login page or the wrong host, and treating it as an empty queue
 * would report "nothing is waiting" about a server nobody reached.
 *
 * Returns: (transfer full) (nullable) (element-type ClawtVentureConfirmation):
 *   the cards, or %NULL with @error set
 */
GPtrArray *clawt_venture_confirmations_parse(const gchar  *json,
                                             gssize        length,
                                             GError      **error);

/**
 * clawt_venture_decision_id:
 * @instance: the integration instance's name
 * @confirmation_id: venture's id for the card
 *
 * The decision id one confirmation always gets.
 *
 * Derived rather than generated, because that is what makes a second
 * poll idempotent: the store is asked whether it already holds this id
 * and a card that has been seen is skipped.  A fresh id per poll would
 * fill the inbox with one copy per interval of every change nobody has
 * answered yet.
 *
 * Keyed by the instance as well, since two venture servers -- a live
 * one and a staging one -- can mint the same short id.
 *
 * Returns: (transfer full): the id
 */
gchar *clawt_venture_decision_id(const gchar *instance,
                                 const gchar *confirmation_id);

/**
 * clawt_venture_decision_for:
 * @confirmation: a card
 * @instance: the integration instance's name
 * @agent_id: who the decision is filed against
 *
 * Turns one staged change into a question for the operator.
 *
 * The options are exactly #CLAWT_VENTURE_APPROVE and
 * #CLAWT_VENTURE_REJECT, and the default is reject with a reason
 * naming venture's own expiry: an unanswered card is dropped by
 * venture when its `ai.confirmation_ttl` runs out, so "reject" is a
 * description of what will happen rather than a policy invented here.
 *
 * Returns: (transfer full): the decision
 */
ClawtDecision *clawt_venture_decision_for(
    ClawtVentureConfirmation *confirmation,
    const gchar              *instance,
    const gchar              *agent_id);

/**
 * clawt_venture_confirmations_url:
 * @base: the instance, e.g. `http://localhost:8747`
 *
 * Returns: (transfer full) (nullable): where the queue is read
 */
gchar *clawt_venture_confirmations_url(const gchar *base);

/**
 * clawt_venture_answer_url:
 * @base: the instance
 * @confirmation_id: venture's id for the card
 * @approve: %TRUE to apply the change, %FALSE to discard it
 *
 * Returns: (transfer full) (nullable): where the answer is posted
 */
gchar *clawt_venture_answer_url(const gchar *base,
                                const gchar *confirmation_id,
                                gboolean     approve);

/**
 * clawt_venture_answer_is_approval:
 * @answer: what the person said
 *
 * Whether an operator's answer means "apply it".
 *
 * A #ClawtDecision carries free text rather than an index, because an
 * operator whose answer is "neither, do X" is giving the most useful
 * answer there is.  Venture has only two endpoints, so anything that is
 * not recognisably approval is a rejection -- which is the safe
 * direction: a misread "yes" writes to somebody's books, and a misread
 * "no" leaves a card for them to answer again.
 *
 * Returns: %TRUE for an approval
 */
gboolean clawt_venture_answer_is_approval(const gchar *answer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtVentureConfirmation,
                              clawt_venture_confirmation_free)

G_END_DECLS
