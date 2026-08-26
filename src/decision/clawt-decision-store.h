/*
 * clawt-decision-store.h - Decisions that outlive the agent that asked
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

#include "decision/clawt-decision.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_DECISION_STORE (clawt_decision_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtDecisionStore, clawt_decision_store, CLAWT,
                     DECISION_STORE, GObject)

/**
 * clawt_decision_store_new:
 * @path: where to keep them
 * @error: (out) (optional): return location for a #GError
 *
 * Durable, because a decision is worth less than nothing if it can be
 * lost: an agent that asked and got no answer carried on with its
 * default, and an operator who never saw the question has no way to
 * know that happened.  The mailbox is durable for the same reason one
 * layer along.
 *
 * Returns: (transfer full) (nullable): the store
 */
ClawtDecisionStore *clawt_decision_store_new(const gchar *path,
                                             GError     **error);

/**
 * clawt_decision_store_post:
 * @self: a #ClawtDecisionStore
 * @decision: (transfer none): the question
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the id it was stored under
 */
gchar *clawt_decision_store_post(ClawtDecisionStore *self,
                                 ClawtDecision      *decision,
                                 GError            **error);

/**
 * clawt_decision_store_list:
 * @self: a #ClawtDecisionStore
 * @open_only: %TRUE for the ones still waiting on somebody
 *
 * Ordered by urgency and then by age: an item whose deadline is close
 * comes first, because an inbox with no ordering is read top to bottom
 * until somebody gets bored and the one that stops being answerable
 * today would be as likely to be at the end as the start.
 *
 * Returns: (transfer full) (element-type ClawtDecision): the decisions
 */
GPtrArray *clawt_decision_store_list(ClawtDecisionStore *self,
                                     gboolean            open_only);

/**
 * clawt_decision_store_get:
 * @self: a #ClawtDecisionStore
 * @id: which one
 *
 * Returns: (transfer full) (nullable): the decision
 */
ClawtDecision *clawt_decision_store_get(ClawtDecisionStore *self,
                                        const gchar        *id);

/**
 * clawt_decision_store_answer:
 * @self: a #ClawtDecisionStore
 * @id: which one
 * @answer: what the person chose
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the answered decision, so the
 *   caller can route it back to whoever asked without a second lookup
 */
ClawtDecision *clawt_decision_store_answer(ClawtDecisionStore *self,
                                           const gchar        *id,
                                           const gchar        *answer,
                                           GError            **error);

/**
 * clawt_decision_store_dismiss:
 * @self: a #ClawtDecisionStore
 * @id: which one
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if it was there
 */
gboolean clawt_decision_store_dismiss(ClawtDecisionStore *self,
                                      const gchar        *id,
                                      GError            **error);

/**
 * clawt_decision_store_count_open:
 * @self: a #ClawtDecisionStore
 *
 * Returns: how many are still waiting on a person
 */
guint clawt_decision_store_count_open(ClawtDecisionStore *self);

G_END_DECLS
