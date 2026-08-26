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
 * clawt_workspace_detect_identity_files:
 * @workspace: a directory that may already hold a persona
 *
 * The identity files an imported workspace already has, in load order.
 *
 * clawtilla spells its identity files in org; a workspace that grew up
 * elsewhere keeps the same concerns in markdown, and the two sets never
 * collide -- so an import copied a complete persona in and scaffolded a
 * blank .org beside every file of it, and the agent loaded the blanks.
 *
 * Returns %NULL when the workspace holds any of clawtilla's own .org
 * identity files: one that already speaks this spelling has chosen.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the
 *   files to adopt, or %NULL if there is nothing to adopt
 */
GStrv
clawt_workspace_detect_identity_files(const gchar *workspace);

/**
 * clawt_workspace_effective_identity_files:
 * @agent: the agent's configuration
 *
 * The identity files this agent will actually load.
 *
 * A configured list wins; an inline `persona.system_prompt` makes the
 * question moot and the answer is empty; otherwise it is the standard
 * set. The scaffolder and the renderer both ask, because a file one
 * writes and the other never reads is not a starting point -- it is
 * clutter contradicting the files that *are* read.
 *
 * Returns: (transfer full) (array zero-terminated=1): the files, possibly
 *   empty, never %NULL
 */
GStrv
clawt_workspace_effective_identity_files(ClawtAgentConfig *agent);

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
 * clawt_workspace_scaffold_with_mission:
 * @agent: the agent's configuration
 * @mission: (nullable): what this agent is for, in prose
 * @mission_written: (out) (optional): %TRUE when @mission reached SOUL.org
 * @error: (out) (optional): return location for a #GError
 *
 * Scaffolds the workspace, with @mission as the agent's mission.
 *
 * @mission is the persona an operator writes when creating an agent. It
 * belongs in SOUL.org and nowhere else: an inline `persona.system_prompt`
 * *replaces* the identity files rather than adding to them, so putting it
 * there would trade a discarded persona for a discarded IDENTITY.org and
 * TOOLS.org -- and an agent that has to discover what computer it has
 * spends its first turns finding out.
 *
 * An existing SOUL.org still wins, because it is somebody's work. That is
 * what @mission_written is for: the caller can say the purpose did not
 * land instead of leaving whoever wrote it believing it did.
 *
 * With @mission %NULL this is exactly clawt_workspace_scaffold().
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_workspace_scaffold_with_mission(ClawtAgentConfig  *agent,
                                      const gchar       *mission,
                                      gboolean          *mission_written,
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
 * clawt_workspace_update_tool_list:
 * @agent: the agent's configuration
 * @listing: org text naming the tools it actually has
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the tools clawtilla is offering this agent *right now* into a
 * marked region of `TOOLS.org` of its own.
 *
 * The file used to carry a table written when the workspace was
 * scaffolded and never touched again, so a tool granted afterwards never
 * appeared -- and an agent reading its own file concluded it did not
 * have one, which was true of the file and false of the fleet.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_workspace_update_tool_list(ClawtAgentConfig *agent,
                                 const gchar      *listing,
                                 GError          **error);

/**
 * clawt_workspace_update_computer:
 * @agent: the agent's configuration
 * @described: what its computer is, from
 *   clawt_agent_describe_computer()
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the agent's *live* computer into a marked region of
 * `TOOLS.org`.
 *
 * The scaffolded "Your computer" section is written once and then
 * belongs to whoever edits the file, so it describes the machine as it
 * was the day the workspace was made. Shared folders are exactly what
 * changes afterwards: a fleet default added in Settings reaches every
 * agent's computer and reached no agent's file, so an agent had a
 * directory it was never told about and could only find by calling a
 * tool it had no reason to call.
 *
 * Same shape as the tool list beside it, and the same reason: an agent
 * believes its own file, because the file is in the prompt and a tool
 * call is something it has to decide to make.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_workspace_update_computer(ClawtAgentConfig *agent,
                                const gchar      *described,
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

/**
 * ClawtImportMode:
 * @CLAWT_IMPORT_COPY: recursively copy the source into the workspace
 * @CLAWT_IMPORT_LINK: symlink the workspace at the source
 * @CLAWT_IMPORT_GIT: clone a git repository, as a submodule where that
 *   is possible
 *
 * How an existing agent's files become a clawtilla workspace.
 *
 * A copy is the safe default and is a *fork*: the original stops being
 * the thing the agent reads the moment it is made, so editing either
 * afterwards is editing one of two diverging directories. That is right
 * for adopting somebody else's agent and wrong for the case people
 * actually hit -- a workspace they are already maintaining somewhere
 * they like it, which they want clawtilla to *use* rather than to take
 * a snapshot of.
 */
typedef enum {
    CLAWT_IMPORT_COPY = 0,
    CLAWT_IMPORT_LINK,
    CLAWT_IMPORT_GIT
} ClawtImportMode;

/**
 * clawt_import_mode_count:
 *
 * How many import modes a client should offer.
 *
 * Walked rather than named, for the reason the computer types and the
 * colour schemes both record: two hand-written copies is how a mode
 * came to be offered by one client and not the other.
 *
 * Returns: the number of modes
 */
guint clawt_import_mode_count(void);

/**
 * clawt_import_mode_nth:
 * @n: an index below clawt_import_mode_count()
 *
 * Returns: the mode at @n, safest first
 */
ClawtImportMode clawt_import_mode_nth(guint n);

/**
 * clawt_import_mode_nth_nick:
 * @n: an index below clawt_import_mode_count()
 *
 * Returns: (transfer none): the spelling sent on the wire
 */
const gchar *clawt_import_mode_nth_nick(guint n);

/**
 * clawt_import_mode_nth_label:
 * @n: an index below clawt_import_mode_count()
 *
 * Says what the mode *does to the original*, which is the question
 * somebody is answering when they choose one.
 *
 * Returns: (transfer none): the label, never %NULL
 */
const gchar *clawt_import_mode_nth_label(guint n);

/**
 * clawt_import_mode_from_nick:
 * @nick: (nullable): a spelling from a form, a flag or an IPC payload
 *
 * Returns: the mode, or %CLAWT_IMPORT_COPY for anything unrecognised --
 *   the safest of the three, and the one that cannot touch a directory
 *   somebody else is using
 */
ClawtImportMode clawt_import_mode_from_nick(const gchar *nick);

/**
 * clawt_import_mode_takes_url:
 * @mode: a #ClawtImportMode
 *
 * Whether @mode's source is a git URL rather than a directory.
 *
 * Asked rather than compared against "git" in each client, so a client
 * cannot end up offering a folder chooser for a mode that needs a URL.
 *
 * Returns: %TRUE for %CLAWT_IMPORT_GIT
 */
gboolean clawt_import_mode_takes_url(ClawtImportMode mode);

/**
 * clawt_workspace_adopt:
 * @mode: how to adopt @source
 * @source: a directory, or a git URL for %CLAWT_IMPORT_GIT
 * @workspace: where the agent's workspace goes
 * @keep_git: whether a copy brings the source's .git directory with it
 * @out_files: (out) (optional): how many files a copy wrote
 * @out_detail: (out) (optional) (transfer full): one sentence saying
 *   what actually happened, for a client to show
 * @error: (out) (optional): return location for a #GError
 *
 * The one implementation of every import mode, so the CLI, the GTK
 * client and the web client cannot disagree about what `--link` means.
 *
 * @out_detail exists because two of the three modes have an outcome the
 * caller could not predict: a git import becomes a submodule only when
 * the workspace root is inside a repository, and saying which happened
 * is the difference between a workspace somebody's `git status` will
 * track and one it will not.
 *
 * Returns: %TRUE if the workspace now exists
 */
gboolean
clawt_workspace_adopt(ClawtImportMode   mode,
                      const gchar      *source,
                      const gchar      *workspace,
                      gboolean          keep_git,
                      guint            *out_files,
                      gchar           **out_detail,
                      GError          **error);

/**
 * clawt_workspace_git_toplevel:
 * @path: a directory, which need not exist
 *
 * The git repository @path would live inside, if any.
 *
 * Separate and pure-ish so the submodule decision can be asserted on
 * without standing up a repository around the developer's own home
 * directory -- and because "is this a repo" is asked twice, once to
 * decide and once to report which happened.
 *
 * Returns: (transfer full) (nullable): the repository's top level, or
 *   %NULL when @path is not inside one
 */
gchar *
clawt_workspace_git_toplevel(const gchar *path);

G_END_DECLS
