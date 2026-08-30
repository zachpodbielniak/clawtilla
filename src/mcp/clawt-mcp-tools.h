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

#include "clawt-enums.h"
#include "clawt-types.h"
#include "agent/clawt-agent-manager.h"
#include "chat/clawt-loop-guard.h"
#include "chat/clawt-room.h"
#include "core/clawt-event-bus.h"
#include "memory/clawt-transcript-index.h"
#include "task/clawt-handoff-store.h"
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
 * @priority: the delivery band, %CLAWT_PRIORITY_NORMAL unless the agent
 *   named one
 * @user_data: data passed when the callback was installed
 * @error: (out) (optional): return location for a #GError
 *
 * Routes a message.  Installed by the daemon, which owns the mailboxes.
 *
 * @priority is on the hook rather than only in the tool that reads it
 * because this is the only route from an agent's tool call to a
 * mailbox.  Without it `clawtilla_message_agent` parsed the band an
 * agent asked for, refused a wrong one, and then dropped a right one on
 * the floor -- so the tool description's promise that urgent jumps the
 * queue was true of nothing.
 *
 * Returns: %TRUE if the message was accepted for delivery
 */
typedef gboolean (*ClawtMcpDeliverFunc)(const gchar   *from_agent,
                                        const gchar   *target,
                                        const gchar   *body,
                                        const gchar   *task_id,
                                        gint           depth,
                                        ClawtPriority  priority,
                                        gpointer       user_data,
                                        GError       **error);

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
 * ClawtMcpAskDecisionFunc:
 * @agent_id: who is asking
 * @decision: (transfer none): the question, its options and its default
 * @user_data: as supplied
 * @error: return location for a #GError
 *
 * Files a decision on behalf of an agent.
 *
 * A hook for the same reason creating an agent is one: the tools belong
 * to the library and the daemon owns the inbox.  It is also what makes
 * the tool safe to offer to every agent -- filing a question is not a
 * fleet operation, so there is nothing here to gate, and an agent that
 * files trivia wastes an operator's attention rather than their
 * machine.
 *
 * Returns: (transfer full) (nullable): what to tell the agent
 */
typedef gchar *(*ClawtMcpAskDecisionFunc)(const gchar    *agent_id,
                                          ClawtDecision  *decision,
                                          gpointer        user_data,
                                          GError        **error);

/**
 * clawt_mcp_tools_set_ask_decision_func:
 * @self: a #ClawtMcpTools
 * @func: (nullable) (scope notified): the hook
 * @user_data: passed to @func
 * @destroy: frees @user_data
 *
 * Unset, the tool is not offered.  A library embedded without a daemon
 * has no inbox to file into, and a tool that is listed and then fails
 * teaches an agent to keep trying.
 */
void clawt_mcp_tools_set_ask_decision_func(ClawtMcpTools           *self,
                                           ClawtMcpAskDecisionFunc  func,
                                           gpointer                 user_data,
                                           GDestroyNotify           destroy);

/**
 * ClawtMcpHandoffFunc:
 * @from_agent: who is giving the task up
 * @task_id: the task whose ownership is moving
 * @to_agent: who is taking it on
 * @reason: (nullable): why
 * @out_queued: (out) (optional): how many handoffs this agent now has queued
 * @user_data: as supplied
 * @error: return location for a #GError
 *
 * Queues an ownership transfer against the turn that asked for it.
 *
 * A hook for the same reason delivering a message is one: the tools
 * belong to the library and the daemon owns the queue, the store and the
 * turn boundary this runs on.  It **queues** and returns; it does not
 * wait for the recipient, because the recipient may be mid-turn and a
 * turn is minutes.
 *
 * Whatever supplies this enforces orchestration.handoff_max_per_turn,
 * since only it can see the queue.  Everything that can be judged from
 * the caller -- the task exists, the target exists, the team allows it,
 * the chain is not too deep -- is judged before this is reached, so a
 * refusal names the reason nearest the agent.
 *
 * Returns: %TRUE if it was queued
 */
typedef gboolean (*ClawtMcpHandoffFunc)(const gchar  *from_agent,
                                        const gchar  *task_id,
                                        const gchar  *to_agent,
                                        const gchar  *reason,
                                        guint        *out_queued,
                                        gpointer      user_data,
                                        GError      **error);

/**
 * clawt_mcp_tools_set_handoff_func:
 * @self: a #ClawtMcpTools
 * @func: (nullable) (scope notified): the hook
 * @user_data: passed to @func
 * @destroy: frees @user_data
 *
 * Unset, `clawtilla_handoff` is not offered.  A library embedded
 * without a daemon has no turn boundary to run a handoff on and no
 * store to leave a receipt in, and a tool that is listed and then fails
 * teaches an agent to keep trying.
 */
void clawt_mcp_tools_set_handoff_func(ClawtMcpTools       *self,
                                      ClawtMcpHandoffFunc  func,
                                      gpointer             user_data,
                                      GDestroyNotify       destroy);

/**
 * clawt_mcp_tools_set_handoff_store:
 * @self: a #ClawtMcpTools
 * @store: (nullable): where handoff receipts are kept
 *
 * So `clawtilla_task_status` can answer about a task the in-memory
 * #ClawtTaskManager has forgotten.
 *
 * Separate from the hook, because reading a receipt and queueing a
 * handoff are different permissions: an agent that may not hand work
 * over may still ask what became of work handed to *it*.
 */
void clawt_mcp_tools_set_handoff_store(ClawtMcpTools     *self,
                                       ClawtHandoffStore *store);

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
 * clawt_mcp_tools_set_skill_library:
 * @self: a #ClawtMcpTools
 * @library: (nullable): the fleet's skills, or %NULL when there are none
 *
 * So an agent can list and read the procedures it has been given.
 *
 * Unowned, and re-set on every reload: `skills.dir` is in the file a
 * reload rereads, so a library kept by reference here would go on
 * answering correctly about a directory nobody has configured any more.
 * %NULL withdraws both skill tools rather than leaving them to fail.
 */
void clawt_mcp_tools_set_skill_library(ClawtMcpTools     *self,
                                       ClawtSkillLibrary *library);

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
 * clawt_mcp_tools_set_event_bus:
 * @self: a #ClawtMcpTools
 * @bus: (transfer none) (nullable): where to publish what agents do
 *
 * Puts an agent's own tool calls on the audit trail.
 *
 * `clawtilla_computer_exec` run by a *person* through a client has been
 * published as `computer.exec` since the daemon was written, because
 * running something on the machine is the most consequential thing that
 * socket can do.  The same command run by the agent itself reached no
 * bus at all -- there was no route from here to one -- so
 * `docs/security.org` asserted both were audited while the half a
 * reader would most want to look up was the missing one.
 *
 * Unowned.  The bus outlives the tools, which the daemon builds and
 * drops with the rest of its components.
 */
/**
 * clawt_mcp_tools_set_takeover:
 * @self: a #ClawtMcpTools
 * @takeover: (transfer none) (nullable): who is holding which screen
 *
 * Gives the tools the lease, so an agent can ask for a person.
 *
 * Unowned, like the other hooks here: the daemon owns it and outlives
 * this. Without it `clawtilla_request_hands` refuses rather than
 * pretending -- a library embedded without a daemon has nobody to ask.
 */
void clawt_mcp_tools_set_takeover(ClawtMcpTools *self,
                                  ClawtTakeover *takeover);

void clawt_mcp_tools_set_event_bus(ClawtMcpTools *self,
                                   ClawtEventBus *bus);

/**
 * ClawtMcpToolObserverFunc:
 * @agent_id: which agent made the call
 * @tool: the tool it called
 * @args: (nullable): its arguments, serialised
 * @user_data: what was passed to clawt_mcp_tools_set_observer()
 *
 * One tool call, before it is dispatched.
 */
typedef void (*ClawtMcpToolObserverFunc)(const gchar *agent_id,
                                         const gchar *tool,
                                         const gchar *args,
                                         gpointer     user_data);

/**
 * clawt_mcp_tools_set_observer:
 * @self: a #ClawtMcpTools
 * @func: (nullable) (scope notified) (closure user_data): what to tell about
 *   each call, or %NULL for nothing
 * @user_data: passed to @func
 * @notify: (nullable): frees @user_data
 *
 * Tells the daemon about every tool call as it arrives, which is what
 * feeds the repeat counter and the turn watchdog's idea of activity.
 *
 * Told **before** dispatch and before the permission check, because a
 * refused call is still a call: an agent that has been told twenty times
 * it may not use a tool is looping exactly as hard as one that is being
 * served, and counting only the successes would hide the case that
 * matters most.
 */
void clawt_mcp_tools_set_observer(ClawtMcpTools            *self,
                                  ClawtMcpToolObserverFunc  func,
                                  gpointer                  user_data,
                                  GDestroyNotify            notify);

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
 * clawt_mcp_tools_set_transcript_index:
 * @self: a #ClawtMcpTools
 * @index: (transfer none) (nullable): the fleet's searchable transcript
 *
 * Lets clawtilla_recall search past conversations.
 *
 * Without it the tool is not offered at all rather than offered and
 * failing -- a library embedded without a daemon has no index, and an
 * agent that can see a tool will try it.
 */
void clawt_mcp_tools_set_transcript_index(ClawtMcpTools        *self,
                                          ClawtTranscriptIndex *index);

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
 * @room_id: (nullable): which of the agent's conversations this call
 *   belongs to, or %NULL when the caller cannot say
 * @request: (transfer none): an MCP JSON-RPC request
 *
 * Handles one tool call.
 *
 * Returns: (transfer full): the JSON-RPC response
 */
JsonNode *clawt_mcp_tools_call(ClawtMcpTools *self,
                               const gchar   *agent_id,
                               const gchar   *room_id,
                               JsonNode      *request);

/**
 * clawt_mcp_tools_call_defers: (skip)
 * @self: a #ClawtMcpTools
 * @agent_id: the agent calling
 * @request: (transfer none): an MCP JSON-RPC request
 *
 * Whether this call must be answered later rather than from the caller's
 * context.
 *
 * True for exactly one tool today, and the property is the tool's rather
 * than the caller's: computer_exec waits for a command whose duration the
 * *agent* chooses, so answering it inline stalls every other client for
 * as long as that command takes. Every other tool answers from memory or
 * from a local file.
 *
 * A caller that gets %TRUE must use clawt_mcp_tools_call_async().
 *
 * Returns: %TRUE if the call blocks
 */
gboolean clawt_mcp_tools_call_defers(ClawtMcpTools *self,
                                     const gchar   *agent_id,
                                     JsonNode      *request);

/**
 * clawt_mcp_tools_call_async: (skip)
 * @self: a #ClawtMcpTools
 * @agent_id: the agent calling
 * @request: (transfer none): an MCP JSON-RPC request
 * @callback: called on the context this was dispatched from
 * @user_data: data for @callback
 *
 * Handles one tool call, waiting on a worker thread.
 *
 * The lookup and the audit trail stay on the calling context; only the
 * command itself runs on the thread. A refusal is still delivered
 * through @callback, so a caller that has already deferred its reply
 * always has something to answer with.
 */
void clawt_mcp_tools_call_async(ClawtMcpTools       *self,
                                const gchar         *agent_id,
                                const gchar         *room_id,
                                JsonNode            *request,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data);

/**
 * clawt_mcp_tools_call_finish: (skip)
 * @self: a #ClawtMcpTools
 * @result: the #GAsyncResult
 *
 * Returns: (transfer full): the JSON-RPC response
 */
JsonNode *clawt_mcp_tools_call_finish(ClawtMcpTools *self,
                                      GAsyncResult  *result);

/**
 * clawt_mcp_tools_set_main_context:
 * @self: a #ClawtMcpTools
 * @context: (nullable): the context an asynchronous answer must arrive on
 *
 * Names the context clawt_mcp_tools_call_async() completes on.
 *
 * Without it a task takes whatever is thread-default when the call comes
 * in, which is the process default unless a caller pushed one --
 * dispatching a source pushes nothing. An answer would then be queued on
 * a loop the daemon does not run, and the request would never be
 * answered at all.
 *
 * %NULL means the ambient context, which is right for a caller that has
 * only one.
 */
void clawt_mcp_tools_set_main_context(ClawtMcpTools *self,
                                      GMainContext  *context);

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
