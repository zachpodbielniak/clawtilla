/*
 * clawt-task-sources.h - Which routine or trigger run a task belongs to
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

#include "usage/clawt-usage-ledger.h"

G_BEGIN_DECLS

/**
 * CLAWT_TASK_SOURCES_MAX_AGE_SECONDS:
 *
 * How long an entry for a task this daemon no longer knows is kept: a
 * week.  A task can leave the manager -- pruned after the retention
 * window, or never restored because the task store would not open --
 * while a turn about it can still arrive, from a mailbox item that
 * outlived it.  A week covers a
 * stopped agent and a long weekend without letting a five-minute
 * routine's entries grow for ever.
 */
#define CLAWT_TASK_SOURCES_MAX_AGE_SECONDS (7 * 24 * 60 * 60)

#define CLAWT_TYPE_TASK_SOURCES (clawt_task_sources_get_type())

G_DECLARE_FINAL_TYPE(ClawtTaskSources, clawt_task_sources,
		     CLAWT, TASK_SOURCES, GObject)

/**
 * ClawtTaskSourcesLiveFunc:
 * @task_id: a task id with an entry
 * @known: (out): %TRUE when the caller knows the task at all
 * @user_data: (closure): what was passed to clawt_task_sources_prune()
 *
 * Asked by clawt_task_sources_prune() about each entry.
 *
 * Returns: %TRUE while the task is known and has not finished
 */
typedef gboolean (*ClawtTaskSourcesLiveFunc)(const gchar *task_id,
					     gboolean    *known,
					     gpointer     user_data);

/**
 * clawt_task_sources_new:
 * @path: (nullable): the file the table is kept in, or %NULL to keep it
 *   in memory only
 *
 * Loads the table from @path when it exists.  A file that cannot be read
 * is logged and the table starts empty: losing a cost attribution is a
 * worse ledger, not a reason for the daemon not to start.
 *
 * Returns: (transfer full): a new #ClawtTaskSources
 */
ClawtTaskSources *clawt_task_sources_new(const gchar *path);

/**
 * clawt_task_sources_note:
 * @self: a #ClawtTaskSources
 * @task_id: the task a run created, or a task found to belong to one
 * @kind: %CLAWT_USAGE_ORIGIN_ROUTINE or %CLAWT_USAGE_ORIGIN_TRIGGER
 * @id: the routine or trigger id
 *
 * Records which run @task_id belongs to and writes the table out at
 * once, so a run that spans a daemon restart is still charged to its
 * routine or trigger rather than to `task:<id>`.  A @task_id or @id
 * holding a tab or a line break is refused, since neither is an id this
 * tree makes.
 *
 * Returns: %TRUE when it was recorded (in memory; a failed write is
 *   logged)
 */
gboolean clawt_task_sources_note(ClawtTaskSources     *self,
				 const gchar          *task_id,
				 ClawtUsageOriginKind  kind,
				 const gchar          *id);

/**
 * clawt_task_sources_lookup:
 * @self: a #ClawtTaskSources
 * @task_id: (nullable): a task id
 *
 * The run @task_id itself was recorded against -- not its ancestors;
 * the caller walks the parent chain, since only it knows the tasks.
 *
 * Returns: (transfer none) (nullable): the origin, kind and id only, or
 *   %NULL
 */
const ClawtUsageOrigin *clawt_task_sources_lookup(ClawtTaskSources *self,
						  const gchar      *task_id);

/**
 * clawt_task_sources_get_count:
 * @self: a #ClawtTaskSources
 *
 * Returns: how many entries the table holds
 */
guint clawt_task_sources_get_count(ClawtTaskSources *self);

/**
 * clawt_task_sources_prune:
 * @self: a #ClawtTaskSources
 * @is_live: (scope call): asked about each entry
 * @user_data: passed to @is_live
 * @now: unix seconds to measure age against
 *
 * Drops the entries that can no longer be asked about: a task the caller
 * knows and that has finished, and a task the caller does not know whose
 * entry is older than %CLAWT_TASK_SOURCES_MAX_AGE_SECONDS.  An unknown
 * task is not dropped merely for being unknown -- after a restart that is
 * every entry, which is the case the table exists for.
 *
 * Returns: how many entries were dropped
 */
guint clawt_task_sources_prune(ClawtTaskSources         *self,
			       ClawtTaskSourcesLiveFunc  is_live,
			       gpointer                  user_data,
			       gint64                    now);

G_END_DECLS
