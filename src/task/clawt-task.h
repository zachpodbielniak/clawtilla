/*
 * clawt-task.h - A unit of delegated work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What makes chief-of-staff orchestration observable rather than an
 * unbounded chat cascade: delegation creates a task, which can be listed,
 * followed and cancelled.
 *
 * Each task gets its own libreclaw session, so one job never contaminates
 * the next -- an agent that spent an hour on a refactor should not carry
 * that context into an unrelated question.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TASK (clawt_task_get_type())

GType clawt_task_get_type(void) G_GNUC_CONST;

ClawtTask *clawt_task_new(const gchar *origin_agent,
                          const gchar *assignee,
                          const gchar *prompt);

ClawtTask *clawt_task_copy(ClawtTask *self);
void       clawt_task_free(ClawtTask *self);

const gchar   *clawt_task_get_id(ClawtTask *self);
const gchar   *clawt_task_get_origin(ClawtTask *self);
const gchar   *clawt_task_get_assignee(ClawtTask *self);
const gchar   *clawt_task_get_prompt(ClawtTask *self);
const gchar   *clawt_task_get_result(ClawtTask *self);
const gchar   *clawt_task_get_room(ClawtTask *self);
const gchar   *clawt_task_get_parent_id(ClawtTask *self);
const gchar   *clawt_task_get_reason(ClawtTask *self);
const gchar   *clawt_task_get_session_key(ClawtTask *self);
ClawtTaskState clawt_task_get_state(ClawtTask *self);
gint           clawt_task_get_depth(ClawtTask *self);
gint64         clawt_task_get_created_at(ClawtTask *self);
gint64         clawt_task_get_finished_at(ClawtTask *self);

void clawt_task_set_room(ClawtTask *self, const gchar *room);
void clawt_task_set_parent_id(ClawtTask *self, const gchar *parent_id);
void clawt_task_set_reason(ClawtTask *self, const gchar *reason);
void clawt_task_set_result(ClawtTask *self, const gchar *result);
void clawt_task_set_state(ClawtTask *self, ClawtTaskState state);
void clawt_task_set_depth(ClawtTask *self, gint depth);

/**
 * clawt_task_is_finished:
 * @self: a #ClawtTask
 *
 * Returns: %TRUE if the task will not change state again
 */
gboolean clawt_task_is_finished(ClawtTask *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTask, clawt_task_free)

G_END_DECLS
