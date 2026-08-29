/*
 * clawt-task-manager.h - Every delegated task
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

#include "clawt-types.h"
#include "task/clawt-task.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TASK_MANAGER (clawt_task_manager_get_type())

G_DECLARE_FINAL_TYPE(ClawtTaskManager, clawt_task_manager,
                     CLAWT, TASK_MANAGER, GObject)

/**
 * clawt_task_manager_new:
 *
 * Tasks live in memory: they are work in flight, and work in flight does
 * not survive the daemon that was doing it.  What did happen is in the
 * event log.
 *
 * Returns: (transfer full): a new #ClawtTaskManager
 */
ClawtTaskManager *clawt_task_manager_new(void);

/**
 * clawt_task_manager_create:
 * @self: a #ClawtTaskManager
 * @origin_agent: who delegated it
 * @assignee: who is to do it
 * @prompt: what to do
 * @parent_id: (nullable): the task this one was spawned from
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a task, inheriting depth from its parent.
 *
 * Returns: (transfer none) (nullable): the task, or %NULL if it would be
 *   too deep
 */
ClawtTask *clawt_task_manager_create(ClawtTaskManager  *self,
                                     const gchar       *origin_agent,
                                     const gchar       *assignee,
                                     const gchar       *prompt,
                                     const gchar       *parent_id,
                                     GError           **error);

/**
 * clawt_task_manager_get:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 *
 * Returns: (transfer none) (nullable): the task, or %NULL
 */
ClawtTask *clawt_task_manager_get(ClawtTaskManager *self,
                                  const gchar      *task_id);

/**
 * clawt_task_manager_list:
 * @self: a #ClawtTaskManager
 * @assignee: (nullable): only this agent's tasks, or %NULL for all
 * @include_finished: whether to include tasks that have ended
 *
 * Returns: (transfer container) (element-type ClawtTask): matching tasks,
 *   newest first
 */
GPtrArray *clawt_task_manager_list(ClawtTaskManager *self,
                                   const gchar      *assignee,
                                   gboolean          include_finished);

/**
 * clawt_task_manager_list_involving:
 * @self: a #ClawtTaskManager
 * @agent_id: the agent whose tasks are wanted
 * @include_finished: whether to include tasks that have ended
 *
 * Every task @agent_id delegated **or** was assigned, newest first.
 *
 * Both roles, because "my tasks" means both to the agent asking and the
 * two are answered from the same place.  clawt_task_manager_list() takes
 * an assignee alone, which is the operator's question -- an agent that
 * handed work out is not the assignee of any of it, so filtering that
 * way returns nothing to the caller who most needs an answer.
 *
 * The caller tells the two apart by comparing the origin itself; that is
 * presentation rather than filtering.
 *
 * Returns: (transfer container) (element-type ClawtTask): matching tasks
 */
GPtrArray *clawt_task_manager_list_involving(ClawtTaskManager *self,
                                             const gchar      *agent_id,
                                             gboolean          include_finished);

/**
 * clawt_task_manager_complete:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @result: what it produced
 *
 * Returns: %TRUE if the task existed and was running
 */
gboolean clawt_task_manager_complete(ClawtTaskManager *self,
                                     const gchar      *task_id,
                                     const gchar      *result);

/**
 * clawt_task_manager_fail:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @reason: what went wrong
 *
 * Returns: %TRUE if the task existed and had not already ended
 */
gboolean clawt_task_manager_fail(ClawtTaskManager *self,
                                 const gchar      *task_id,
                                 const gchar      *reason);

/**
 * clawt_task_manager_cancel:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 * @reason: (nullable): why
 *
 * Cancels a task and everything it spawned.
 *
 * Cancelling only the parent would leave its children running and reporting
 * into a task nobody is waiting for -- which is exactly the runaway
 * cancellation is meant to stop.
 *
 * Returns: how many tasks were cancelled
 */
guint clawt_task_manager_cancel(ClawtTaskManager *self,
                                const gchar      *task_id,
                                const gchar      *reason);

/**
 * clawt_task_manager_start:
 * @self: a #ClawtTaskManager
 * @task_id: a task id
 *
 * Marks a task as being worked on.
 *
 * Returns: %TRUE if it was pending
 */
gboolean clawt_task_manager_start(ClawtTaskManager *self,
                                  const gchar      *task_id);

/**
 * clawt_task_manager_orphan_agent_tasks:
 * @self: a #ClawtTaskManager
 * @agent_id: an agent that has gone away
 *
 * Fails the running tasks of an agent that has stopped.
 *
 * Without this a delegator waits for a result that can never arrive,
 * because the agent that owed it no longer exists.
 *
 * Returns: how many tasks were failed
 */
guint clawt_task_manager_orphan_agent_tasks(ClawtTaskManager *self,
                                            const gchar      *agent_id);

/**
 * clawt_task_manager_set_max_depth:
 * @self: a #ClawtTaskManager
 * @max_depth: how deep delegation may nest
 */
void clawt_task_manager_set_max_depth(ClawtTaskManager *self,
                                      guint             max_depth);

G_END_DECLS
