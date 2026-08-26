/*
 * clawt-decision.h - A choice an agent needs a human to make
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

G_BEGIN_DECLS

/**
 * ClawtDecisionState:
 * @CLAWT_DECISION_OPEN: waiting on a person
 * @CLAWT_DECISION_ANSWERED: a person chose
 * @CLAWT_DECISION_DEFAULTED: nobody chose in time and the agent's own
 *   default stands
 * @CLAWT_DECISION_DISMISSED: the operator decided it did not need them
 *
 * Answered and defaulted are deliberately different states.  Both mean
 * the work went a particular way; only one of them means somebody knew.
 * Collapsing them would make the inbox unable to answer the question it
 * exists for -- what did I actually decide, and what merely happened.
 */
typedef enum {
    CLAWT_DECISION_OPEN = 0,
    CLAWT_DECISION_ANSWERED,
    CLAWT_DECISION_DEFAULTED,
    CLAWT_DECISION_DISMISSED
} ClawtDecisionState;

#define CLAWT_TYPE_DECISION (clawt_decision_get_type())

typedef struct _ClawtDecision ClawtDecision;

GType clawt_decision_get_type(void) G_GNUC_CONST;

/**
 * clawt_decision_new:
 * @id: (nullable): the id, or %NULL to generate one
 * @agent: who is asking
 * @question: what they need decided
 *
 * Returns: (transfer full): a new #ClawtDecision
 */
ClawtDecision *clawt_decision_new(const gchar *id,
                                  const gchar *agent,
                                  const gchar *question);

ClawtDecision *clawt_decision_copy(ClawtDecision *self);
void           clawt_decision_free(ClawtDecision *self);

const gchar *clawt_decision_get_id(ClawtDecision *self);
const gchar *clawt_decision_get_agent(ClawtDecision *self);
const gchar *clawt_decision_get_question(ClawtDecision *self);

/**
 * clawt_decision_get_options:
 * @self: a #ClawtDecision
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): the
 *   choices offered
 */
const gchar * const *clawt_decision_get_options(ClawtDecision *self);
void clawt_decision_set_options(ClawtDecision *self,
                                const gchar * const *options);

/**
 * clawt_decision_get_default:
 * @self: a #ClawtDecision
 *
 * What the agent does if nobody answers.
 *
 * This is the field that makes the whole thing work rather than being a
 * queue of stalled work with a nicer name.  An agent that must state
 * what it will do anyway can keep going honestly, and the operator's
 * answer *redirects* rather than unblocks.
 *
 * Returns: (nullable): the default option
 */
const gchar *clawt_decision_get_default(ClawtDecision *self);
void         clawt_decision_set_default(ClawtDecision *self,
                                        const gchar   *option,
                                        const gchar   *reason);

/**
 * clawt_decision_get_default_reason:
 * @self: a #ClawtDecision
 *
 * Returns: (nullable): why that default, in the agent's words
 */
const gchar *clawt_decision_get_default_reason(ClawtDecision *self);

/**
 * clawt_decision_get_reversible_until:
 * @self: a #ClawtDecision
 *
 * When the default stops being cheap to undo, as Unix seconds, or 0 if
 * the agent did not say.
 *
 * Half of the honesty the default buys: "going right unless you say
 * otherwise" means something quite different when it can be undone all
 * week than when it ships in an hour.
 *
 * Returns: the deadline, or 0
 */
gint64 clawt_decision_get_reversible_until(ClawtDecision *self);
void   clawt_decision_set_reversible_until(ClawtDecision *self, gint64 when);

const gchar *clawt_decision_get_task(ClawtDecision *self);
void         clawt_decision_set_task(ClawtDecision *self, const gchar *task);

gint64 clawt_decision_get_created_at(ClawtDecision *self);
void   clawt_decision_set_created_at(ClawtDecision *self, gint64 when);

ClawtDecisionState clawt_decision_get_state(ClawtDecision *self);
void clawt_decision_set_state(ClawtDecision *self, ClawtDecisionState state);

const gchar *clawt_decision_get_answer(ClawtDecision *self);

/**
 * clawt_decision_answer:
 * @self: a #ClawtDecision
 * @option: what the person chose
 * @when: the time, as Unix seconds
 *
 * Records a person's choice.
 *
 * Free text rather than an index into the options, because an operator
 * whose answer is "neither, do X" is giving the most valuable answer
 * there is and an inbox that could not carry it would push them back
 * into the conversation this exists to keep them out of.
 */
void clawt_decision_answer(ClawtDecision *self,
                           const gchar   *option,
                           gint64         when);

gint64 clawt_decision_get_answered_at(ClawtDecision *self);
void   clawt_decision_set_answered_at(ClawtDecision *self, gint64 when);

/**
 * clawt_decision_is_urgent:
 * @self: a #ClawtDecision
 * @now: the current time, as Unix seconds
 *
 * Whether the point of no return is close enough to matter.
 *
 * An inbox with no ordering is read top to bottom until somebody gets
 * bored, so the one item that stops being answerable today has to be
 * distinguishable from the one that can wait a week.  A decision with
 * no stated deadline is never urgent: an agent that did not say cannot
 * have its silence read as pressure.
 *
 * Returns: %TRUE if the deadline is inside a day
 */
gboolean clawt_decision_is_urgent(ClawtDecision *self, gint64 now);

/**
 * clawt_decision_default_has_taken_effect:
 * @self: a #ClawtDecision
 * @now: the current time, as Unix seconds
 *
 * Whether answering this can still change anything.
 *
 * Past the reversibility deadline an unanswered decision has been made
 * -- by nobody, which is the outcome this feature exists to make
 * visible rather than to prevent.  Saying so is the difference between
 * an inbox and a list of regrets: an operator who answers a stale item
 * should be told the work already went the other way, not thanked.
 *
 * Returns: %TRUE if the default is now what happened
 */
gboolean clawt_decision_default_has_taken_effect(ClawtDecision *self,
                                                 gint64         now);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtDecision, clawt_decision_free)

G_END_DECLS
