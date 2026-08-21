/*
 * clawt-mcp-tools.c - The tools agents use to work together
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mcp/clawt-mcp-tools.h"
#include "interfaces/clawt-tool-provider.h"
#include "plugin/clawt-param-info.h"

#include <string.h>

/*
 * Which capability a tool needs.
 *
 * A tool an agent cannot use is not offered at all.  Listing it and then
 * refusing costs the agent a turn to discover something it could have been
 * told up front, and teaches it to keep trying.
 */
typedef enum {
    NEEDS_NOTHING = 0,
    NEEDS_PEER_COMMS,
    NEEDS_COMPUTER
} ToolRequirement;

typedef struct {
    const gchar          *name;
    const gchar          *description;
    ToolRequirement       requirement;
    const ClawtParamInfo *params;
    gsize                 n_params;
} ToolDefinition;

/* ── Parameter tables ────────────────────────────────────────────── */

static const ClawtParamInfo no_params[] = { { NULL, NULL, NULL, FALSE } };

static const ClawtParamInfo get_agent_params[] = {
    { "agent_id", "string", "The agent to look up.", TRUE }
};

static const ClawtParamInfo message_agent_params[] = {
    { "agent_id", "string", "Who to message.", TRUE },
    { "body",     "string", "What to say.", TRUE },
    { "priority", "string",
      "low, normal, high or urgent. Urgent jumps the queue, so reserve it "
      "for things that genuinely cannot wait.", FALSE }
};

static const ClawtParamInfo post_room_params[] = {
    { "room_id", "string", "Which room.", TRUE },
    { "body",    "string", "What to say.", TRUE }
};

static const ClawtParamInfo ask_agent_params[] = {
    { "agent_id", "string", "Who to ask.", TRUE },
    { "message",  "string", "The question.", TRUE }
};

static const ClawtParamInfo delegate_params[] = {
    { "agent_id", "string", "Who should do the work.", TRUE },
    { "task",     "string", "What they should do. Be specific: they cannot "
                            "see this conversation.", TRUE },
    { "reason",   "string", "Why this agent. Recorded for the audit trail.",
      FALSE }
};

static const ClawtParamInfo task_id_params[] = {
    { "task_id", "string", "The task.", TRUE }
};

static const ClawtParamInfo task_result_params[] = {
    { "task_id", "string", "The task being finished.", TRUE },
    { "result",  "string", "What it produced.", TRUE }
};

static const ClawtParamInfo mailbox_list_params[] = {
    { "limit", "integer", "How many to return. Defaults to 20.", FALSE }
};

static const ClawtParamInfo mailbox_id_params[] = {
    { "message_id", "string", "The message.", TRUE }
};

static const ClawtParamInfo mailbox_reply_params[] = {
    { "message_id", "string", "The message being replied to.", TRUE },
    { "body",       "string", "The reply.", TRUE }
};

static const ClawtParamInfo create_room_params[] = {
    { "room_id", "string", "Identifier for the new room.", TRUE },
    { "members", "string", "Comma-separated agent ids.", TRUE },
    { "name",    "string", "Display name.", FALSE }
};

static const ClawtParamInfo room_history_params[] = {
    { "room_id", "string", "Which room.", TRUE },
    { "limit",   "integer", "How many recent messages. Defaults to 20.",
      FALSE }
};

static const ClawtParamInfo computer_exec_params[] = {
    { "command",     "string", "The command to run.", TRUE },
    { "working_dir", "string", "Where to run it.", FALSE },
    { "timeout",     "integer",
      "Seconds before giving up. Defaults to 120. Always set one for "
      "anything that might wait for input.", FALSE }
};

/* ── The tools ───────────────────────────────────────────────────── */

#define TOOL(name_, desc_, req_, params_) \
    { name_, desc_, req_, params_, G_N_ELEMENTS(params_) }

static const ToolDefinition tools[] = {
    TOOL("clawtilla_list_agents",
         "List the other agents in this fleet, with what each is for and "
         "whether it is running. Use this before delegating, so you pick "
         "someone suited to the work.",
         NEEDS_NOTHING, no_params),

    TOOL("clawtilla_get_agent",
         "Look up one agent: its description, state and what it can do.",
         NEEDS_NOTHING, get_agent_params),

    TOOL("clawtilla_message_agent",
         "Send a message to another agent. It is queued, so this works even "
         "if they are stopped -- they will see it when they start. Returns "
         "immediately without waiting for a reply.",
         NEEDS_PEER_COMMS, message_agent_params),

    TOOL("clawtilla_ask_agent",
         "Ask another agent something and wait for their answer. Use this "
         "when you cannot continue without the reply; use "
         "clawtilla_message_agent when you can.",
         NEEDS_PEER_COMMS, ask_agent_params),

    TOOL("clawtilla_delegate",
         "Hand a piece of work to another agent and get a task id back. "
         "They work on it independently; check on it with "
         "clawtilla_task_status. Use this rather than asking, when the work "
         "will take a while.",
         NEEDS_PEER_COMMS, delegate_params),

    TOOL("clawtilla_post_room",
         "Post a message to a room, reaching every member.",
         NEEDS_PEER_COMMS, post_room_params),

    TOOL("clawtilla_create_room",
         "Create a room with the given members, for work that several "
         "agents need to see.",
         NEEDS_PEER_COMMS, create_room_params),

    TOOL("clawtilla_room_history",
         "Read recent messages from a room.",
         NEEDS_NOTHING, room_history_params),

    TOOL("clawtilla_task_status",
         "Check how a delegated task is going.",
         NEEDS_NOTHING, task_id_params),

    TOOL("clawtilla_task_result",
         "Get the result of a finished task.",
         NEEDS_NOTHING, task_id_params),

    TOOL("clawtilla_task_cancel",
         "Cancel a task you delegated, and everything it spawned.",
         NEEDS_NOTHING, task_id_params),

    TOOL("clawtilla_task_complete",
         "Report that a task assigned to you is done, with its result. Do "
         "this rather than only replying, or whoever delegated it is still "
         "waiting.",
         NEEDS_NOTHING, task_result_params),

    TOOL("clawtilla_mailbox_list",
         "List the messages waiting for you.",
         NEEDS_NOTHING, mailbox_list_params),

    TOOL("clawtilla_mailbox_read",
         "Read one message from your mailbox.",
         NEEDS_NOTHING, mailbox_id_params),

    TOOL("clawtilla_mailbox_ack",
         "Mark a message dealt with, so it is not delivered again.",
         NEEDS_NOTHING, mailbox_id_params),

    TOOL("clawtilla_mailbox_reply",
         "Reply to a message in your mailbox and acknowledge it in one step.",
         NEEDS_PEER_COMMS, mailbox_reply_params),

    TOOL("clawtilla_computer_exec",
         "Run a command on your computer.",
         NEEDS_COMPUTER, computer_exec_params),

    TOOL("clawtilla_computer_state",
         "Describe your computer: what it is and what you can reach from it.",
         NEEDS_COMPUTER, no_params)
};

#undef TOOL

struct _ClawtMcpTools {
    GObject parent_instance;

    ClawtAgentManager *agents;
    ClawtTaskManager  *tasks;
    ClawtLoopGuard    *guard;
    GHashTable        *rooms;   /* room_id -> ClawtRoom, unowned */

    GPtrArray *tool_providers;  /* GObject*, unowned */

    ClawtMcpDeliverFunc deliver;
    gpointer            deliver_data;
    GDestroyNotify      deliver_destroy;
};

G_DEFINE_FINAL_TYPE(ClawtMcpTools, clawt_mcp_tools, G_TYPE_OBJECT)

ClawtMcpTools *
clawt_mcp_tools_new(ClawtAgentManager *agents,
                    ClawtTaskManager  *tasks,
                    ClawtLoopGuard    *guard)
{
    ClawtMcpTools *self = g_object_new(CLAWT_TYPE_MCP_TOOLS, NULL);

    if (agents != NULL)
        self->agents = g_object_ref(agents);
    if (tasks != NULL)
        self->tasks = g_object_ref(tasks);
    if (guard != NULL)
        self->guard = g_object_ref(guard);

    return self;
}

void
clawt_mcp_tools_set_deliver_func(ClawtMcpTools       *self,
                                 ClawtMcpDeliverFunc  func,
                                 gpointer             user_data,
                                 GDestroyNotify       destroy)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    if (self->deliver_destroy != NULL && self->deliver_data != NULL)
        self->deliver_destroy(self->deliver_data);

    self->deliver = func;
    self->deliver_data = user_data;
    self->deliver_destroy = destroy;
}

void
clawt_mcp_tools_set_rooms(ClawtMcpTools *self, GHashTable *rooms)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    self->rooms = rooms;
}

void
clawt_mcp_tools_set_tool_providers(ClawtMcpTools *self, GPtrArray *providers)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    g_clear_pointer(&self->tool_providers, g_ptr_array_unref);

    if (providers != NULL)
        self->tool_providers = g_ptr_array_ref(providers);
}

/*
 * Finds which plugin owns a tool name.
 *
 * First match wins.  Two plugins claiming one name is a packaging mistake
 * rather than something to arbitrate, and picking deterministically at
 * least makes it reproducible.
 */
static ClawtToolProvider *
find_provider(ClawtMcpTools *self, const gchar *tool_name)
{
    guint i;

    for (i = 0; self->tool_providers != NULL &&
                i < self->tool_providers->len; i++) {
        GObject *object = g_ptr_array_index(self->tool_providers, i);
        g_auto(GStrv) names = NULL;
        gsize j;

        if (!CLAWT_IS_TOOL_PROVIDER(object))
            continue;

        names = clawt_tool_provider_list_tools(CLAWT_TOOL_PROVIDER(object));

        for (j = 0; names != NULL && names[j] != NULL; j++) {
            if (g_strcmp0(names[j], tool_name) == 0)
                return CLAWT_TOOL_PROVIDER(object);
        }
    }

    return NULL;
}

/* ── Permissions ─────────────────────────────────────────────────── */

static const ToolDefinition *
find_tool(const gchar *name)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(tools); i++) {
        if (g_strcmp0(tools[i].name, name) == 0)
            return &tools[i];
    }

    return NULL;
}

gboolean
clawt_mcp_tools_is_permitted(ClawtMcpTools *self,
                             const gchar   *agent_id,
                             const gchar   *tool_name)
{
    const ToolDefinition *tool;
    ClawtAgent *agent;
    ClawtAgentConfig *config;
    g_auto(GStrv) allow = NULL;
    g_auto(GStrv) deny = NULL;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_MCP_TOOLS(self), FALSE);

    tool = find_tool(tool_name);

    /*
     * A plugin tool has no built-in definition, so it needs no capability
     * -- but it still goes through the same allow and deny lists below.
     * A plugin must not be able to hand an agent something its operator
     * turned off.
     */
    if (tool == NULL && find_provider(self, tool_name) == NULL)
        return FALSE;

    agent = (self->agents != NULL)
            ? clawt_agent_manager_get(self->agents, agent_id) : NULL;

    if (agent == NULL)
        return FALSE;

    /*
     * Capability first.  A tool the agent physically cannot use is not
     * permitted however generous its allow list, since offering it would
     * only produce a confident call and a confusing failure.
     */
    switch (tool != NULL ? tool->requirement : NEEDS_NOTHING) {
    case NEEDS_COMPUTER:
        if ((clawt_agent_get_caps(agent) & CLAWT_AGENT_CAPS_COMPUTER) == 0)
            return FALSE;
        break;

    case NEEDS_PEER_COMMS:
        if ((clawt_agent_get_caps(agent) & CLAWT_AGENT_CAPS_PEER_COMMS) == 0)
            return FALSE;
        break;

    case NEEDS_NOTHING:
    default:
        break;
    }

    config = clawt_agent_get_config(agent);

    /*
     * Deny is applied after allow and wins, so a broad allow cannot quietly
     * re-enable something specifically turned off.
     */
    deny = clawt_agent_config_get_string_list(config, "tools.deny");
    for (i = 0; deny != NULL && deny[i] != NULL; i++) {
        if (g_strcmp0(deny[i], tool_name) == 0)
            return FALSE;
    }

    allow = clawt_agent_config_get_string_list(config, "tools.allow");
    if (allow == NULL || allow[0] == NULL)
        return TRUE;

    for (i = 0; allow[i] != NULL; i++) {
        if (g_strcmp0(allow[i], tool_name) == 0)
            return TRUE;
    }

    return FALSE;
}

JsonNode *
clawt_mcp_tools_list(ClawtMcpTools *self, const gchar *agent_id)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    gsize i;

    g_return_val_if_fail(CLAWT_IS_MCP_TOOLS(self), NULL);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "tools");
    json_builder_begin_array(builder);

    for (i = 0; i < G_N_ELEMENTS(tools); i++) {
        g_autoptr(JsonNode) schema = NULL;

        if (!clawt_mcp_tools_is_permitted(self, agent_id, tools[i].name))
            continue;

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, tools[i].name);

        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, tools[i].description);

        schema = clawt_param_info_to_schema(tools[i].params,
                                            tools[i].params == no_params
                                            ? 0 : tools[i].n_params);
        json_builder_set_member_name(builder, "inputSchema");
        json_builder_add_value(builder, g_steal_pointer(&schema));

        json_builder_end_object(builder);
    }

    for (i = 0; self->tool_providers != NULL &&
                i < self->tool_providers->len; i++) {
        GObject *object = g_ptr_array_index(self->tool_providers, i);
        g_auto(GStrv) names = NULL;
        gsize j;

        if (!CLAWT_IS_TOOL_PROVIDER(object))
            continue;

        names = clawt_tool_provider_list_tools(CLAWT_TOOL_PROVIDER(object));

        for (j = 0; names != NULL && names[j] != NULL; j++) {
            const ClawtParamInfo *params;
            g_autoptr(JsonNode) schema = NULL;
            gsize n_params = 0;

            if (!clawt_mcp_tools_is_permitted(self, agent_id, names[j]))
                continue;

            params = clawt_tool_provider_get_params(
                CLAWT_TOOL_PROVIDER(object), names[j], &n_params);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, names[j]);
            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(builder, "Provided by a plugin.");

            schema = clawt_param_info_to_schema(params, n_params);
            json_builder_set_member_name(builder, "inputSchema");
            json_builder_add_value(builder, g_steal_pointer(&schema));
            json_builder_end_object(builder);
        }
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Responses ───────────────────────────────────────────────────── */

static JsonNode *
make_response(JsonNode *request_id, const gchar *text, gboolean is_error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");

    if (request_id != NULL) {
        json_builder_set_member_name(builder, "id");
        json_builder_add_value(builder, json_node_copy(request_id));
    }

    json_builder_set_member_name(builder, "result");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "content");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "text");
    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, text != NULL ? text : "");
    json_builder_end_object(builder);
    json_builder_end_array(builder);

    /*
     * isError rather than a JSON-RPC error.  MCP distinguishes "the tool
     * ran and reports a problem" from "the call itself was malformed", and
     * a model handles the first by trying something else rather than by
     * concluding the tool is broken.
     */
    json_builder_set_member_name(builder, "isError");
    json_builder_add_boolean_value(builder, is_error);

    json_builder_end_object(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static const gchar *
argument_string(JsonObject *arguments, const gchar *name)
{
    if (arguments == NULL || !json_object_has_member(arguments, name))
        return NULL;

    if (json_node_get_value_type(json_object_get_member(arguments, name)) !=
        G_TYPE_STRING)
        return NULL;

    return json_object_get_string_member(arguments, name);
}

static gint64
argument_int(JsonObject *arguments, const gchar *name, gint64 fallback)
{
    if (arguments == NULL || !json_object_has_member(arguments, name))
        return fallback;

    return json_object_get_int_member(arguments, name);
}

/* ── Tool implementations ────────────────────────────────────────── */

static gchar *
tool_list_agents(ClawtMcpTools *self, const gchar *agent_id)
{
    g_autoptr(GString) out = g_string_new(NULL);
    GPtrArray *agents;
    guint i;

    if (self->agents == NULL)
        return g_strdup("There are no other agents.");

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        const gchar *description;

        /* Never itself: an agent listing itself invites self-delegation. */
        if (g_strcmp0(clawt_agent_get_id(agent), agent_id) == 0)
            continue;

        description = clawt_agent_get_description(agent);

        g_string_append_printf(out, "%s (%s) - %s [%s]\n",
                               clawt_agent_get_id(agent),
                               clawt_agent_get_name(agent),
                               description != NULL ? description
                                                   : "no description",
                               clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                   clawt_agent_get_state(agent)));
    }

    if (out->len == 0)
        return g_strdup("You are the only agent in this fleet.");

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static gchar *
tool_get_agent(ClawtMcpTools *self, JsonObject *arguments, gboolean *is_error)
{
    const gchar *wanted = argument_string(arguments, "agent_id");
    ClawtAgent *agent;
    g_autofree gchar *caps = NULL;

    if (wanted == NULL) {
        *is_error = TRUE;
        return g_strdup("agent_id is required.");
    }

    agent = (self->agents != NULL)
            ? clawt_agent_manager_get(self->agents, wanted) : NULL;

    if (agent == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no agent called '%s'.", wanted);
    }

    caps = clawt_flags_to_string(CLAWT_TYPE_AGENT_CAPS,
                                 clawt_agent_get_caps(agent));

    return g_strdup_printf(
        "%s (%s)\nState: %s\nDescription: %s\nCan: %s",
        clawt_agent_get_id(agent), clawt_agent_get_name(agent),
        clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                           clawt_agent_get_state(agent)),
        clawt_agent_get_description(agent) != NULL
            ? clawt_agent_get_description(agent) : "no description",
        caps);
}

static gchar *
tool_message_agent(ClawtMcpTools *self,
                   const gchar   *agent_id,
                   JsonObject    *arguments,
                   gboolean      *is_error)
{
    const gchar *target = argument_string(arguments, "agent_id");
    const gchar *body = argument_string(arguments, "body");
    g_autoptr(GError) error = NULL;

    if (target == NULL || body == NULL) {
        *is_error = TRUE;
        return g_strdup("agent_id and body are both required.");
    }

    if (self->deliver == NULL) {
        *is_error = TRUE;
        return g_strdup("Messaging is not available.");
    }

    if (!self->deliver(agent_id, target, body, NULL, 1, self->deliver_data,
                       &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    return g_strdup_printf("Queued for %s. They will see it when they are "
                           "next running.", target);
}

static gchar *
tool_delegate(ClawtMcpTools *self,
              const gchar   *agent_id,
              JsonObject    *arguments,
              gboolean      *is_error)
{
    const gchar *assignee = argument_string(arguments, "agent_id");
    const gchar *work = argument_string(arguments, "task");
    const gchar *reason = argument_string(arguments, "reason");
    g_autoptr(GError) error = NULL;
    ClawtTask *task;

    if (assignee == NULL || work == NULL) {
        *is_error = TRUE;
        return g_strdup("agent_id and task are both required.");
    }

    if (self->tasks == NULL || self->deliver == NULL) {
        *is_error = TRUE;
        return g_strdup("Delegation is not available.");
    }

    if (clawt_agent_manager_get(self->agents, assignee) == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no agent called '%s'. Use "
                               "clawtilla_list_agents to see who is here.",
                               assignee);
    }

    task = clawt_task_manager_create(self->tasks, agent_id, assignee, work,
                                     NULL, &error);
    if (task == NULL) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    clawt_task_set_reason(task, reason);

    if (!self->deliver(agent_id, assignee, work, clawt_task_get_id(task),
                       1, self->deliver_data, &error)) {
        /*
         * The task is failed rather than left pending.  A task nobody was
         * ever told about would sit in the list for ever looking like work
         * in progress.
         */
        clawt_task_manager_fail(self->tasks, clawt_task_get_id(task),
                                error->message);
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    return g_strdup_printf(
        "Delegated to %s as task %s. Check on it with "
        "clawtilla_task_status.", assignee, clawt_task_get_id(task));
}

static gchar *
tool_task_status(ClawtMcpTools *self, JsonObject *arguments,
                 gboolean *is_error)
{
    const gchar *task_id = argument_string(arguments, "task_id");
    ClawtTask *task;

    if (task_id == NULL) {
        *is_error = TRUE;
        return g_strdup("task_id is required.");
    }

    task = (self->tasks != NULL)
           ? clawt_task_manager_get(self->tasks, task_id) : NULL;

    if (task == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no task %s.", task_id);
    }

    return g_strdup_printf("Task %s (%s): %s%s%s",
                           task_id,
                           clawt_task_get_assignee(task),
                           clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                              clawt_task_get_state(task)),
                           clawt_task_get_reason(task) != NULL ? " - " : "",
                           clawt_task_get_reason(task) != NULL
                               ? clawt_task_get_reason(task) : "");
}

static gchar *
tool_task_result(ClawtMcpTools *self, JsonObject *arguments,
                 gboolean *is_error)
{
    const gchar *task_id = argument_string(arguments, "task_id");
    ClawtTask *task;

    if (task_id == NULL) {
        *is_error = TRUE;
        return g_strdup("task_id is required.");
    }

    task = (self->tasks != NULL)
           ? clawt_task_manager_get(self->tasks, task_id) : NULL;

    if (task == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no task %s.", task_id);
    }

    if (!clawt_task_is_finished(task))
        return g_strdup_printf("Task %s is still %s; there is no result yet.",
                               task_id,
                               clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                   clawt_task_get_state(task)));

    if (clawt_task_get_result(task) == NULL)
        return g_strdup_printf("Task %s ended as %s with no result: %s",
                               task_id,
                               clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                   clawt_task_get_state(task)),
                               clawt_task_get_reason(task) != NULL
                                   ? clawt_task_get_reason(task)
                                   : "no reason given");

    return g_strdup(clawt_task_get_result(task));
}

static gchar *
tool_computer_exec(ClawtMcpTools *self,
                   const gchar   *agent_id,
                   JsonObject    *arguments,
                   gboolean      *is_error)
{
    const gchar *command = argument_string(arguments, "command");
    const gchar *working_dir = argument_string(arguments, "working_dir");
    guint timeout = (guint)argument_int(arguments, "timeout", 120);
    ClawtAgent *agent;
    ClawtComputer *computer;
    g_autoptr(ClawtExecResult) result = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;

    if (command == NULL) {
        *is_error = TRUE;
        return g_strdup("command is required.");
    }

    agent = clawt_agent_manager_get(self->agents, agent_id);
    computer = (agent != NULL) ? clawt_agent_get_computer(agent) : NULL;

    if (computer == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no computer to run commands on.");
    }

    if (!g_shell_parse_argv(command, NULL, &argv, &error)) {
        *is_error = TRUE;
        return g_strdup_printf("That command could not be parsed: %s",
                               error->message);
    }

    result = clawt_computer_exec(computer, (const gchar * const *)argv,
                                 working_dir, timeout, NULL, &error);

    if (result == NULL) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    if (clawt_exec_result_succeeded(result))
        return g_strdup(clawt_exec_result_get_stdout(result));

    /*
     * Exit status and stderr together.  A failing command whose reply is
     * only its stdout tells the agent nothing about why it failed.
     */
    *is_error = TRUE;
    return g_strdup_printf("Exit status %d.\n%s%s",
                           clawt_exec_result_get_exit_status(result),
                           clawt_exec_result_get_stderr(result),
                           clawt_exec_result_get_stdout(result));
}

static gchar *
tool_computer_state(ClawtMcpTools *self, const gchar *agent_id)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);
    ClawtComputer *computer = (agent != NULL)
                              ? clawt_agent_get_computer(agent) : NULL;

    if (computer == NULL)
        return g_strdup("You have no computer.");

    return clawt_computer_describe(computer);
}

static gchar *
tool_mailbox_list(ClawtMcpTools *self, const gchar *agent_id,
                  JsonObject *arguments)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);
    ClawtMailbox *mailbox = (agent != NULL)
                            ? clawt_agent_get_mailbox(agent) : NULL;
    ClawtMailboxFilter filter = { CLAWT_MAILBOX_PENDING, 20, TRUE };
    g_autoptr(GPtrArray) items = NULL;
    g_autoptr(GString) out = NULL;
    guint i;

    if (mailbox == NULL)
        return g_strdup("You have no mailbox.");

    filter.limit = (guint)argument_int(arguments, "limit", 20);
    items = clawt_mailbox_list(mailbox, &filter);

    if (items->len == 0)
        return g_strdup("Your mailbox is empty.");

    out = g_string_new(NULL);

    for (i = 0; i < items->len; i++) {
        ClawtMailboxItem *item = g_ptr_array_index(items, i);

        g_string_append_printf(out, "%s from %s [%s]: %s\n",
                               clawt_mailbox_item_get_id(item),
                               clawt_mailbox_item_get_from(item),
                               clawt_enum_to_nick(CLAWT_TYPE_PRIORITY,
                                   clawt_mailbox_item_get_priority(item)),
                               clawt_mailbox_item_get_body(item));
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Dispatch ────────────────────────────────────────────────────── */

JsonNode *
clawt_mcp_tools_call(ClawtMcpTools *self,
                     const gchar   *agent_id,
                     JsonNode      *request)
{
    JsonObject *root;
    JsonObject *params = NULL;
    JsonObject *arguments = NULL;
    JsonNode *request_id = NULL;
    const gchar *method = NULL;
    const gchar *tool_name = NULL;
    g_autofree gchar *text = NULL;
    gboolean is_error = FALSE;

    g_return_val_if_fail(CLAWT_IS_MCP_TOOLS(self), NULL);
    g_return_val_if_fail(request != NULL, NULL);

    if (!JSON_NODE_HOLDS_OBJECT(request))
        return make_response(NULL, "Malformed request.", TRUE);

    root = json_node_get_object(request);

    if (json_object_has_member(root, "id"))
        request_id = json_object_get_member(root, "id");

    if (json_object_has_member(root, "method"))
        method = json_object_get_string_member(root, "method");

    if (json_object_has_member(root, "params"))
        params = json_object_get_object_member(root, "params");

    if (g_strcmp0(method, "tools/list") == 0) {
        g_autoptr(JsonNode) listing = clawt_mcp_tools_list(self, agent_id);
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "jsonrpc");
        json_builder_add_string_value(builder, "2.0");

        if (request_id != NULL) {
            json_builder_set_member_name(builder, "id");
            json_builder_add_value(builder, json_node_copy(request_id));
        }

        json_builder_set_member_name(builder, "result");
        json_builder_add_value(builder, g_steal_pointer(&listing));
        json_builder_end_object(builder);

        return json_builder_get_root(builder);
    }

    if (g_strcmp0(method, "tools/call") != 0)
        return make_response(request_id,
                             "Only tools/list and tools/call are supported.",
                             TRUE);

    if (params != NULL) {
        tool_name = argument_string(params, "name");

        if (json_object_has_member(params, "arguments"))
            arguments = json_object_get_object_member(params, "arguments");
    }

    if (tool_name == NULL)
        return make_response(request_id, "No tool was named.", TRUE);

    if (!clawt_mcp_tools_is_permitted(self, agent_id, tool_name)) {
        /*
         * Named plainly, so the agent stops rather than trying three
         * variations of the same call.
         */
        return make_response(request_id,
                             g_strdup_printf("You are not permitted to use "
                                             "%s.", tool_name),
                             TRUE);
    }

    if (g_strcmp0(tool_name, "clawtilla_list_agents") == 0)
        text = tool_list_agents(self, agent_id);
    else if (g_strcmp0(tool_name, "clawtilla_get_agent") == 0)
        text = tool_get_agent(self, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_message_agent") == 0 ||
             g_strcmp0(tool_name, "clawtilla_ask_agent") == 0)
        text = tool_message_agent(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_delegate") == 0)
        text = tool_delegate(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_task_status") == 0)
        text = tool_task_status(self, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_task_result") == 0)
        text = tool_task_result(self, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_task_cancel") == 0) {
        const gchar *task_id = argument_string(arguments, "task_id");
        guint cancelled = (task_id != NULL && self->tasks != NULL)
                          ? clawt_task_manager_cancel(self->tasks, task_id,
                                                      "cancelled by the "
                                                      "delegating agent")
                          : 0;

        text = g_strdup_printf("Cancelled %u task(s).", cancelled);
    } else if (g_strcmp0(tool_name, "clawtilla_task_complete") == 0) {
        const gchar *task_id = argument_string(arguments, "task_id");
        const gchar *result = argument_string(arguments, "result");

        if (task_id == NULL || result == NULL) {
            is_error = TRUE;
            text = g_strdup("task_id and result are both required.");
        } else if (self->tasks != NULL &&
                   clawt_task_manager_complete(self->tasks, task_id, result)) {
            text = g_strdup_printf("Task %s marked complete.", task_id);
        } else {
            is_error = TRUE;
            text = g_strdup_printf("Task %s could not be completed; it may "
                                   "have already finished.", task_id);
        }
    } else if (g_strcmp0(tool_name, "clawtilla_computer_exec") == 0)
        text = tool_computer_exec(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_computer_state") == 0)
        text = tool_computer_state(self, agent_id);
    else if (g_strcmp0(tool_name, "clawtilla_mailbox_list") == 0)
        text = tool_mailbox_list(self, agent_id, arguments);
    else {
        ClawtToolProvider *provider = find_provider(self, tool_name);
        g_autoptr(GError) provider_error = NULL;

        if (provider == NULL) {
            is_error = TRUE;
            text = g_strdup_printf("There is no tool called %s.", tool_name);
        } else {
            text = clawt_tool_provider_call(provider, agent_id, tool_name,
                                            arguments, &provider_error);

            if (text == NULL) {
                is_error = TRUE;
                text = g_strdup(provider_error != NULL
                                ? provider_error->message
                                : "that tool failed without saying why");
            }
        }
    }

    return make_response(request_id, text, is_error);
}

static void
clawt_mcp_tools_dispose(GObject *object)
{
    ClawtMcpTools *self = CLAWT_MCP_TOOLS(object);

    if (self->deliver_destroy != NULL && self->deliver_data != NULL) {
        self->deliver_destroy(self->deliver_data);
        self->deliver_destroy = NULL;
        self->deliver_data = NULL;
    }

    g_clear_pointer(&self->tool_providers, g_ptr_array_unref);
    g_clear_object(&self->agents);
    g_clear_object(&self->tasks);
    g_clear_object(&self->guard);

    G_OBJECT_CLASS(clawt_mcp_tools_parent_class)->dispose(object);
}

static void
clawt_mcp_tools_class_init(ClawtMcpToolsClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_mcp_tools_dispose;
}

static void
clawt_mcp_tools_init(ClawtMcpTools *self)
{
    (void)self;
}
