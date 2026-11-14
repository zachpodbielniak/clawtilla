/*
 * clawt-mcp-directory.h - A server with too many tools, as three tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every tool an MCP server lists is read by the model on every call:
 * its name, its description and its input schema.  For a server with a
 * dozen tools that is the point of listing them.  For one with four
 * hundred it is twenty-five thousand tokens spent before the agent has
 * read a word of what it was asked, on tools it will mostly never use --
 * and an API-model engine refuses more than 128 tools a turn outright.
 *
 * So a big server is presented as a directory: three tools that search
 * the others, give one tool's exact inputs, and run one by name.  The
 * agent looks up what it needs instead of reading everything.
 *
 * The directory is built from the list the relay has *already filtered*
 * by permission, and a call through it is checked against that list a
 * second time on the way out.  The predicate and the builder answer from
 * one list, so a tool this agent may not use can be neither found nor
 * called -- the rule "a wall built without the door" in AGENTS.md is
 * about exactly this pair.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/* ── The modes, for a client to offer ────────────────────────────── */

/**
 * clawt_tool_directory_mode_count:
 *
 * How many ways a connector's tools can be presented.
 *
 * Clients build their "Present as a directory" choice by walking this,
 * so a mode added here is offered everywhere at once.
 *
 * Returns: the number of modes
 */
guint clawt_tool_directory_mode_count(void);

/**
 * clawt_tool_directory_mode_nth:
 * @n: an index below clawt_tool_directory_mode_count()
 *
 * Returns: the mode at @n, in the order to offer them
 */
ClawtToolDirectoryMode clawt_tool_directory_mode_nth(guint n);

/**
 * clawt_tool_directory_mode_nth_nick:
 * @n: an index below clawt_tool_directory_mode_count()
 *
 * Returns: (transfer none): the spelling written to clawtilla.yaml
 */
const gchar *clawt_tool_directory_mode_nth_nick(guint n);

/**
 * clawt_tool_directory_mode_nth_label:
 * @n: an index below clawt_tool_directory_mode_count()
 *
 * Returns: (transfer none): what to show a person choosing one
 */
const gchar *clawt_tool_directory_mode_nth_label(guint n);

/**
 * clawt_tool_directory_mode_from_nick:
 * @nick: (nullable): a value of a connector's `directory` key
 * @out_mode: (out): the mode
 *
 * Reads a `directory` value.  An absent or empty one is
 * %CLAWT_TOOL_DIRECTORY_AUTO, which is what saying nothing means.
 *
 * Returns: %FALSE when @nick is a spelling no mode has, in which case
 *   @out_mode is %CLAWT_TOOL_DIRECTORY_AUTO and the caller says so
 */
gboolean clawt_tool_directory_mode_from_nick(const gchar             *nick,
					     ClawtToolDirectoryMode  *out_mode);

/* ── The rule ────────────────────────────────────────────────────── */

/**
 * clawt_mcp_directory_is_big:
 * @n_tools: how many tools the server lists to this agent
 * @n_chars: how many characters of tool definitions those are
 * @min_tools: `connectors.directory_min_tools`
 * @min_chars: `connectors.directory_min_chars`
 *
 * Whether a server has more tools than an agent should read whole.
 *
 * Strictly more than either threshold: forty tools at the default is
 * still a list.
 *
 * Returns: %TRUE when either threshold is passed
 */
gboolean clawt_mcp_directory_is_big(guint64 n_tools,
				    guint64 n_chars,
				    guint64 min_tools,
				    guint64 min_chars);

/**
 * clawt_mcp_directory_decide:
 * @mode: the connector's `directory`
 * @cli_searches_tools: whether the agent's CLI searches tools by itself
 * @n_tools: how many tools the server lists to this agent
 * @n_chars: how many characters of tool definitions those are
 * @min_tools: `connectors.directory_min_tools`
 * @min_chars: `connectors.directory_min_chars`
 *
 * Whether this server reaches this agent as a directory.
 *
 * The one statement of the rule.  The relay applies it, TOOLS.org
 * describes it, and the clients show it, and three copies would be
 * three answers.
 *
 * Returns: %TRUE for a directory, %FALSE for every tool listed
 */
gboolean clawt_mcp_directory_decide(ClawtToolDirectoryMode mode,
				    gboolean               cli_searches_tools,
				    guint64                n_tools,
				    guint64                n_chars,
				    guint64                min_tools,
				    guint64                min_chars);

/**
 * clawt_mcp_directory_cli_searches_tools:
 * @provider: (nullable): an agent's `model.provider`
 *
 * Whether that agent's CLI looks tools up as it needs them, rather than
 * reading every tool's description on every call.
 *
 * Claude Code does: past about a tenth of its context in MCP tools it
 * defers them and searches.  Wrapping such a CLI's servers in a second
 * search would make it search a search.  claude-tmux runs the same
 * binary and is answered the same way.  Every other CLI reads the whole
 * list, so this is %FALSE for them.
 *
 * The answer belongs to ai-glib -- it is a fact about a CLI, like
 * `AiCliClientClass.config_dir_env` -- and it lives here only until a
 * class datum exists there; see the roadmap item "ai-glib: a
 * searches_tools class datum".  The provider is normalised through
 * libreclaw first, so an unknown spelling is answered for the CLI it
 * will actually run.
 *
 * Returns: %TRUE when the CLI searches tools itself
 */
gboolean clawt_mcp_directory_cli_searches_tools(const gchar *provider);

/**
 * clawt_mcp_directory_measure:
 * @tools: a `tools/list` result's `tools` array
 *
 * How many characters of tool definitions the model would read: each
 * tool serialised as it arrives, name, description and schema together.
 *
 * Returns: the count
 */
guint64 clawt_mcp_directory_measure(JsonArray *tools);

/**
 * clawt_mcp_directory_tool_area:
 * @name: a tool's name
 *
 * The area a tool belongs to, for the overview: the part of its name
 * before the first underscore (`issues_create` is `issues`), or the
 * first word of a camelCase name (`createIssue` is `create`).
 *
 * Returns: (transfer full): the area, lower case
 */
gchar *clawt_mcp_directory_tool_area(const gchar *name);

/**
 * clawt_mcp_directory_status_label:
 * @mode_nick: (nullable): the connector's `directory`
 * @n_tools: how many tools it was last seen to list, or -1 when no
 *   agent has used it yet
 * @n_chars: how many characters of definitions those were
 * @min_tools: `connectors.directory_min_tools`
 * @min_chars: `connectors.directory_min_chars`
 *
 * One line for a connector's row in a client: how many tools it has and
 * whether agents see them as a directory.
 *
 * In the library because all three clients draw it, and a rule two
 * clients apply separately is a rule they disagree about.
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_mcp_directory_status_label(const gchar *mode_nick,
					gint64       n_tools,
					gint64       n_chars,
					gint64       min_tools,
					gint64       min_chars);

/* ── The directory ───────────────────────────────────────────────── */

#define CLAWT_TYPE_MCP_DIRECTORY (clawt_mcp_directory_get_type())

/**
 * ClawtMcpDirectory:
 *
 * One server's tools, as the relay has seen them, and the three tools
 * that stand in for them when there are too many.
 *
 * It reads the conversation as it passes: it notes each `tools/list`
 * request, gathers the reply (following `nextCursor` itself when it is
 * the one presenting a directory), and answers the three directory
 * tools without the server.  When the directory is not wanted -- mode
 * `never`, or a CLI that searches by itself -- it still counts what goes
 * past and touches nothing, so the Connectors page can say how big a
 * server is whichever agents use it.
 */
G_DECLARE_FINAL_TYPE(ClawtMcpDirectory, clawt_mcp_directory,
		     CLAWT, MCP_DIRECTORY, GObject)

/**
 * clawt_mcp_directory_new:
 * @server: the connector's name, which prefixes the three tools
 * @mode: the connector's `directory`
 * @cli_searches_tools: whether the agent's CLI searches tools itself
 * @min_tools: `connectors.directory_min_tools`
 * @min_chars: `connectors.directory_min_chars`
 *
 * Returns: (transfer full): a directory with nothing in it yet
 */
ClawtMcpDirectory *clawt_mcp_directory_new(const gchar            *server,
					   ClawtToolDirectoryMode  mode,
					   gboolean                cli_searches_tools,
					   guint64                 min_tools,
					   guint64                 min_chars);

/**
 * clawt_mcp_directory_get_server:
 * @self: a #ClawtMcpDirectory
 *
 * Returns: (transfer none): the name the three tools are prefixed with
 */
const gchar *clawt_mcp_directory_get_server(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_get_search_name:
 * @self: a #ClawtMcpDirectory
 *
 * Returns: (transfer none): `<server>_search_tools`
 */
const gchar *clawt_mcp_directory_get_search_name(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_get_describe_name:
 * @self: a #ClawtMcpDirectory
 *
 * Returns: (transfer none): `<server>_describe_tool`
 */
const gchar *clawt_mcp_directory_get_describe_name(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_get_call_name:
 * @self: a #ClawtMcpDirectory
 *
 * Returns: (transfer none): `<server>_call_tool`
 */
const gchar *clawt_mcp_directory_get_call_name(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_load:
 * @self: a #ClawtMcpDirectory
 * @tools: the tools this agent may use -- already filtered by permission
 *
 * Replaces what the directory knows with @tools and decides whether it
 * is presented as a directory.  Emits #ClawtMcpDirectory::catalogued.
 *
 * Returns: %TRUE when it is now a directory
 */
gboolean clawt_mcp_directory_load(ClawtMcpDirectory *self, JsonArray *tools);

/**
 * clawt_mcp_directory_is_active:
 * @self: a #ClawtMcpDirectory
 *
 * Returns: %TRUE when the last tool list was replaced by the three tools
 */
gboolean clawt_mcp_directory_is_active(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_get_tool_count:
 * @self: a #ClawtMcpDirectory
 *
 * Returns: how many tools the last complete list held
 */
guint clawt_mcp_directory_get_tool_count(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_get_char_count:
 * @self: a #ClawtMcpDirectory
 *
 * Returns: how many characters of definitions the last list held
 */
guint64 clawt_mcp_directory_get_char_count(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_has_tool:
 * @self: a #ClawtMcpDirectory
 * @name: a tool's name
 *
 * Returns: %TRUE when @name is one of the tools this agent may use
 */
gboolean clawt_mcp_directory_has_tool(ClawtMcpDirectory *self,
				      const gchar       *name);

/**
 * clawt_mcp_directory_list_tools:
 * @self: a #ClawtMcpDirectory
 *
 * The three tools that stand in for the server's own, ready to be a
 * `tools/list` result's `tools`.
 *
 * Returns: (transfer full): the definitions
 */
JsonArray *clawt_mcp_directory_list_tools(ClawtMcpDirectory *self);

/**
 * clawt_mcp_directory_overview:
 * @self: a #ClawtMcpDirectory
 * @max_chars: roughly how long it may be
 *
 * What the server covers, area by area, with how many tools in each:
 * `issues (41), pulls (30), repos (12), ... and 9 more`.
 *
 * Returns: (transfer full): the overview, empty when nothing is loaded
 */
gchar *clawt_mcp_directory_overview(ClawtMcpDirectory *self,
				    gsize              max_chars);

/**
 * clawt_mcp_directory_search:
 * @self: a #ClawtMcpDirectory
 * @query: (nullable): what the agent wants to do, in a few words
 * @limit: how many matches at most; 0 means the default
 *
 * The tools that best match @query, best first, each with a one-line
 * description and its input fields.  An empty @query is answered with
 * the area-by-area overview and a few tool names from each area, so a
 * first call can be "what is here at all".
 *
 * Returns: (transfer full): the answer, as the text of a tool result
 */
gchar *clawt_mcp_directory_search(ClawtMcpDirectory *self,
				  const gchar       *query,
				  guint              limit);

/**
 * clawt_mcp_directory_describe:
 * @self: a #ClawtMcpDirectory
 * @name: a tool's exact name
 *
 * One tool's definition: its description and its exact input schema.
 *
 * Returns: (transfer full) (nullable): JSON text, or %NULL when this
 *   agent has no tool by that name
 */
gchar *clawt_mcp_directory_describe(ClawtMcpDirectory *self,
				    const gchar       *name);

/**
 * clawt_mcp_directory_rewrite_outbound:
 * @self: a #ClawtMcpDirectory
 * @line: one JSON-RPC message from the agent's MCP client
 * @forward: (out) (transfer full) (nullable): what to send the server in
 *   its place -- a call to the inner tool, for `<server>_call_tool`
 * @reply: (out) (transfer full) (nullable): the answer, when the
 *   directory answers by itself
 *
 * Looks at a message on its way to the server.  A search or a describe
 * is answered here; a call through the directory becomes an ordinary
 * `tools/call` naming the inner tool, so whatever checks the call next
 * -- the permitted list, an action rule, a person approving it -- sees
 * the tool that will actually run and never the directory.
 *
 * Returns: %TRUE when @forward or @reply was set, %FALSE to send @line
 *   on unchanged
 */
gboolean clawt_mcp_directory_rewrite_outbound(ClawtMcpDirectory  *self,
					      const gchar        *line,
					      gchar             **forward,
					      gchar             **reply);

/**
 * clawt_mcp_directory_rewrite_inbound:
 * @self: a #ClawtMcpDirectory
 * @line: one JSON-RPC message from the server, already filtered
 * @to_client: (out) (transfer full) (nullable): what to send the client
 * @to_server: (out) (transfer full) (nullable): a request of the
 *   directory's own to send the server -- the next page of a tool list
 *
 * Looks at a message on its way to the client.  The reply to a
 * `tools/list` is gathered, and once whole it is either passed on or
 * replaced by the three directory tools.
 *
 * Returns: %TRUE when the outs say what to do, %FALSE to send @line on
 *   unchanged
 */
gboolean clawt_mcp_directory_rewrite_inbound(ClawtMcpDirectory  *self,
					     const gchar        *line,
					     gchar             **to_client,
					     gchar             **to_server);

/* ── What the daemon remembers about a connector's size ──────────── */

/**
 * ClawtToolCensus:
 * @name: the connector's name
 * @tools: how many tools its server listed, after the `tools:` narrowing
 * @chars: how many characters of definitions those were
 * @areas: (nullable): the area overview, as clawt_mcp_directory_overview()
 *   gives it
 * @seen_at: when, in seconds since the epoch
 *
 * What a relay last saw of a connector's server.
 *
 * The daemon never starts a connector's server itself -- the relay does,
 * on an agent's behalf, with a credential -- so this is how the
 * Connectors page and TOOLS.org can say how big a server is.  Kept in
 * `<state_dir>/connector-tools.yaml`.
 */
typedef struct {
	gchar  *name;
	gint64  tools;
	gint64  chars;
	gchar  *areas;
	gint64  seen_at;
} ClawtToolCensus;

GType clawt_tool_census_get_type(void) G_GNUC_CONST;

#define CLAWT_TYPE_TOOL_CENSUS (clawt_tool_census_get_type())

/**
 * clawt_tool_census_copy:
 * @self: a #ClawtToolCensus
 *
 * Returns: (transfer full): a copy
 */
ClawtToolCensus *clawt_tool_census_copy(const ClawtToolCensus *self);

/**
 * clawt_tool_census_free:
 * @self: (transfer full) (nullable): a #ClawtToolCensus
 */
void clawt_tool_census_free(ClawtToolCensus *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtToolCensus, clawt_tool_census_free)

/**
 * clawt_tool_census_path:
 * @state_dir: the daemon's `daemon.state_dir`, expanded
 *
 * Returns: (transfer full): where the census is kept
 */
gchar *clawt_tool_census_path(const gchar *state_dir);

/**
 * clawt_tool_census_load:
 * @path: the census file
 * @name: a connector's name
 *
 * Returns: (transfer full) (nullable): what was last seen of @name, or
 *   %NULL when no relay has reported it -- including when the file is
 *   missing or unreadable, since this is bookkeeping and never a reason
 *   to refuse anything
 */
ClawtToolCensus *clawt_tool_census_load(const gchar *path, const gchar *name);

/**
 * clawt_tool_census_store:
 * @path: the census file
 * @census: what a relay just saw
 * @error: (out) (optional): return location for a #GError
 *
 * Records @census, replacing the entry with its name and keeping the
 * rest.  Written atomically, mode 0600.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_tool_census_store(const gchar            *path,
				 const ClawtToolCensus  *census,
				 GError                **error);

G_END_DECLS
