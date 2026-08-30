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
    gchar *progress_note;

    /*
     * Everyone who has owned it, oldest first, with the current
     * assignee always last.  One array rather than a "previous owner"
     * field: a task can be handed on twice, and the second handoff would
     * have overwritten the record of the first.
     */
    GPtrArray *owners;

    /*
     * Two facts about how a task ends, kept apart because they answer
     * different questions.
     *
     * holds_completion is the assignee saying "my turn is over and the
     * work is not", set by clawt_task_manager_note_progress() and spent
     * by the next clawt_task_manager_complete_on_turn_end().  One-shot,
     * because a hold that persisted would mean the task could never
     * finish by inference again -- and the assignee is the one who knows,
     * so it says so each turn it is still going.
     *
     * result_inferred records that nothing ever called
     * clawtilla_task_complete: the result is whatever the assignee
     * happened to write as its turn ended.  A delegator cannot otherwise
     * tell "they said it was done" from "they stopped talking", and the
     * two need different follow-ups.
     */
    gboolean holds_completion;
    gboolean result_inferred;

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
     * Seeded with the first assignee rather than left empty, so the
     * history and clawt_task_get_assignee() can never disagree about
     * who owns it now: the answer is always the last entry.
     */
    self->owners = g_ptr_array_new_with_free_func(g_free);

    if (assignee != NULL)
        g_ptr_array_add(self->owners, g_strdup(assignee));

    /*
     * What a session key *would* be if a task had a session of its own.
     *
     * It does not.  Nothing outside a test has ever read this: libreclaw
     * keys a session on channel, room and sender, and clawtilla delivers
     * a task in the room the delegator and the assignee already share --
     * so every task on an agent runs in that one session, and this
     * string reaches nobody.  Kept because it is what the field would
     * have to be, and removing a public getter is a break; the comment
     * that used to sit here asserted the isolation as fact and was the
     * only reason anybody believed it.
     *
     * Delivering it means routing a task into a room named for it. That
     * moves a routine's output out of the operator's transcript, which
     * is a decision rather than a repair.
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
    copy->progress_note = g_strdup(self->progress_note);

    /*
     * Deep, because g_ptr_array_copy() carries the source's element-free
     * func and the copy would then own every string twice.
     */
    copy->owners = g_ptr_array_new_with_free_func(g_free);

    {
        guint i;

        for (i = 0; self->owners != NULL && i < self->owners->len; i++)
            g_ptr_array_add(copy->owners,
                            g_strdup(g_ptr_array_index(self->owners, i)));
    }

    copy->holds_completion = self->holds_completion;
    copy->result_inferred = self->result_inferred;
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
    g_free(self->progress_note);
    g_clear_pointer(&self->owners, g_ptr_array_unref);
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
GETTER(progress_note, progress_note)

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
SETTER(progress_note, progress_note)

#undef SETTER

void
clawt_task_hold_completion(ClawtTask *self)
{
    g_return_if_fail(self != NULL);

    self->holds_completion = TRUE;
}

gboolean
clawt_task_take_completion_hold(ClawtTask *self)
{
    gboolean held;

    g_return_val_if_fail(self != NULL, FALSE);

    held = self->holds_completion;
    self->holds_completion = FALSE;

    return held;
}

gboolean
clawt_task_get_result_inferred(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->result_inferred;
}

void
clawt_task_set_result_inferred(ClawtTask *self, gboolean inferred)
{
    g_return_if_fail(self != NULL);

    self->result_inferred = inferred;
}

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

GPtrArray *
clawt_task_get_owner_history(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->owners;
}

gboolean
clawt_task_transfer_owner(ClawtTask *self, const gchar *new_owner)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(new_owner != NULL, FALSE);

    /*
     * Handing a task to whoever already has it is not a move.  Recorded
     * as one it would read as a round trip that never happened, and a
     * chief reading its own history would conclude the work had bounced.
     */
    if (g_strcmp0(self->assignee, new_owner) == 0)
        return FALSE;

    g_free(self->assignee);
    self->assignee = g_strdup(new_owner);

    g_ptr_array_add(self->owners, g_strdup(new_owner));

    return TRUE;
}

gboolean
clawt_task_is_finished(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, TRUE);

    return self->state == CLAWT_TASK_COMPLETED ||
           self->state == CLAWT_TASK_FAILED ||
           self->state == CLAWT_TASK_CANCELLED ||
           self->state == CLAWT_TASK_STALLED;
}

const gchar *
clawt_task_state_tone(ClawtTaskState state)
{
    /*
     * No `default:`, deliberately. -Wswitch then names a state nobody
     * classified -- a warning rather than an error, since this tree does
     * not build with -Werror, so the zero-warning rule is what makes it
     * stop somebody; tests/test-task.c is what actually goes red.
     *
     * Worth the belt and the braces, because the string comparison this
     * replaces named two nicknames the enum has never had, and both
     * clients rendered the result without complaint.
     */
    switch (state) {
    case CLAWT_TASK_COMPLETED:
        return "good";

    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_CANCELLED:
        return "bad";

    case CLAWT_TASK_STALLED:
        return "warn";

    case CLAWT_TASK_RUNNING:
        return "info";

    case CLAWT_TASK_PENDING:
        return "neutral";
    }

    /*
     * Only reachable from an integer cast to the type, which is a
     * programming error rather than a state.
     */
    g_return_val_if_reached("neutral");
}
