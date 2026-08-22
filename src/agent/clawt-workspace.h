/*
 * clawt-workspace.h - The standard file set in an agent's workspace
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

#include <glib.h>

#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * ClawtWorkspaceFile:
 * @name: the file's name in the workspace
 * @title: one line for a listing
 * @identity: %TRUE when it belongs in the system prompt
 *
 * One entry in the standard workspace file set.
 */
typedef struct {
    const gchar *name;
    const gchar *title;
    gboolean     identity;
} ClawtWorkspaceFile;

/**
 * clawt_workspace_files:
 * @n_files: (out) (optional): how many
 *
 * The standard file set, in the order an agent should read it.
 *
 * The org files are the content and the source of truth. AGENTS.md is a
 * loader that @-includes them in this order, and CLAUDE.md is one line
 * pointing at AGENTS.md -- so a tool that looks for either finds the same
 * set, and there is one list to keep current rather than two that drift.
 *
 * Note that AGENTS.org and AGENTS.md are different things: the org file
 * is how the agent works, the markdown file is the loader.
 *
 * Returns: (transfer none) (array length=n_files): the file set
 */
const ClawtWorkspaceFile *
clawt_workspace_files(guint *n_files);

/**
 * clawt_workspace_identity_files:
 *
 * The subset that goes into the system prompt, in order.
 *
 * This is what `persona.identity_files` defaults to. README.org is left
 * out on purpose: it describes the workspace to a person reading the
 * directory, and paying for it in every turn's context buys nothing.
 *
 * Returns: (transfer full) (array zero-terminated=1): the file names
 */
GStrv
clawt_workspace_identity_files(void);

/**
 * clawt_workspace_scaffold:
 * @agent: the agent's configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Writes any missing file in the standard set into the agent's workspace.
 *
 * Existing files are never touched. The defaults are a starting point the
 * user is expected to edit, and rewriting them on every start would throw
 * away exactly the work this exists to make possible.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_workspace_scaffold(ClawtAgentConfig  *agent,
                         GError           **error);

/**
 * clawt_workspace_file_path:
 * @agent: the agent's configuration
 * @name: a file name from the standard set, or any other name
 *
 * Resolves a workspace-relative name to a full path.
 *
 * Refuses a name containing a path separator or "..", because this is
 * reached from an IPC request and a client that could name
 * "../../secrets" would be reading another agent's credentials.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL if @name is not
 *   a plain file name
 */
gchar *
clawt_workspace_file_path(ClawtAgentConfig *agent,
                          const gchar      *name);

G_END_DECLS
