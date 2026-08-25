/*
 * clawt-mcp-tools.h - The tools agents use to work together
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Served to each agent over its own link as an MCP endpoint.  This is how a
 * chief-of-staff actually delegates: it calls clawtilla_delegate the way it
 * would call any other tool, and the daemon does the routing.
 *
 * Every call comes back through the daemon on purpose.  Recursion depth,
 * rate limits, budgets and tool permissions are then enforced in exactly
 * one place, rather than in each agent's own idea of what is reasonable.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"
#include "agent/clawt-agent-manager.h"
#include "chat/clawt-loop-guard.h"
#include "chat/clawt-room.h"
#include "task/clawt-task-manager.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MCP_TOOLS (clawt_mcp_tools_get_type())

G_DECLARE_FINAL_TYPE(ClawtMcpTools, clawt_mcp_tools,
                     CLAWT, MCP_TOOLS, GObject)

/**
 * ClawtMcpDeliverFunc:
 * @from_agent: who is sending
 * @target: an agent id or a room id
 * @body: the message
 * @task_id: (nullable): the task it belongs to
 * @depth: how far this message is from the original request
 * @user_data: data passed when the callback was installed
 * @error: (out) (optional): return location for a #GError
 *
 * Routes a message.  Installed by the daemon, which owns the mailboxes.
 *
 * Returns: %TRUE if the message was accepted for delivery
 */
typedef gboolean (*ClawtMcpDeliverFunc)(const gchar  *from_agent,
                                        const gchar  *target,
                                        const gchar  *body,
                                        const gchar  *task_id,
                                        gint          depth,
                                        gpointer      user_data,
                                        GError      **error);

ClawtMcpTools *clawt_mcp_tools_new(ClawtAgentManager *agents,
                                   ClawtTaskManager  *tasks,
                                   ClawtLoopGuard    *guard);

/**
 * clawt_mcp_tools_set_deliver_func:
 * @self: a #ClawtMcpTools
 * @func: (nullable) (scope notified): how to route a message
 * @user_data: data for @func
 * @destroy: (nullable): called when @func is replaced
 */
void clawt_mcp_tools_set_deliver_func(ClawtMcpTools       *self,
                                      ClawtMcpDeliverFunc  func,
                                      gpointer             user_data,
                                      GDestroyNotify       destroy);

/**
 * ClawtMcpCreateAgentFunc:
 * @agent_id: the id for the new agent
 * @purpose: (nullable): the persona, in prose, for the new agent's
 *   `SOUL.org`
 * @settings: (element-type utf8 utf8): configuration keys and values,
 *   keyed exactly as they appear in the schema
 * @start: whether to start it once it exists
 * @user_data: as supplied
 * @error: return location for a #GError
 *
 * Creates an agent on behalf of one that asked.
 *
 * A hook rather than a call into the daemon, for the same reason
 * delivering a message is: the tools belong to the library and the
 * daemon owns the fleet.  Whatever supplies this must put the request
 * through the same validation a person's would go through -- an agent
 * asking is not a reason to trust the answer more.
 *
 * @purpose is separate from @settings because it is not a configuration
 * key and never was.  Passing it through the same hash meant it was
 * written to the config file under a name nothing reads, and an operator
 * who wrote a whole persona got an agent that had never seen it.
 *
 * Returns: (transfer full) (nullable): what to tell the agent, or %NULL
 */
typedef gchar *(*ClawtMcpCreateAgentFunc)(const gchar  *agent_id,
                                          const gchar  *purpose,
                                          GHashTable   *settings,
                                          gboolean      start,
                                          gpointer      user_data,
                                          GError      **error);

/**
 * clawt_mcp_tools_describe_for_agent:
 * @self: a #ClawtMcpTools
 * @agent_id: whose tools to describe
 *
 * The tools this agent is being offered *right now*, as org text for
 * its `TOOLS.org`.
 *
 * From the same gate that answers `tools/list`, so the file and the
 * live list cannot disagree -- which they did, for as long as the file
 * carried a table written when the workspace was scaffolded.
 *
 * Returns: (transfer full): org text
 */
gchar *clawt_mcp_tools_describe_for_agent(ClawtMcpTools *self,
                                          const gchar   *agent_id);

/**
 * clawt_mcp_tools_set_create_agent_func:
 * @self: a #ClawtMcpTools
 * @func: (nullable) (scope notified): the hook
 * @user_data: passed to @func
 * @destroy: frees @user_data
 *
 * Without this the fleet tools are not offered at all, whatever an
 * agent's permissions say: a tool that is listed and then fails teaches
 * an agent to keep trying.
 */
void clawt_mcp_tools_set_create_agent_func(ClawtMcpTools           *self,
                                           ClawtMcpCreateAgentFunc  func,
                                           gpointer                 user_data,
                                           GDestroyNotify           destroy);

/**
 * clawt_mcp_tools_set_image_store:
 * @self: a #ClawtMcpTools
 * @store: (nullable): the disk images that have been fetched
 *
 * So an agent creating a VM agent can be told which images exist rather
 * than inventing a path.  The designer could not name one and had to be
 * stopped from choosing `vm` at all; this is the other answer to the
 * same problem.
 */
void clawt_mcp_tools_set_image_store(ClawtMcpTools     *self,
                                     ClawtVmImageStore *store);

/**
 * clawt_mcp_tools_set_attachment_dir:
 * @self: a #ClawtMcpTools
 * @dir: (nullable): where files sent to an operator are kept
 *
 * Gives agents a way to send a file rather than only text.
 *
 * The bytes are copied here at send time.  Passing the path through
 * would work for a client on the same host and show nothing for a remote
 * one -- a failure that looks like a broken image rather than an
 * unsupported configuration.  Without a directory the `attachments`
 * argument is refused with a reason instead of silently ignored.
 */
void clawt_mcp_tools_set_attachment_dir(ClawtMcpTools *self,
                                        const gchar   *dir);

/**
 * clawt_mcp_tools_set_room_manager:
 * @self: a #ClawtMcpTools
 * @rooms: (transfer none) (nullable): the fleet's rooms
 *
 * Lets the room tools look up and create rooms.  Without it
 * clawtilla_create_room and clawtilla_room_history are listed but
 * cannot work.
 */
void clawt_mcp_tools_set_room_manager(ClawtMcpTools    *self,
                                      ClawtRoomManager *rooms);

/**
 * clawt_mcp_tools_list:
 * @self: a #ClawtMcpTools
 * @agent_id: the agent asking
 *
 * The MCP tools/list reply for one agent, with tools it may not use
 * omitted entirely.
 *
 * Omitted rather than listed-and-refused: an agent that can see a tool will
 * try it, and a refusal costs a turn to discover something it could have
 * been told up front.
 *
 * Returns: (transfer full): the tool list
 */
/**
 * clawt_mcp_tools_set_tool_providers:
 * @self: a #ClawtMcpTools
 * @providers: (transfer none) (nullable) (element-type GObject): objects
 *   implementing #ClawtToolProvider
 *
 * Adds plugin-provided tools to what agents are offered.
 *
 * Listed alongside the built-in tools rather than in a separate namespace,
 * because from the agent's side there is no difference: a tool is a tool.
 * Plugin tools are still subject to the same per-agent allow and deny
 * lists.
 */
void clawt_mcp_tools_set_tool_providers(ClawtMcpTools *self,
                                        GPtrArray     *providers);

JsonNode *clawt_mcp_tools_list(ClawtMcpTools *self, const gchar *agent_id);

/**
 * clawt_mcp_tools_call:
 * @self: a #ClawtMcpTools
 * @agent_id: the agent calling
 * @request: (transfer none): an MCP JSON-RPC request
 *
 * Handles one tool call.
 *
 * Returns: (transfer full): the JSON-RPC response
 */
JsonNode *clawt_mcp_tools_call(ClawtMcpTools *self,
                               const gchar   *agent_id,
                               JsonNode      *request);

/**
 * clawt_mcp_tools_is_permitted:
 * @self: a #ClawtMcpTools
 * @agent_id: an agent
 * @tool_name: a tool
 *
 * Whether @agent_id may call @tool_name, applying its allow and deny lists
 * and the capabilities it actually has.
 *
 * Returns: %TRUE if the call is permitted
 */
gboolean clawt_mcp_tools_is_permitted(ClawtMcpTools *self,
                                      const gchar   *agent_id,
                                      const gchar   *tool_name);

G_END_DECLS
