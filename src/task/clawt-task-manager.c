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

GPtrArray *
clawt_task_manager_list(ClawtTaskManager *self,
                        const gchar      *assignee,
                        gboolean          include_finished)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

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

        if (!include_finished && clawt_task_is_finished(task))
            continue;

        g_ptr_array_add(out, task);
    }

    return out;
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

guint
clawt_task_manager_cancel(ClawtTaskManager *self,
                          const gchar      *task_id,
                          const gchar      *reason)
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

        cancelled += clawt_task_manager_cancel(self, child_id, reason);
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
