/*
 * clawt-decision-view.h - What every client draws about one decision
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
#include <json-glib/json-glib.h>

#include "core/clawt-event.h"
#include "decision/clawt-decision.h"

G_BEGIN_DECLS

/**
 * CLAWT_DECISION_VIA_CHAT:
 *
 * The `via` of an answer pressed on the inline view, in the room the
 * decision was asked from.
 */
#define CLAWT_DECISION_VIA_CHAT  "chat"

/**
 * CLAWT_DECISION_VIA_INBOX:
 *
 * The `via` of an answer given on the Decisions page.
 */
#define CLAWT_DECISION_VIA_INBOX "inbox"

/**
 * CLAWT_DECISION_VIA_CLI:
 *
 * The `via` of an answer given with `clawtilla decision answer`.
 */
#define CLAWT_DECISION_VIA_CLI   "cli"

/**
 * clawt_decision_via_is_known:
 * @via: (nullable): a `via` as a client sent it
 *
 * Whether @via names a surface this daemon can describe.  %NULL is
 * known -- it is "nobody said", which an old client and the venture
 * bridge both are -- and anything else not in the list is refused where
 * it arrives, because a word nobody can render would come back out of
 * every view of the decision as a blank.
 *
 * Returns: %TRUE if @via may be stored
 */
gboolean clawt_decision_via_is_known(const gchar *via);

/**
 * clawt_decision_new_from_object:
 * @object: (nullable): one decision as `decision.list` or
 *   `decision.answer` puts it on the wire
 *
 * The decision back as a #ClawtDecision, for a client holding the inline
 * view of it.  One reader, so the three clients cannot disagree about
 * which member is the room or how a state arrives.
 *
 * Returns: (transfer full) (nullable): the decision, or %NULL when
 *   @object has no id
 */
ClawtDecision *clawt_decision_new_from_object(JsonObject *object);

/**
 * clawt_decision_apply_settled:
 * @self: a #ClawtDecision a client is holding
 * @how: `answered`, `defaulted` or `dismissed`, from `decision.settled`
 * @answer: (nullable): what it was settled with
 * @via: (nullable): where a person settled it
 * @when: when, as Unix seconds
 *
 * Folds one `decision.settled` into a held copy, so the inline view in
 * the chat and the row in the inbox both change when either is pressed
 * -- or when neither is, and the clock settles it.  A copy that is
 * already settled is left alone: the daemon refuses a second settling,
 * so a second event about one is a replay, not news.
 *
 * Returns: %TRUE if anything changed
 */
gboolean clawt_decision_apply_settled(ClawtDecision *self,
                                      const gchar   *how,
                                      const gchar   *answer,
                                      const gchar   *via,
                                      gint64         when);

/**
 * clawt_decision_apply_event:
 * @self: a #ClawtDecision a client is holding
 * @event: (nullable): any event
 *
 * clawt_decision_apply_settled() from a `decision.settled` event about
 * @self; any other event, or one about another decision, changes
 * nothing.
 *
 * Returns: %TRUE if anything changed
 */
gboolean clawt_decision_apply_event(ClawtDecision *self, ClawtEvent *event);

/**
 * clawt_decision_event_room:
 * @event: (nullable): a `decision.*` event
 *
 * The room a decision event is about, or %NULL.  `decision.asked`,
 * `decision.settled` and `decision.deleted` all carry it once the
 * decision was asked from somewhere; a chat compares it against the
 * room on screen and ignores the rest.
 *
 * Returns: (transfer none) (nullable): the room id
 */
const gchar *clawt_decision_event_room(ClawtEvent *event);

/**
 * clawt_decision_outcome_text:
 * @self: a #ClawtDecision
 *
 * How it ended and who ended it, in one sentence: "You answered "X" in
 * the chat.", "Nobody answered in time, so the default stands: "X".",
 * "Dismissed in the inbox; the default stands: "X"."  Both views draw
 * it, so a person who answered in one sees the same words in the other.
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL while it
 *   is still open
 */
gchar *clawt_decision_outcome_text(ClawtDecision *self);

/**
 * clawt_decision_default_text:
 * @self: a #ClawtDecision
 * @tz: (nullable): the zone to write the deadline in, %NULL for local
 *
 * What happens if nobody answers, and when that stops being cheap to
 * undo: "If nobody answers: "X" -- reason. Reversible until Sat 15 Nov
 * 14:05."  The deadline is a clock time rather than "in 3h", because a
 * relative time in a transcript is wrong from the moment it is drawn and
 * nothing redraws an entry whose decision has not changed.
 *
 * Returns: (transfer full) (nullable): the line, or %NULL when there is
 *   no default
 */
gchar *clawt_decision_default_text(ClawtDecision *self, GTimeZone *tz);

/**
 * clawt_decision_refusal_text:
 * @self: a #ClawtDecision that is no longer open
 *
 * What the daemon says to a press on a decision that was already
 * settled: what happened, and that the press changed nothing.  A double
 * tap's second press gets this too, which is the point -- it is a press
 * on a decision its first press settled.
 *
 * Returns: (transfer full): the message
 */
gchar *clawt_decision_refusal_text(ClawtDecision *self);

/**
 * CLAWT_DECISION_LOSS_MARKER:
 *
 * Where a rebuild request's question says what a rebuild destroys.  The
 * daemon writes the loss after it, and hands it to clients again on its
 * own as the decision's `loses` member.
 */
#define CLAWT_DECISION_LOSS_MARKER "\n\nWhat a rebuild destroys: "

/**
 * clawt_decision_question_text:
 * @question: (nullable): a decision's `question`
 * @loses: (nullable): its `loses` member, when it has one
 *
 * The question to draw when the client draws what is lost on its own.
 * A rebuild request's question ends with what a rebuild destroys, and
 * every client also draws `loses` as a warning beside the options -- so
 * drawing the whole question said the same paragraph twice, the second
 * time in the card's heading.  With @loses %NULL or empty, or a question
 * that does not carry the marker, the question is returned whole.
 *
 * Returns: (transfer full): the question, without the loss paragraph
 */
gchar *clawt_decision_question_text(const gchar *question,
                                    const gchar *loses);

/**
 * clawt_decision_precedes:
 * @self: a #ClawtDecision
 * @message_ts: a #ClawtMessage timestamp, in seconds
 *
 * Whether @self was asked before @message_ts, for a client merging a
 * room's decisions back into its transcript.  Strictly before: a
 * question asked in the same second as the message that prompted it
 * goes after that message, which is the order it was asked in.
 *
 * Returns: %TRUE if it belongs before the message
 */
gboolean clawt_decision_precedes(ClawtDecision *self, gint64 message_ts);

G_END_DECLS
