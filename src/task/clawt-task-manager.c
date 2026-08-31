/*
 * clawt-task-manager.c - Every delegated task
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-task-manager.h"

enum {
    SIGNAL_TASK_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtTaskManager {
    GObject parent_instance;

    GHashTable *tasks;    /* task_id -> ClawtTask (owned) */
    GPtrArray  *order;    /* task ids, oldest first, unowned */
    guint       max_depth;
};

G_DEFINE_FINAL_TYPE(ClawtTaskManager, clawt_task_manager, G_TYPE_OBJECT)

ClawtTaskManager *
clawt_task_manager_new(void)
{
    return g_object_new(CLAWT_TYPE_TASK_MANAGER, NULL);
}

void
clawt_task_manager_set_max_depth(ClawtTaskManager *self, guint max_depth)
{
    g_return_if_fail(CLAWT_IS_TASK_MANAGER(self));

    self->max_depth = max_depth;
}

static void
emit_changed(ClawtTaskManager *self, ClawtTask *task)
{
    g_signal_emit(self, signals[SIGNAL_TASK_CHANGED], 0,
                  clawt_task_get_id(task), clawt_task_get_state(task));
}

ClawtTask *
clawt_task_manager_create(ClawtTaskManager  *self,
                          const gchar       *origin_agent,
                          const gchar       *assignee,
                          const gchar       *prompt,
                          const gchar       *parent_id,
                          GError           **error)
{
    ClawtTask *task;
    gint depth = 0;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(prompt != NULL, NULL);

    if (parent_id != NULL) {
        ClawtTask *parent = g_hash_table_lookup(self->tasks, parent_id);

        if (parent != NULL)
            depth = clawt_task_get_depth(parent) + 1;
    }

    /*
     * Depth is bounded here as well as on messages, because delegation can
     * nest without any message hop: a task spawning a task spawning a task
     * is three levels deep and each message in it is one hop.
     */
    if (self->max_depth > 0 && depth >= (gint)self->max_depth) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "delegation is already %d levels deep and the limit is "
                    "%u. Do this yourself rather than delegating again.",
                    depth, self->max_depth);
        return NULL;
    }

    task = clawt_task_new(origin_agent, assignee, prompt);
    clawt_task_set_depth(task, depth);
    clawt_task_set_parent_id(task, parent_id);

    g_hash_table_insert(self->tasks, g_strdup(clawt_task_get_id(task)), task);
    g_ptr_array_add(self->order, (gpointer)clawt_task_get_id(task));

    emit_changed(self, task);

    return task;
}

ClawtTask *
clawt_task_manager_get(ClawtTaskManager *self, const gchar *task_id)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    if (task_id == NULL)
        return NULL;

    return g_hash_table_lookup(self->tasks, task_id);
}

/*
 * One loop behind both public listings.  They differ only in which
 * field an agent id is compared against, and two copies of "newest
 * first, skip the finished ones" would differ exactly once.
 */
static GPtrArray *
list_filtered(ClawtTaskManager *self,
              const gchar      *assignee,
              const gchar      *involving,
              gboolean          include_finished)
{
    GPtrArray *out;
    guint i;

    out = g_ptr_array_new();

    /* Newest first: the thing somebody just asked about is at the top. */
    for (i = self->order->len; i > 0; i--) {
        const gchar *task_id = g_ptr_array_index(self->order, i - 1);
        ClawtTask *task = g_hash_table_lookup(self->tasks, task_id);

        if (task == NULL)
            continue;

        if (assignee != NULL &&
            g_strcmp0(clawt_task_get_assignee(task), assignee) != 0)
            continue;

        if (involving != NULL &&
            g_strcmp0(clawt_task_get_assignee(task), involving) != 0 &&
            g_strcmp0(clawt_task_get_origin(task), involving) != 0)
            continue;

        if (!include_finished && clawt_task_is_finished(task))
            continue;

        g_ptr_array_add(out, task);
    }

    return out;
}

GPtrArray *
clawt_task_manager_list(ClawtTaskManager *self,
                        const gchar      *assignee,
                        gboolean          include_finished)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    return list_filtered(self, assignee, NULL, include_finished);
}

GPtrArray *
clawt_task_manager_list_involving(ClawtTaskManager *self,
                                  const gchar      *agent_id,
                                  gboolean          include_finished)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    return list_filtered(self, NULL, agent_id, include_finished);
}

gboolean
clawt_task_manager_start(ClawtTaskManager *self, const gchar *task_id)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_get_state(task) != CLAWT_TASK_PENDING)
        return FALSE;

    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    emit_changed(self, task);

    return TRUE;
}

gboolean
clawt_task_manager_complete(ClawtTaskManager *self,
                            const gchar      *task_id,
                            const gchar      *result)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL)
        return FALSE;

    /*
     * A task that already ended stays ended.  A late result arriving after
     * a cancellation must not quietly un-cancel it -- somebody stopped that
     * work on purpose.
     */
    if (clawt_task_is_finished(task))
        return FALSE;

    clawt_task_set_result(task, result);
    clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
    emit_changed(self, task);

    return TRUE;
}

/*
 * How many tasks this one handed on that have not ended.
 *
 * Direct children only, and that is enough by induction: a child cannot
 * finish while a child of its own is running, so an unfinished
 * grandchild keeps its parent unfinished and this sees the parent.  The
 * one gap is a branch that was cancelled or failed out from under a
 * still-running grandchild, and cancellation already cascades.
 */
guint
clawt_task_manager_count_unfinished_children(ClawtTaskManager *self,
                                             const gchar      *task_id)
{
    guint unfinished = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);

    if (task_id == NULL)
        return 0;

    for (i = 0; i < self->order->len; i++) {
        const gchar *child_id = g_ptr_array_index(self->order, i);
        ClawtTask *child = g_hash_table_lookup(self->tasks, child_id);

        if (child == NULL || clawt_task_is_finished(child))
            continue;

        if (g_strcmp0(clawt_task_get_parent_id(child), task_id) == 0)
            unfinished++;
    }

    return unfinished;
}

/*
 * Whether anything above @task was delegated by @agent_id.
 *
 * Bounded rather than trusted to terminate.  A parent always exists
 * before its child, so the chain cannot loop -- but a walk that hangs
 * the daemon is a worse answer than one that gives up, and this runs on
 * the main context while a client blocks.
 */
static gboolean
has_ancestor_from(ClawtTaskManager *self,
                  ClawtTask        *task,
                  const gchar      *agent_id)
{
    const gchar *walk = clawt_task_get_parent_id(task);
    guint steps;

    for (steps = 0; walk != NULL && steps < 1024; steps++) {
        ClawtTask *parent = g_hash_table_lookup(self->tasks, walk);

        if (parent == NULL)
            return FALSE;

        if (g_strcmp0(clawt_task_get_origin(parent), agent_id) == 0)
            return TRUE;

        walk = clawt_task_get_parent_id(parent);
    }

    if (walk != NULL)
        g_warning("task %s: parent chain is longer than 1024 tasks; giving "
                  "up walking it", clawt_task_get_id(task));

    return FALSE;
}

/*
 * The fan-out below the tasks this agent handed out.
 *
 * clawt_task_manager_list_involving() answers "what am I a party to",
 * which for a chief-of-staff stops one level down: it sees the task it
 * gave a lead and nothing the lead gave anybody.  So a fan-out was
 * invisible from the only place anybody was watching -- the parent read
 * `completed`, the children could not be listed at all, and nothing
 * distinguished finished from never-started.
 *
 * Kept separate from _list_involving() rather than folded into it,
 * because they are different claims: one is work this agent is doing or
 * waiting on, the other is work its work turned into.  A caller that
 * merged them would report somebody else's task as its own.
 */
GPtrArray *
clawt_task_manager_list_descendants(ClawtTaskManager *self,
                                    const gchar      *agent_id,
                                    gboolean          include_finished)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    out = g_ptr_array_new();

    for (i = self->order->len; i > 0; i--) {
        const gchar *task_id = g_ptr_array_index(self->order, i - 1);
        ClawtTask *task = g_hash_table_lookup(self->tasks, task_id);

        if (task == NULL)
            continue;

        if (!include_finished && clawt_task_is_finished(task))
            continue;

        /*
         * What the agent is itself a party to belongs to the other
         * listing.  Returning it here as well would have a chief seeing
         * its own delegation twice under two headings that mean
         * different things.
         */
        if (g_strcmp0(clawt_task_get_origin(task), agent_id) == 0 ||
            g_strcmp0(clawt_task_get_assignee(task), agent_id) == 0)
            continue;

        if (has_ancestor_from(self, task, agent_id))
            g_ptr_array_add(out, task);
    }

    return out;
}

/*
 * The assignee saying its turn is over and the work is not.
 *
 * Also marks the task running, because it is proof somebody picked it
 * up: clawtilla_delegate does not, so an agent-delegated task otherwise
 * reads `pending` for its whole life and a delegator that takes that
 * literally delegates it again.
 */
gboolean
clawt_task_manager_note_progress(ClawtTaskManager *self,
                                 const gchar      *task_id,
                                 const gchar      *note)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_is_finished(task))
        return FALSE;

    clawt_task_set_progress_note(task, note);
    clawt_task_hold_completion(task);

    if (clawt_task_get_state(task) == CLAWT_TASK_PENDING)
        clawt_task_set_state(task, CLAWT_TASK_RUNNING);

    emit_changed(self, task);

    return TRUE;
}

/*
 * Completing a task because its assignee's turn ended.
 *
 * An inference, not a report: an AI CLI cannot end a turn without
 * writing something, so the last thing it wrote is all there is to go
 * on.  It is right for the ordinary case -- work, answer, done -- and
 * wrong for every assignee that batches, which is the behaviour the rest
 * of the guidance asks for.  So the two ways it can be wrong are checked
 * here, once, rather than at whichever call site last noticed:
 *
 *   - the assignee said so, through clawtilla_task_progress;
 *   - the task has work outstanding that it handed on itself.
 *
 * The second could not fire at all until clawtilla_delegate started
 * recording a parent: every agent-delegated task was a root, so a fan-out
 * had no children to find and the parent closed over the top of them.
 *
 * A task that does end this way is marked as having been inferred, so
 * clawtilla_task_result can say which of "they said it was done" and
 * "they stopped talking" it is looking at.
 */
gboolean
clawt_task_manager_complete_on_turn_end(ClawtTaskManager  *self,
                                        const gchar       *task_id,
                                        const gchar       *who,
                                        const gchar       *result,
                                        gchar            **held_reason)
{
    ClawtTask *task;
    guint unfinished;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);
    g_return_val_if_fail(who != NULL, FALSE);

    if (held_reason != NULL)
        *held_reason = NULL;

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_is_finished(task))
        return FALSE;

    /*
     * Only the assignee's turn says anything about the work.  Everyone
     * in a task's thread ends turns there -- a delegator acknowledging a
     * progress note ends one -- and completing on whichever turn ended
     * next recorded "Thanks, carry on" as the result of work that was
     * still running.  Before the hold check, because the hold check
     * *consumes* the hold: a delegator's turn ending in the thread must
     * not spend what the assignee armed against its own next one.
     */
    if (g_strcmp0(clawt_task_get_assignee(task), who) != 0) {
        if (held_reason != NULL)
            *held_reason = g_strdup_printf(
                "the turn was %s's, and only its assignee %s can end it",
                who, clawt_task_get_assignee(task));

        return FALSE;
    }

    if (clawt_task_take_completion_hold(task)) {
        if (held_reason != NULL)
            *held_reason = g_strdup("its assignee reported progress rather "
                                    "than a result");

        /*
         * The note is deliberately *not* overwritten here.  The assignee
         * has just chosen words for this exact purpose through
         * clawtilla_task_progress; @result is whatever an AI CLI wrote
         * to end its turn, which is often "I will check back shortly".
         * Replacing the first with the second is a strictly worse answer
         * for the delegator reading it.
         */
        emit_changed(self, task);

        return FALSE;
    }

    unfinished = clawt_task_manager_count_unfinished_children(self, task_id);

    if (unfinished > 0) {
        if (held_reason != NULL)
            *held_reason = g_strdup_printf(
                "%u task%s it handed on %s still running", unfinished,
                unfinished == 1 ? "" : "s", unfinished == 1 ? "is" : "are");

        /*
         * And here it is recorded, because nothing else did: a task held
         * open by its own fan-out has no deliberate note, so the end of
         * the turn is the freshest thing anybody knows about it.
         */
        clawt_task_set_progress_note(task, result);
        emit_changed(self, task);

        return FALSE;
    }

    /*
     * Marked before the completion, not after: clawt_task_manager_complete()
     * emits ::task-changed, and a client that reads the task from that
     * signal would otherwise see the flag still clear on the one
     * notification that tells it the task ended.
     */
    clawt_task_set_result_inferred(task, TRUE);

    if (!clawt_task_manager_complete(self, task_id, result)) {
        clawt_task_set_result_inferred(task, FALSE);
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_task_manager_fail(ClawtTaskManager *self,
                        const gchar      *task_id,
                        const gchar      *reason)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_is_finished(task))
        return FALSE;

    clawt_task_set_reason(task, reason);
    clawt_task_set_state(task, CLAWT_TASK_FAILED);
    emit_changed(self, task);

    return TRUE;
}

/*
 * A task the loop guard ended.
 *
 * Its own verb rather than clawt_task_manager_fail(), because "the two
 * agents went in circles and clawtilla stopped them" and "the work broke"
 * need different answers from whoever reads the receipt: the first is
 * work somebody can pick up and finish, the second needs diagnosing.
 * Recording a stall as a failure sends the reader hunting a bug that is
 * not there.
 */
gboolean
clawt_task_manager_stall(ClawtTaskManager *self,
                         const gchar      *task_id,
                         const gchar      *reason)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_is_finished(task))
        return FALSE;

    clawt_task_set_reason(task, reason);
    clawt_task_set_state(task, CLAWT_TASK_STALLED);
    emit_changed(self, task);

    return TRUE;
}

guint
clawt_task_manager_cancel(ClawtTaskManager *self,
                          const gchar      *task_id,
                          const gchar      *reason,
                          const gchar      *who)
{
    ClawtTask *task;
    guint cancelled = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);

    task = clawt_task_manager_get(self, task_id);
    if (task == NULL)
        return 0;

    if (!clawt_task_is_finished(task)) {
        clawt_task_set_reason(task, reason);

        /*
         * Before the state, so the ::task-changed handler that turns a
         * terminal transition into a settle notice can already see who
         * did it -- a delegator is not told about its own cancel, and
         * the state emission is the one moment that decision is made.
         */
        clawt_task_set_cancelled_by(task, who);
        clawt_task_set_state(task, CLAWT_TASK_CANCELLED);
        emit_changed(self, task);
        cancelled++;
    }

    /*
     * And everything it spawned.  Cancelling only the parent leaves its
     * children running and reporting into a task nobody is waiting for --
     * which is the runaway cancellation exists to stop.
     */
    for (i = 0; i < self->order->len; i++) {
        const gchar *child_id = g_ptr_array_index(self->order, i);
        ClawtTask *child = g_hash_table_lookup(self->tasks, child_id);

        if (child == NULL)
            continue;

        if (g_strcmp0(clawt_task_get_parent_id(child), task_id) != 0)
            continue;

        /*
         * The original canceller, all the way down.  A cascade-cancelled
         * child's delegator is usually somewhere inside the chain being
         * cancelled -- and it is exactly the party that must stop
         * waiting, so the notice logic needs to see that somebody
         * *else* ended this one.
         */
        cancelled += clawt_task_manager_cancel(self, child_id, reason, who);
    }

    return cancelled;
}

guint
clawt_task_manager_orphan_agent_tasks(ClawtTaskManager *self,
                                      const gchar      *agent_id)
{
    guint failed = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);
    g_return_val_if_fail(agent_id != NULL, 0);

    for (i = 0; i < self->order->len; i++) {
        const gchar *task_id = g_ptr_array_index(self->order, i);
        ClawtTask *task = g_hash_table_lookup(self->tasks, task_id);

        if (task == NULL || clawt_task_is_finished(task))
            continue;

        if (g_strcmp0(clawt_task_get_assignee(task), agent_id) != 0)
            continue;

        /*
         * Failed with a reason rather than silently dropped.  Whoever
         * delegated it is waiting, and "the agent stopped" is the answer
         * they need.
         */
        if (clawt_task_manager_fail(self, task_id,
                                    "the agent handling this stopped before "
                                    "finishing"))
            failed++;
    }

    return failed;
}

static void
clawt_task_manager_finalize(GObject *object)
{
    ClawtTaskManager *self = CLAWT_TASK_MANAGER(object);

    g_clear_pointer(&self->order, g_ptr_array_unref);
    g_clear_pointer(&self->tasks, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_task_manager_parent_class)->finalize(object);
}

static void
clawt_task_manager_class_init(ClawtTaskManagerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_task_manager_finalize;

    /**
     * ClawtTaskManager::task-changed:
     * @self: the manager
     * @task_id: which task
     * @state: its new state
     */
    signals[SIGNAL_TASK_CHANGED] =
        g_signal_new("task-changed", CLAWT_TYPE_TASK_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);
}

static void
clawt_task_manager_init(ClawtTaskManager *self)
{
    self->tasks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify)clawt_task_free);

    /* Ids are borrowed from the tasks, which own them and outlive this. */
    self->order = g_ptr_array_new();
    self->max_depth = 8;
}
