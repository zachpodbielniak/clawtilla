/*
 * clawt-task.c - A unit of delegated work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-task.h"

struct _ClawtTask {
    gint ref_count;

    gchar *id;
    gchar *origin;
    gchar *assignee;
    gchar *prompt;
    gchar *result;
    gchar *room;
    gchar *parent_id;
    gchar *reason;
    gchar *session_key;

    ClawtTaskState state;
    gint           depth;
    gint64         created_at;
    gint64         finished_at;
};

static ClawtTask *
clawt_task_ref(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtTask, clawt_task, clawt_task_ref, clawt_task_free)

ClawtTask *
clawt_task_new(const gchar *origin_agent,
               const gchar *assignee,
               const gchar *prompt)
{
    ClawtTask *self;

    g_return_val_if_fail(prompt != NULL, NULL);

    self = g_new0(ClawtTask, 1);
    self->ref_count = 1;
    self->id = clawt_generate_id("task");
    self->origin = g_strdup(origin_agent);
    self->assignee = g_strdup(assignee);
    self->prompt = g_strdup(prompt);
    self->state = CLAWT_TASK_PENDING;
    self->created_at = g_get_real_time() / G_USEC_PER_SEC;

    /*
     * A session key derived from the task id, so each task gets its own
     * libreclaw session and one job never contaminates the next.  Deriving
     * rather than storing separately means the two cannot drift apart.
     */
    self->session_key = g_strdup_printf("clawtilla-task-%s", self->id);

    return self;
}

ClawtTask *
clawt_task_copy(ClawtTask *self)
{
    ClawtTask *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtTask, 1);
    copy->ref_count = 1;
    copy->id = g_strdup(self->id);
    copy->origin = g_strdup(self->origin);
    copy->assignee = g_strdup(self->assignee);
    copy->prompt = g_strdup(self->prompt);
    copy->result = g_strdup(self->result);
    copy->room = g_strdup(self->room);
    copy->parent_id = g_strdup(self->parent_id);
    copy->reason = g_strdup(self->reason);
    copy->session_key = g_strdup(self->session_key);
    copy->state = self->state;
    copy->depth = self->depth;
    copy->created_at = self->created_at;
    copy->finished_at = self->finished_at;

    return copy;
}

void
clawt_task_free(ClawtTask *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self->origin);
    g_free(self->assignee);
    g_free(self->prompt);
    g_free(self->result);
    g_free(self->room);
    g_free(self->parent_id);
    g_free(self->reason);
    g_free(self->session_key);
    g_free(self);
}

#define GETTER(name, field)                              \
    const gchar *                                        \
    clawt_task_get_##name(ClawtTask *self)               \
    {                                                    \
        g_return_val_if_fail(self != NULL, NULL);        \
        return self->field;                              \
    }

GETTER(id, id)
GETTER(origin, origin)
GETTER(assignee, assignee)
GETTER(prompt, prompt)
GETTER(result, result)
GETTER(room, room)
GETTER(parent_id, parent_id)
GETTER(reason, reason)
GETTER(session_key, session_key)

#undef GETTER

#define SETTER(name, field)                              \
    void                                                 \
    clawt_task_set_##name(ClawtTask *self,               \
                          const gchar *value)            \
    {                                                    \
        g_return_if_fail(self != NULL);                  \
        g_free(self->field);                             \
        self->field = g_strdup(value);                   \
    }

SETTER(room, room)
SETTER(parent_id, parent_id)
SETTER(reason, reason)
SETTER(result, result)

#undef SETTER

ClawtTaskState
clawt_task_get_state(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_TASK_FAILED);
    return self->state;
}

gint
clawt_task_get_depth(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->depth;
}

gint64
clawt_task_get_created_at(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->created_at;
}

gint64
clawt_task_get_finished_at(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->finished_at;
}

void
clawt_task_set_state(ClawtTask *self, ClawtTaskState state)
{
    g_return_if_fail(self != NULL);

    self->state = state;

    /*
     * Stamped once, when the task reaches a state it will not leave.  A
     * finish time that moved would make "how long did this take" unanswerable.
     */
    if (clawt_task_is_finished(self) && self->finished_at == 0)
        self->finished_at = g_get_real_time() / G_USEC_PER_SEC;
}

void
clawt_task_set_depth(ClawtTask *self, gint depth)
{
    g_return_if_fail(self != NULL);
    self->depth = depth;
}

gboolean
clawt_task_is_finished(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, TRUE);

    return self->state == CLAWT_TASK_COMPLETED ||
           self->state == CLAWT_TASK_FAILED ||
           self->state == CLAWT_TASK_CANCELLED;
}
