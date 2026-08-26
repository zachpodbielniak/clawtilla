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
 * A task does *not* get a libreclaw session of its own, though this said
 * so for a long time and clawt_task_new() still builds a session key
 * nothing reads.  lc_router_resolve_session_key() keys on channel, room
 * and sender and excludes the thread on purpose -- there a thread anchors
 * a reply rather than dividing a conversation -- so a task lands in the
 * sender's session and inherits whatever context is already in it.
 *
 * Isolating a job therefore means routing it into a room of its own,
 * which is what `routines.isolate` does.
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

/**
 * clawt_task_new:
 * @origin_agent: who delegated it
 * @assignee: who is to do it
 * @prompt: what they are to do
 *
 * Creates a task in the ~pending~ state.  Ordinary code goes through
 * clawt_task_manager_create(), which also enforces the depth limit and
 * records the parent.
 *
 * Returns: (transfer full): a new #ClawtTask
 */
ClawtTask *clawt_task_new(const gchar *origin_agent,
                          const gchar *assignee,
                          const gchar *prompt);

/**
 * clawt_task_copy:
 * @self: a #ClawtTask
 *
 * Returns: (transfer full): a copy
 */
ClawtTask *clawt_task_copy(ClawtTask *self);
void       clawt_task_free(ClawtTask *self);

/**
 * clawt_task_get_id:
 * @self: a #ClawtTask
 *
 * The accessors below are plain reads of what clawt_task_new() and the
 * setters put there.  Documented as a group because a line each saying
 * "returns the assignee" would be noise; the fields themselves are
 * described on #ClawtTask.
 *
 * Every string getter is (transfer none) and may be %NULL for the
 * optional fields -- room, parent, reason, result and session key are
 * all absent until something sets them.
 *
 * Returns: (transfer none): the task's identifier
 */
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

/**
 * clawt_task_set_room:
 * @self: a #ClawtTask
 * @room: (nullable): the room this task belongs to
 *
 * The setters below all take %NULL to clear the field.  They exist for
 * the manager and for reading a task back off the wire; ordinary code
 * goes through #ClawtTaskManager, which enforces the state machine.
 */
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
