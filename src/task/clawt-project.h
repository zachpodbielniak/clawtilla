/*
 * clawt-project.h - Which project a piece of work belongs to
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A task belongs to at most one project (`projects:`), decided once when
 * it is made: the project its delegation or the operator named; else its
 * parent's, since a piece of a project's work is the project's work; else
 * the one whose directory its repository is -- the same path, or a
 * repository sharing one of its git remotes, compared by
 * clawt_git_remote_canonical() so an ssh clone and an https one match.
 */

#pragma once

#include <glib.h>

#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * clawt_project_resolve:
 * @config: the fleet
 * @named: (nullable): the project asked for by id; an unknown one is an
 *   error, never a guess
 * @parent_project: (nullable): the parent task's project
 * @repository: (nullable): the host path of the work's repository or
 *   directory, matched against each project's `directory`
 * @error: (out) (optional): return location for a #GError
 *
 * Since 0.40.0.
 *
 * Returns: (transfer full) (nullable): the project, or %NULL for none
 *   (with @error set only when @named is unknown)
 */
ClawtProjectSpec *clawt_project_resolve(ClawtConfig  *config,
                                        const gchar  *named,
                                        const gchar  *parent_project,
                                        const gchar  *repository,
                                        GError      **error);

/**
 * clawt_project_month_start:
 * @now: a Unix time
 *
 * Returns: the start of @now's calendar month, local time, as Unix time
 */
gint64 clawt_project_month_start(gint64 now);

G_END_DECLS
