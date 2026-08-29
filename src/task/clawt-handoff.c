/*
 * clawt-handoff.c - Handing ownership of a task to somebody else
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-handoff.h"

struct _ClawtHandoff {
    gint ref_count;

    gchar *id;
    gchar *task_id;
    gchar *from_agent;
    gchar *to_agent;
    gchar *reason;
    gchar *room;
    gchar *verdict;

    ClawtHandoffState state;
    guint             attempts;
    gint              depth;
    gint64            created_at;
    gint64            settled_at;
};

static ClawtHandoff *
clawt_handoff_ref(ClawtHandoff *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtHandoff, clawt_handoff, clawt_handoff_ref,
                    clawt_handoff_free)

ClawtHandoff *
clawt_handoff_new(const gchar *task_id,
                  const gchar *from_agent,
                  const gchar *to_agent,
                  const gchar *reason)
{
    ClawtHandoff *self;

    g_return_val_if_fail(task_id != NULL, NULL);
    g_return_val_if_fail(to_agent != NULL, NULL);

    self = g_new0(ClawtHandoff, 1);
    self->ref_count = 1;
    self->id = clawt_generate_id("handoff");
    self->task_id = g_strdup(task_id);
    self->from_agent = g_strdup(from_agent);
    self->to_agent = g_strdup(to_agent);
    self->reason = g_strdup(reason);
    self->state = CLAWT_HANDOFF_QUEUED;

    /*
     * Seconds, matching #ClawtTask rather than the microseconds every
     * event carries.  The two live side by side in one listing, and a
     * mixture is how `20694d ago` -- the epoch -- ended up rendered on
     * every task row once already.  One unit per neighbourhood, and the
     * callers that hand this to clawt_time_ago_label() multiply.
     */
    self->created_at = g_get_real_time() / G_USEC_PER_SEC;

    return self;
}

ClawtHandoff *
clawt_handoff_copy(ClawtHandoff *self)
{
    ClawtHandoff *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtHandoff, 1);
    copy->ref_count = 1;
    copy->id = g_strdup(self->id);
    copy->task_id = g_strdup(self->task_id);
    copy->from_agent = g_strdup(self->from_agent);
    copy->to_agent = g_strdup(self->to_agent);
    copy->reason = g_strdup(self->reason);
    copy->room = g_strdup(self->room);
    copy->verdict = g_strdup(self->verdict);
    copy->state = self->state;
    copy->attempts = self->attempts;
    copy->depth = self->depth;
    copy->created_at = self->created_at;
    copy->settled_at = self->settled_at;

    return copy;
}

void
clawt_handoff_free(ClawtHandoff *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self->task_id);
    g_free(self->from_agent);
    g_free(self->to_agent);
    g_free(self->reason);
    g_free(self->room);
    g_free(self->verdict);
    g_free(self);
}

#define GETTER(name, field)                              \
    const gchar *                                        \
    clawt_handoff_get_##name(ClawtHandoff *self)         \
    {                                                    \
        g_return_val_if_fail(self != NULL, NULL);        \
        return self->field;                              \
    }

GETTER(id, id)
GETTER(task_id, task_id)
GETTER(from_agent, from_agent)
GETTER(to_agent, to_agent)
GETTER(reason, reason)
GETTER(room, room)
GETTER(verdict, verdict)

#undef GETTER

#define SETTER(name, field)                              \
    void                                                 \
    clawt_handoff_set_##name(ClawtHandoff *self,         \
                             const gchar  *value)        \
    {                                                    \
        g_return_if_fail(self != NULL);                  \
        g_free(self->field);                             \
        self->field = g_strdup(value);                   \
    }

SETTER(id, id)
SETTER(reason, reason)
SETTER(room, room)
SETTER(verdict, verdict)

#undef SETTER

ClawtHandoffState
clawt_handoff_get_state(ClawtHandoff *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_HANDOFF_ERROR);
    return self->state;
}

guint
clawt_handoff_get_attempts(ClawtHandoff *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->attempts;
}

gint
clawt_handoff_get_depth(ClawtHandoff *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->depth;
}

void
clawt_handoff_set_depth(ClawtHandoff *self, gint depth)
{
    g_return_if_fail(self != NULL);
    self->depth = depth;
}

gint64
clawt_handoff_get_created_at(ClawtHandoff *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->created_at;
}

gint64
clawt_handoff_get_settled_at(ClawtHandoff *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->settled_at;
}

void
clawt_handoff_set_attempts(ClawtHandoff *self, guint attempts)
{
    g_return_if_fail(self != NULL);
    self->attempts = attempts;
}

void
clawt_handoff_set_created_at(ClawtHandoff *self, gint64 created_at)
{
    g_return_if_fail(self != NULL);
    self->created_at = created_at;
}

void
clawt_handoff_set_settled_at(ClawtHandoff *self, gint64 settled_at)
{
    g_return_if_fail(self != NULL);
    self->settled_at = settled_at;
}

void
clawt_handoff_set_state(ClawtHandoff *self, ClawtHandoffState state)
{
    g_return_if_fail(self != NULL);

    self->state = state;

    /*
     * Stamped once.  A settle time that moved would make "how long did
     * this wait for a free agent" unanswerable, which is the only
     * question a busy-gave-up receipt exists to answer.
     */
    if (clawt_handoff_is_settled(self) && self->settled_at == 0)
        self->settled_at = g_get_real_time() / G_USEC_PER_SEC;
}

gboolean
clawt_handoff_is_settled(ClawtHandoff *self)
{
    g_return_val_if_fail(self != NULL, TRUE);

    /*
     * Named rather than defaulted.  A `default:` here would answer
     * "settled" for a state added later, and a queued handoff reported
     * as settled is a handoff that never runs and never says why --
     * -Wswitch catching the next value is the whole point.
     */
    switch (self->state) {
    case CLAWT_HANDOFF_QUEUED:
        return FALSE;

    case CLAWT_HANDOFF_DONE:
    case CLAWT_HANDOFF_FAILED:
    case CLAWT_HANDOFF_DENIED:
    case CLAWT_HANDOFF_BUSY_GAVE_UP:
    case CLAWT_HANDOFF_DROPPED:
    case CLAWT_HANDOFF_ERROR:
        return TRUE;
    }

    return TRUE;
}
