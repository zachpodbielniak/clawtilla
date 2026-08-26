/*
 * clawt-decision.c - A choice an agent needs a human to make
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "decision/clawt-decision.h"

/*
 * How close a deadline has to be before an item is called urgent.
 *
 * A day, because that is the horizon an operator can act inside without
 * rearranging anything: an item that stops being answerable this
 * afternoon needs to be told apart from one that stops next week, and a
 * finer distinction than "today" is not one anybody acts on differently.
 */
#define URGENT_WINDOW_SECONDS (24 * 60 * 60)

struct _ClawtDecision {
    gchar             *id;
    gchar             *agent;
    gchar             *question;
    GStrv              options;
    gchar             *default_option;
    gchar             *default_reason;
    gchar             *task;
    gchar             *answer;
    gint64             reversible_until;
    gint64             created_at;
    gint64             answered_at;
    ClawtDecisionState state;
};

G_DEFINE_BOXED_TYPE(ClawtDecision, clawt_decision, clawt_decision_copy,
                    clawt_decision_free)

ClawtDecision *
clawt_decision_new(
    const gchar *id,
    const gchar *agent,
    const gchar *question
){
    ClawtDecision *self = g_new0(ClawtDecision, 1);

    self->id = (id != NULL) ? g_strdup(id) : g_uuid_string_random();
    self->agent = g_strdup(agent);
    self->question = g_strdup(question);
    self->created_at = g_get_real_time() / G_USEC_PER_SEC;
    self->state = CLAWT_DECISION_OPEN;

    return self;
}

ClawtDecision *
clawt_decision_copy(ClawtDecision *self)
{
    ClawtDecision *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtDecision, 1);
    copy->id = g_strdup(self->id);
    copy->agent = g_strdup(self->agent);
    copy->question = g_strdup(self->question);
    copy->options = (self->options != NULL) ? g_strdupv(self->options) : NULL;
    copy->default_option = g_strdup(self->default_option);
    copy->default_reason = g_strdup(self->default_reason);
    copy->task = g_strdup(self->task);
    copy->answer = g_strdup(self->answer);
    copy->reversible_until = self->reversible_until;
    copy->created_at = self->created_at;
    copy->answered_at = self->answered_at;
    copy->state = self->state;

    return copy;
}

void
clawt_decision_free(ClawtDecision *self)
{
    if (self == NULL)
        return;

    g_free(self->id);
    g_free(self->agent);
    g_free(self->question);
    g_strfreev(self->options);
    g_free(self->default_option);
    g_free(self->default_reason);
    g_free(self->task);
    g_free(self->answer);
    g_free(self);
}

const gchar *
clawt_decision_get_id(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->id;
}

const gchar *
clawt_decision_get_agent(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->agent;
}

const gchar *
clawt_decision_get_question(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->question;
}

const gchar * const *
clawt_decision_get_options(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return (const gchar * const *)self->options;
}

void
clawt_decision_set_options(ClawtDecision *self, const gchar * const *options)
{
    g_return_if_fail(self != NULL);

    g_clear_pointer(&self->options, g_strfreev);

    if (options != NULL)
        self->options = g_strdupv((GStrv)options);
}

const gchar *
clawt_decision_get_default(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->default_option;
}

void
clawt_decision_set_default(
    ClawtDecision *self,
    const gchar   *option,
    const gchar   *reason
){
    g_return_if_fail(self != NULL);

    g_free(self->default_option);
    g_free(self->default_reason);
    self->default_option = g_strdup(option);
    self->default_reason = g_strdup(reason);
}

const gchar *
clawt_decision_get_default_reason(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->default_reason;
}

gint64
clawt_decision_get_reversible_until(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->reversible_until;
}

void
clawt_decision_set_reversible_until(ClawtDecision *self, gint64 when)
{
    g_return_if_fail(self != NULL);

    self->reversible_until = when > 0 ? when : 0;
}

const gchar *
clawt_decision_get_task(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->task;
}

void
clawt_decision_set_task(ClawtDecision *self, const gchar *task)
{
    g_return_if_fail(self != NULL);

    g_free(self->task);
    self->task = g_strdup(task);
}

gint64
clawt_decision_get_created_at(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->created_at;
}

void
clawt_decision_set_created_at(ClawtDecision *self, gint64 when)
{
    g_return_if_fail(self != NULL);

    self->created_at = when;
}

ClawtDecisionState
clawt_decision_get_state(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_DECISION_OPEN);

    return self->state;
}

void
clawt_decision_set_state(ClawtDecision *self, ClawtDecisionState state)
{
    g_return_if_fail(self != NULL);

    self->state = state;
}

const gchar *
clawt_decision_get_answer(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->answer;
}

void
clawt_decision_answer(
    ClawtDecision *self,
    const gchar   *option,
    gint64         when
){
    g_return_if_fail(self != NULL);

    g_free(self->answer);
    self->answer = g_strdup(option);
    self->answered_at = when;
    self->state = CLAWT_DECISION_ANSWERED;
}

gint64
clawt_decision_get_answered_at(ClawtDecision *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->answered_at;
}

void
clawt_decision_set_answered_at(ClawtDecision *self, gint64 when)
{
    g_return_if_fail(self != NULL);

    self->answered_at = when;
}

gboolean
clawt_decision_is_urgent(ClawtDecision *self, gint64 now)
{
    g_return_val_if_fail(self != NULL, FALSE);

    /*
     * An agent that did not state a deadline cannot have its silence
     * read as pressure.  Treating "unknown" as urgent would put every
     * lazily-filed item at the top and the ordering would stop meaning
     * anything within a day.
     */
    if (self->reversible_until <= 0)
        return FALSE;

    /* Already past is not urgent; it is over.  See below. */
    if (self->reversible_until <= now)
        return FALSE;

    return (self->reversible_until - now) <= URGENT_WINDOW_SECONDS;
}

gboolean
clawt_decision_default_has_taken_effect(ClawtDecision *self, gint64 now)
{
    g_return_val_if_fail(self != NULL, FALSE);

    /*
     * A decision somebody answered is theirs whatever the clock says.
     * Only an *open* one can be overtaken.
     */
    if (self->state != CLAWT_DECISION_OPEN)
        return FALSE;

    if (self->reversible_until <= 0)
        return FALSE;

    return now >= self->reversible_until;
}
