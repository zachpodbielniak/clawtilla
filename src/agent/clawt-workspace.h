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
 * @generated: %TRUE when clawtilla writes it rather than scaffolding it
 *
 * One entry in the standard workspace file set.
 *
 * @generated separates the files clawtilla keeps current from the ones
 * it lays down once and then leaves alone. Only .mcp.json is generated,
 * and even that one is merged rather than replaced -- the "clawtilla"
 * server in it is clawtilla's, everything beside it is yours.
 */
typedef struct {
    const gchar *name;
    const gchar *title;
    gboolean     identity;
    gboolean     generated;
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
 * clawt_workspace_write_mcp_config:
 * @config: (nullable): the fleet configuration, for shared integrations
 * @agent: the agent's configuration
 * @daemon_socket: (nullable): the daemon's IPC socket
 * @state_dir: the agent's state directory, which holds its token
 * @error: (out) (optional): return location for a #GError
 *
 * Writes `.mcp.json` into the agent's workspace.
 *
 * This is what puts clawtilla's orchestration tools into the agent's
 * session. An agent runs an AI CLI, and the only way such a CLI can be
 * given tools is a config naming an MCP server to talk to; the CLI finds
 * this file in its working directory by itself, the same way it finds
 * CLAUDE.md.
 *
 * It is also how a `mcp` integration reaches an agent: one entry per
 * integration in scope, so a tool server configured once for the fleet
 * arrives in every agent's file without any of them being edited by
 * hand.
 *
 * Rewritten on every start, unlike the org files: it is generated rather
 * than authored, and a stale copy points the agent at a socket that has
 * moved.  Only the keys clawtilla owns are rewritten -- `clawtilla` and
 * anything beginning `clawtilla-`.  Everything else in the file is
 * carried across untouched, because this is the file people add their own
 * MCP servers to.
 *
 * With @config %NULL the built-in servers are still written and no shared
 * integration is: an agent's own tools do not depend on there being a
 * fleet around it.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_workspace_write_mcp_config(ClawtConfig      *config,
                                 ClawtAgentConfig *agent,
                                 const gchar      *daemon_socket,
                                 const gchar      *state_dir,
                                 GError          **error);

/**
 * clawt_workspace_update_tools_org:
 * @config: (nullable): the fleet configuration, for shared integrations
 * @agent: the agent's configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Rewrites the integrations section of the agent's `TOOLS.org`.
 *
 * TOOLS.org is scaffolded once and then belongs to whoever edits it, so
 * clawtilla owns a marked region of it and nothing else: the region is
 * replaced, everything around it is kept, and a file that has lost its
 * markers gets them appended rather than being rewritten.
 *
 * It exists because an integration nobody told the agent about is an
 * integration it does not use.  A Matrix channel is invisible from
 * inside a session -- messages simply arrive -- and an MCP server's tools
 * appear with no indication of who they reach or whether a person is on
 * the other end.
 *
 * Skips the write when nothing changed, so an editor with the file open
 * does not reload it on every daemon start.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_workspace_update_tools_org(ClawtConfig      *config,
                                 ClawtAgentConfig *agent,
                                 GError          **error);

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
