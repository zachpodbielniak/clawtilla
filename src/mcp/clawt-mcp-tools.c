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
#include "chat/clawt-room-manager.h"
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
    NEEDS_DECISION_INBOX,
    NEEDS_PEER_COMMS,
    NEEDS_COMPUTER,
    NEEDS_MEMORY,
    NEEDS_FLEET_ADMIN,
    NEEDS_ASSIGNMENT
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

/*
 * The threshold is in the parameter descriptions, not only in the docs.
 *
 * A decision list that collects every small choice stops being read,
 * and then it is worse than nothing because it looks like the operator
 * has seen things they have not.  The failure mode is agents being
 * polite rather than selective, so the bar is stated where an agent
 * reads it at the moment it is deciding whether to file one.
 */
static const ClawtParamInfo ask_decision_params[] = {
    { "question", "string",
      "The choice, in one sentence. File this only if the branches "
      "produce materially different work, or the choice is not cheaply "
      "reversible. Everything else you simply decide.", TRUE },
    { "options", "string",
      "The choices, one per line. Two or three; if there are more, the "
      "question is not ready to be asked.", FALSE },
    { "default", "string",
      "What you will do if nobody answers. Required, and it is what "
      "makes this honest rather than a way of stalling: you carry on "
      "with this, and their answer redirects you.", TRUE },
    { "default_reason", "string",
      "Why that default. One sentence.", FALSE },
    { "reversible_hours", "number",
      "How many hours until your default stops being cheap to undo. "
      "Leave it out if it genuinely does not expire -- do not invent "
      "urgency, it reorders somebody else's list.", FALSE },
    { "task_id", "string",
      "The task this belongs to, if any, so the answer can find its way "
      "back to the right work.", FALSE }
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
    { "room_id", "string",
      "Which room, or an agent id to read your conversation with that "
      "agent.", TRUE },
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

static const ClawtParamInfo message_user_params[] = {
    { "body", "string", "What to tell them.", TRUE },
    { "attachments", "array",
      "Files to send with it, as paths you can read. The bytes are "
      "copied when you send, so you may delete or rewrite your own copy "
      "afterwards. Images are shown inline in the operator's client; "
      "anything else arrives as a file they can save. Use it for what "
      "you produced rather than what you are describing -- a screenshot, "
      "a rendered diagram, an exported log.", FALSE }
};

static const ClawtParamInfo memory_add_params[] = {
    { "content",    "string",
      "What to remember, written so it still makes sense to you in a "
      "month with none of this conversation around it.", TRUE },
    { "summary",    "string", "One line, for listings.", FALSE },
    { "category",   "string",
      "general, decision, preference, fact, project, learning, insight, "
      "todo, relationship, technical, workflow, debug, research, config "
      "or personal.", FALSE },
    { "importance", "string", "low, normal, high or critical.", FALSE },
    { "tags",       "string", "Comma-separated, for narrowing a search.",
      FALSE }
};

static const ClawtParamInfo memory_search_params[] = {
    { "query",    "string", "Words to look for.", TRUE },
    { "category", "string", "Narrow to one category.", FALSE },
    { "limit",    "integer", "How many at most.", FALSE },
    { "agent",    "string",
      "Whose memories to search. Defaults to your own; another agent's "
      "only if they have listed you in memory.readers.", FALSE }
};

static const ClawtParamInfo memory_list_params[] = {
    { "category",    "string", "Narrow to one category.", FALSE },
    { "pinned_only", "boolean", "Only the pinned ones.", FALSE },
    { "limit",       "integer", "How many at most.", FALSE }
};

static const ClawtParamInfo memory_id_params[] = {
    { "id", "string", "Which memory.", TRUE }
};

static const ClawtParamInfo memory_pin_params[] = {
    { "id",     "string", "Which memory.", TRUE },
    { "pinned", "boolean", "Pin it, or unpin it. Defaults to pinning.",
      FALSE }
};

static const ClawtParamInfo create_agent_params[] = {
    { "agent_id",    "string",
      "The id: lowercase, hyphenated, and how everything else will refer "
      "to it. It cannot be changed later.", TRUE },
    { "description", "string",
      "What this agent is for, in one line. The others see it when they "
      "are deciding who to delegate to, so write it for them.", TRUE },
    { "name",        "string", "Display name. Defaults to the id.", FALSE },
    { "purpose",     "string",
      "The persona, in prose: what it does, how it should work, anything "
      "it must never do. This becomes the agent's own instructions, so "
      "write it as though addressing them.", FALSE },
    { "provider",    "string",
      "Which backend. Call clawtilla_agent_options first -- only some "
      "can run an agent at all.", FALSE },
    { "model",       "string", "Which model from that provider.", FALSE },
    { "computer",    "string",
      "none, host, container or vm. Default none, which is chat only. "
      "A VM needs a disk image that has already been fetched.", FALSE },
    { "container_image", "string",
      "For computer=container: the image to run, e.g. fedora:latest.",
      FALSE },
    { "vm_image",    "string",
      "For computer=vm: the path of an image from "
      "clawtilla_agent_options. Do not invent one -- an image that is "
      "not there produces an agent that refuses to provision.", FALSE },
    { "settings",    "string",
      "Anything else, as key=value lines using the configuration keys "
      "clawtilla_agent_options lists. One per line.", FALSE },
    { "start",       "boolean",
      "Start it once it exists. Defaults to true, which is what builds "
      "its container or VM -- an agent that is never started has no "
      "machine.", FALSE }
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

    TOOL("clawtilla_agent_options",
         "What can be chosen when creating an agent: the providers that "
         "can run one and their models, the disk images that have "
         "actually been fetched, and the configuration keys you may pass "
         "in `settings`. Call this before clawtilla_create_agent -- it "
         "reports what exists rather than what sounds plausible.",
         NEEDS_FLEET_ADMIN, no_params),

    TOOL("clawtilla_create_agent",
         "Add a new agent to the fleet and start it. Use "
         "clawtilla_agent_options first. Creating an agent is not "
         "reversible from here -- say what you are about to make and why "
         "before you make it.",
         NEEDS_FLEET_ADMIN, create_agent_params),

    TOOL("clawtilla_get_agent",
         "Look up one agent: its description, state and what it can do.",
         NEEDS_NOTHING, get_agent_params),

    TOOL("clawtilla_message_agent",
         "Send a message to another agent. It is queued, so this works even "
         "if they are stopped -- they will see it when they start. Returns "
         "immediately without waiting for a reply.",
         NEEDS_PEER_COMMS, message_agent_params),

    TOOL("clawtilla_ask_agent",
         "Ask another agent a question. Their answer arrives as a message "
         "in your mailbox rather than as the result of this call -- "
         "nothing here blocks a turn waiting. If you need the answer "
         "before you can continue, use clawtilla_delegate and check "
         "clawtilla_task_status.",
         NEEDS_PEER_COMMS, ask_agent_params),

    TOOL("clawtilla_delegate",
         "Hand a piece of work to another agent and get a task id back. "
         "They work on it independently; check on it with "
         "clawtilla_task_status. Use this rather than asking, when the "
         "work will take a while. You can assign within your own team; "
         "for anything belonging to another team, send it to the chief of "
         "staff rather than to that team directly.",
         NEEDS_ASSIGNMENT, delegate_params),

    TOOL("clawtilla_ask_decision",
         "Ask your operator to make a choice, without waiting for them. "
         "You state what you will do anyway, and carry on doing it; "
         "their answer arrives later as a message and redirects you. "
         "Use this instead of stalling, and instead of deciding "
         "unilaterally on something they would want to have seen.",
         NEEDS_DECISION_INBOX, ask_decision_params),

    TOOL("clawtilla_fleet_cost",
         "What the fleet has spent so far, per agent and in total, and "
         "what one delegated task is allowed to spend. Worth checking "
         "before fanning work out widely -- delegating is spending.",
         NEEDS_NOTHING, no_params),

    TOOL("clawtilla_list_teams",
         "The teams in this fleet: what each is for, who leads it, who is "
         "on it and how many are running. Read the descriptions to decide "
         "where a piece of work belongs -- they say what a team handles, "
         "which is not always obvious from the names on it.",
         NEEDS_PEER_COMMS, no_params),

    TOOL("clawtilla_post_room",
         "Post a message to a room, reaching every member.",
         NEEDS_PEER_COMMS, post_room_params),

    TOOL("clawtilla_create_room",
         "Create a room with the given members, for work that several "
         "agents need to see.",
         NEEDS_PEER_COMMS, create_room_params),

    TOOL("clawtilla_room_history",
         "Read recent messages from a room, or -- given an agent id -- "
         "your conversation with that agent. This is how you see whether "
         "someone answered you; your mailbox will be empty because "
         "delivery empties it.",
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

    TOOL("clawtilla_message_user",
         "Say something to your human operator. This is the only way to "
         "reach them: replying to a message from another agent reaches "
         "that agent. If they asked you to find something out and a peer "
         "has now told you, report it with this -- otherwise they are "
         "left asking whether you ever heard back.",
         NEEDS_NOTHING, message_user_params),

    TOOL("clawtilla_memory_add",
         "Remember something. Use it for what you would be worse off not "
         "knowing next time: a decision and why, something the operator "
         "prefers, a fact that cost you a turn to establish, a footgun "
         "you hit. Not for what is already written down somewhere you "
         "can read again.",
         NEEDS_MEMORY, memory_add_params),

    TOOL("clawtilla_memory_search",
         "Search your memories. Do this before asking the operator "
         "something they may already have told you, and before working "
         "out something you may already have worked out.",
         NEEDS_MEMORY, memory_search_params),

    TOOL("clawtilla_memory_list",
         "Your most recent memories, pinned ones first.",
         NEEDS_MEMORY, memory_list_params),

    TOOL("clawtilla_memory_get",
         "One memory in full.",
         NEEDS_MEMORY, memory_id_params),

    TOOL("clawtilla_memory_forget",
         "Archive a memory that turned out to be wrong or is no longer "
         "true. It leaves every listing and search; a person can still "
         "go and look.",
         NEEDS_MEMORY, memory_id_params),

    TOOL("clawtilla_memory_pin",
         "Keep a memory at the top of every listing. For the handful you "
         "want to see every time, not for anything you merely think is "
         "important.",
         NEEDS_MEMORY, memory_pin_params),

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
    ClawtRoomManager  *room_manager;   /* unowned */

    GPtrArray *tool_providers;  /* GObject*, unowned */

    ClawtMcpDeliverFunc deliver;
    gpointer            deliver_data;
    GDestroyNotify      deliver_destroy;

    ClawtMcpCreateAgentFunc create_agent;
    gpointer                create_agent_data;
    GDestroyNotify          create_agent_destroy;

    ClawtMcpAskDecisionFunc ask_decision;
    gpointer                ask_decision_data;
    GDestroyNotify          ask_decision_destroy;

    ClawtVmImageStore *images;   /* unowned */
    ClawtEventBus     *bus;      /* unowned */

    gchar *attachment_dir;
};

G_DEFINE_FINAL_TYPE(ClawtMcpTools, clawt_mcp_tools, G_TYPE_OBJECT)

void
clawt_mcp_tools_set_create_agent_func(ClawtMcpTools           *self,
                                      ClawtMcpCreateAgentFunc  func,
                                      gpointer                 user_data,
                                      GDestroyNotify           destroy)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    if (self->create_agent_destroy != NULL && self->create_agent_data != NULL)
        self->create_agent_destroy(self->create_agent_data);

    self->create_agent = func;
    self->create_agent_data = user_data;
    self->create_agent_destroy = destroy;
}

void
clawt_mcp_tools_set_ask_decision_func(ClawtMcpTools           *self,
                                      ClawtMcpAskDecisionFunc  func,
                                      gpointer                 user_data,
                                      GDestroyNotify           destroy)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    if (self->ask_decision_destroy != NULL && self->ask_decision_data != NULL)
        self->ask_decision_destroy(self->ask_decision_data);

    self->ask_decision = func;
    self->ask_decision_data = user_data;
    self->ask_decision_destroy = destroy;
}

void
clawt_mcp_tools_set_image_store(ClawtMcpTools *self, ClawtVmImageStore *store)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    self->images = store;
}

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
clawt_mcp_tools_set_attachment_dir(ClawtMcpTools *self, const gchar *dir)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    g_free(self->attachment_dir);
    self->attachment_dir = g_strdup(dir);
}

void
clawt_mcp_tools_set_event_bus(ClawtMcpTools *self, ClawtEventBus *bus)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    self->bus = bus;
}

void
clawt_mcp_tools_set_room_manager(ClawtMcpTools    *self,
                                 ClawtRoomManager *rooms)
{
    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));

    self->room_manager = rooms;
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

/*
 * How far a message sent by this agent has travelled.
 *
 * One hop beyond whatever it is replying to.  Every tool used to pass a
 * literal 1, which made the hop limit unreachable: a chain twenty agents
 * long still looked like twenty separate first messages.
 */
static gint
outbound_depth(ClawtMcpTools *self, const gchar *agent_id)
{
    ClawtAgent *agent = (self->agents != NULL)
                        ? clawt_agent_manager_get(self->agents, agent_id)
                        : NULL;

    return (agent != NULL) ? clawt_agent_get_hop_depth(agent) + 1 : 1;
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

    case NEEDS_MEMORY:
        /*
         * Not offered when there is no store, rather than offered and
         * failing: an agent that can see a tool will try it, and the
         * refusal costs a turn to learn what it could have been told.
         */
        if (clawt_agent_get_memory(agent) == NULL)
            return FALSE;
        break;

    case NEEDS_PEER_COMMS:
        if ((clawt_agent_get_caps(agent) & CLAWT_AGENT_CAPS_PEER_COMMS) == 0)
            return FALSE;
        break;

    case NEEDS_ASSIGNMENT:
        /*
         * Offered only to an agent that can assign to *somebody*: the
         * chief of staff, or a team lead. A member never can, and a tool
         * it can only ever be refused for is a tool it will try, be told
         * no, and try again in a different shape.
         *
         * Which *particular* target is allowed is settled at call time,
         * because it depends on the target rather than on the caller.
         */
        {
            ClawtAgentConfig *mine = clawt_agent_get_config(agent);

            if (!clawt_agent_config_get_boolean(mine, "chief_of_staff") &&
                clawt_team_role_of(mine) != CLAWT_TEAM_LEAD)
                return FALSE;
        }

        if ((clawt_agent_get_caps(agent) & CLAWT_AGENT_CAPS_PEER_COMMS) == 0)
            return FALSE;
        break;

    case NEEDS_FLEET_ADMIN:
        /*
         * Two conditions, and both have to hold.  There must be
         * something to create agents *with* -- a library embedded
         * without a daemon has no fleet to add to -- and this agent must
         * have been given the permission, which is off by default
         * because an agent that can create agents can give one a
         * machine that runs code.
         */
        if (self->create_agent == NULL)
            return FALSE;

        if (!clawt_agent_config_get_boolean(clawt_agent_get_config(agent),
                                            "tools.manage_fleet"))
            return FALSE;
        break;

    case NEEDS_DECISION_INBOX:
        /*
         * Only the hook, and no permission beyond it.
         *
         * Filing a question is not a fleet operation -- it spends an
         * operator's attention rather than their machine -- so there is
         * nothing here worth gating, and gating it would push agents
         * back to the two bad options this replaces: stall on the
         * operator, or decide unilaterally and hope.
         *
         * The hook is still required, because a library embedded
         * without a daemon has no inbox to file into and a tool that is
         * listed and then fails teaches an agent to keep trying.
         */
        if (self->ask_decision == NULL)
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

/*
 * The same question tools/list answers, written for a person and for the
 * agent's own prompt.
 *
 * It goes through the same gate rather than walking the table, so
 * the file says what the agent can actually call. TOOLS.org used to
 * carry a table written when the workspace was scaffolded, which meant a
 * tool granted later never appeared there -- and an agent asked whether
 * it could create agents read its own file and said no, on the day the
 * tool was added to it.
 */
gchar *
clawt_mcp_tools_describe_for_agent(ClawtMcpTools *self, const gchar *agent_id)
{
    g_autoptr(GString) out = g_string_new(NULL);
    guint offered = 0;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_MCP_TOOLS(self), NULL);

    /*
     * Where this agent stands before what it can do, because the tools
     * below mean different things depending on it -- and because an
     * agent that does not know it leads a team will not behave like it
     * does.
     *
     * Regenerated with the rest of the region, so a change of team or
     * role reaches the file rather than waiting for somebody to
     * remember.
     */
    {
        ClawtAgent *me = (self->agents != NULL)
                         ? clawt_agent_manager_get(self->agents, agent_id)
                         : NULL;
        ClawtAgentConfig *mine = (me != NULL)
                                 ? clawt_agent_get_config(me) : NULL;
        const gchar *team = (mine != NULL)
                            ? clawt_agent_config_get_string(mine, "team")
                            : NULL;

        g_string_append(out, "* Where you sit\n\n");

        if (mine != NULL &&
            clawt_agent_config_get_boolean(mine, "chief_of_staff")) {
            g_string_append(out,
                "You are the *chief of staff*. You are the lead of every\n"
                "team, and dividing work between them is the job. Read\n"
                "~clawtilla_list_teams~ before you place anything: the\n"
                "descriptions say what each team handles, which the names\n"
                "on it do not.\n\n"
                "Hand work to a team's *lead*, not to somebody on it. The\n"
                "lead knows what their people are in the middle of and\n"
                "you do not. A team with no lead is the exception -- you\n"
                "assign into it directly, because nobody else can.\n\n");
        } else if (team == NULL || *team == '\0') {
            g_string_append(out,
                "You are on no team. You take work from the chief of\n"
                "staff, and you may message, ask and share a room with\n"
                "anybody here. You cannot assign work to anyone.\n\n");
        } else if (clawt_team_role_of(mine) == CLAWT_TEAM_LEAD) {
            g_string_append_printf(out,
                "You *lead* the ~%s~ team. Work for it arrives from the\n"
                "chief of staff, and it is yours to place: use\n"
                "~clawtilla_delegate~ on the people on your team, and\n"
                "~clawtilla_list_teams~ to see who they are and who is\n"
                "running.\n\n"
                "You cannot assign outside your own team. Something that\n"
                "belongs elsewhere goes back to the chief of staff, who\n"
                "hands it to the right lead -- do not go around them by\n"
                "messaging another team's people and asking nicely.\n\n",
                team);
        } else {
            g_string_append_printf(out,
                "You are on the ~%s~ team. Work reaches you from your\n"
                "team's lead or from the chief of staff.\n\n"
                "You cannot assign work to anybody, and that is the only\n"
                "restriction: you can message, ask and share a room with\n"
                "any agent in the fleet, on your team or not. Handing\n"
                "something over in conversation, asking somebody who\n"
                "knows, and working on something together are all fine\n"
                "and are what the fleet is for. If a piece of work needs\n"
                "to go on somebody's list, tell your lead.\n\n",
                team);
        }
    }

    g_string_append(out, "* The tools clawtilla is giving you\n\n");

    for (i = 0; i < G_N_ELEMENTS(tools); i++) {
        if (!clawt_mcp_tools_is_permitted(self, agent_id,
                                          tools[i].name))
            continue;

        if (offered == 0)
            g_string_append(out,
                "| Tool | What it does |\n"
                "|------+--------------|\n");

        g_string_append_printf(out, "| ~%s~ | %s |\n", tools[i].name,
                               tools[i].description);
        offered++;
    }

    if (offered == 0) {
        /*
         * Said rather than left blank. An empty list reads as "clawtilla
         * has not worked this out yet", and an agent that suspects it
         * has unlisted tools goes looking for them.
         */
        g_string_append(out,
            "None. You have no orchestration tools at all -- you cannot\n"
            "reach the other agents, and there is nothing here to try.\n");
    } else {
        g_string_append(out,
            "\nThis list is regenerated every time you start, so it is\n"
            "what you have now. If something you expect is missing it has\n"
            "not been granted -- say so rather than looking for another\n"
            "way round, because there is not one.\n");
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
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

static gboolean
argument_boolean(JsonObject *arguments, const gchar *name, gboolean fallback)
{
    if (arguments == NULL || !json_object_has_member(arguments, name))
        return fallback;

    /*
     * Type-checked like the others, and lenient about a model that sends
     * "true" as a string -- which they do, and which reading strictly
     * would turn into a silent FALSE.
     */
    if (json_node_get_value_type(json_object_get_member(arguments, name)) ==
        G_TYPE_BOOLEAN)
        return json_object_get_boolean_member(arguments, name);

    if (json_node_get_value_type(json_object_get_member(arguments, name)) ==
        G_TYPE_STRING) {
        const gchar *text = json_object_get_string_member(arguments, name);

        return g_ascii_strcasecmp(text, "true") == 0 ||
               g_strcmp0(text, "1") == 0 ||
               g_ascii_strcasecmp(text, "yes") == 0;
    }

    return fallback;
}

static gint64
argument_int(JsonObject *arguments, const gchar *name, gint64 fallback)
{
    if (arguments == NULL || !json_object_has_member(arguments, name))
        return fallback;

    /*
     * Type-checked, like argument_string.  Reading a string member as an
     * integer returns 0 rather than the caller's fallback, so a model
     * sending "timeout": "soon" silently got a zero timeout instead of
     * the documented 120 seconds.
     */
    if (json_node_get_value_type(json_object_get_member(arguments, name)) !=
        G_TYPE_INT64)
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


/*
 * What the fleet has spent, for an agent that is deciding whether to
 * spend more.
 *
 * A chief-of-staff handing out ten subtasks is committing real money,
 * and until now nothing in its tool surface could tell it how much --
 * so "delegate this to five agents" and "delegate this to fifty" felt
 * identical from where it sat.  Reported per agent, because the useful
 * answer is nearly always which one is most of it.
 */
static gchar *
tool_fleet_cost(ClawtMcpTools *self, const gchar *agent_id)
{
    g_autoptr(GString) out = g_string_new(NULL);
    ClawtUsageTotals fleet = { 0, 0, 0, 0 };
    ClawtConfig *config;
    GPtrArray *agents;
    gdouble budget;
    guint i;

    (void)agent_id;

    config = (self->agents != NULL)
             ? clawt_agent_manager_get_config(self->agents) : NULL;

    if (config == NULL)
        return g_strdup("This fleet has no configuration to read spending "
                        "from.");

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        const gchar *id = clawt_agent_get_id(agent);
        g_autofree gchar *state_dir = NULL;
        g_autofree gchar *db_path = NULL;
        g_autofree gchar *cost = NULL;
        ClawtUsageTotals totals = { 0, 0, 0, 0 };

        state_dir = clawt_config_agent_state_dir(config, id);
        if (state_dir == NULL)
            continue;

        db_path = clawt_usage_database_path(state_dir);
        clawt_usage_read_totals(db_path, 0, &totals, NULL);
        clawt_usage_totals_add(&fleet, &totals);

        cost = clawt_usage_format_cost(totals.cost_micros);
        g_string_append_printf(out, "%s: %s over %" G_GINT64_FORMAT
                                    " turns\n", id, cost, totals.turns);
    }

    {
        g_autofree gchar *cost = clawt_usage_format_cost(fleet.cost_micros);

        g_string_append_printf(out, "\nFleet total: %s over %"
                                    G_GINT64_FORMAT " turns.\n", cost,
                               fleet.turns);
    }

    budget = clawt_config_get_double(config, "orchestration.task_budget_usd");

    if (budget > 0.0)
        g_string_append_printf(
            out,
            "Each delegated task may spend $%.2f before its messages are "
            "refused, so a task near that figure needs splitting rather "
            "than retrying.\n", budget);

    /*
     * Said here as well as in the clients, because an agent asked to
     * reconcile this against an invoice would otherwise report a
     * discrepancy it cannot explain.
     */
    g_string_append(out,
                    "\nThese are the figures the provider reported per "
                    "turn. They are money actually billed; the token "
                    "counts elsewhere exclude cached context, which is "
                    "billed but not reported as tokens.\n");

    return g_string_free(g_steal_pointer(&out), FALSE);
}


/*
 * The teams, written for whoever is deciding where a piece of work goes.
 *
 * The descriptions are the point. A chief-of-staff choosing between
 * teams has their names and the agents on them, and neither says what a
 * team actually handles -- so the description is what it reads, and it
 * comes first in each entry for that reason.
 */
static gchar *
tool_list_teams(ClawtMcpTools *self, const gchar *agent_id)
{
    g_autoptr(GString) out = g_string_new(NULL);
    g_autoptr(GPtrArray) teams = NULL;
    ClawtConfig *config;
    GPtrArray *agents;
    ClawtAgent *me;
    const gchar *my_team = NULL;
    guint i;

    config = (self->agents != NULL)
             ? clawt_agent_manager_get_config(self->agents) : NULL;

    if (config == NULL)
        return g_strdup("This fleet has no configuration to read teams "
                        "from.");

    teams = clawt_config_get_teams(config);
    agents = clawt_agent_manager_list(self->agents);

    me = clawt_agent_manager_get(self->agents, agent_id);

    if (me != NULL)
        my_team = clawt_agent_config_get_string(clawt_agent_get_config(me),
                                                "team");

    if (teams->len == 0) {
        /*
         * Said rather than left blank. A fleet with no teams is a normal
         * fleet, and an agent that suspects there are teams it cannot
         * see will go looking for them.
         */
        return g_strdup("This fleet has no teams. Everyone is in one "
                        "group, and only the chief of staff assigns work.");
    }

    for (i = 0; i < teams->len; i++) {
        ClawtTeamSpec *team = g_ptr_array_index(teams, i);
        g_autoptr(GString) members = g_string_new(NULL);
        const gchar *lead = NULL;
        guint running = 0;
        guint total = 0;
        guint j;

        for (j = 0; agents != NULL && j < agents->len; j++) {
            ClawtAgent *agent = g_ptr_array_index(agents, j);
            ClawtAgentConfig *agent_config = clawt_agent_get_config(agent);

            if (g_strcmp0(clawt_agent_config_get_string(agent_config,
                                                        "team"),
                          team->id) != 0)
                continue;

            total++;

            if (clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_RUNNING)
                running++;

            if (clawt_team_role_of(agent_config) == CLAWT_TEAM_LEAD)
                lead = clawt_agent_get_id(agent);
            else
                g_string_append_printf(members, "%s%s",
                                       members->len > 0 ? ", " : "",
                                       clawt_agent_get_id(agent));
        }

        g_string_append_printf(out, "* %s (%s)%s\n",
                               team->name != NULL ? team->name : team->id,
                               team->id,
                               g_strcmp0(my_team, team->id) == 0
                                   ? "  -- yours" : "");

        if (team->description != NULL && *team->description != '\0')
            g_string_append_printf(out, "  %s\n", team->description);
        else
            g_string_append(out,
                "  No description, so there is nothing here to match work "
                "against. Worth asking the user for one.\n");

        if (lead != NULL)
            g_string_append_printf(out, "  Lead: %s -- send work here.\n",
                                   lead);
        else
            g_string_append(out,
                "  No lead, so work for this team has to go to the chief "
                "of staff.\n");

        g_string_append_printf(out, "  Members: %s\n",
                               members->len > 0 ? members->str
                                                : "nobody yet");
        g_string_append_printf(out, "  Running: %u of %u\n\n",
                               running, total);
    }

    g_string_append(out,
        "Work belongs to a team, not to a person: hand it to the lead and "
        "let them choose who does it. They know what their people are in "
        "the middle of and you do not.\n");

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Growing the fleet ───────────────────────────────────────────── */

/*
 * What can actually be chosen, rather than what sounds plausible.
 *
 * An agent asked to create another will otherwise invent a provider, a
 * model name and a disk image path, and every one of those produces an
 * agent that looks created and does not work -- a disk image most of
 * all, because the images that exist are the ones somebody fetched.
 * The designer hit exactly this and had to be stopped from choosing
 * `vm` at all; this is the other answer to the same problem.
 */
static gchar *
tool_agent_options(ClawtMcpTools *self)
{
    g_autoptr(GString) out = g_string_new(NULL);
    const ClawtProviderInfo *providers;
    gsize n_providers = 0;
    gsize i;

    g_string_append(out,
        "Computer types: none (chat only), host (this machine, confined), "
        "container, vm.\n\n");

    g_string_append(out, "Providers that can run an agent:\n");

    providers = clawt_model_catalog_get(&n_providers);

    for (i = 0; i < n_providers; i++) {
        gsize j;

        /*
         * Only the ones that can be an agent.  The HTTP providers exist
         * for the designer, which needs tool calls, and naming them here
         * would produce an agent configured for something that quietly
         * runs as Claude Code instead.
         */
        if (!providers[i].agent)
            continue;

        g_string_append_printf(out, "  %s (%s)", providers[i].id,
                               providers[i].label);

        if (providers[i].n_models > 0) {
            g_string_append(out, " -- models:");

            for (j = 0; j < providers[i].n_models; j++)
                g_string_append_printf(out, "%s %s", j > 0 ? "," : "",
                                       providers[i].models[j].id);
        }

        g_string_append_c(out, '\n');
    }

    g_string_append(out, "\nDisk images already fetched");

    if (self->images == NULL) {
        g_string_append(out,
            ": unknown from here. Without one, create a container agent "
            "rather than a VM.\n");
    } else {
        g_autoptr(GPtrArray) have = clawt_vm_image_store_list(self->images);
        guint k;
        guint ready = 0;

        g_string_append(out, " (use the path exactly):\n");

        for (k = 0; have != NULL && k < have->len; k++) {
            ClawtVmImage *image = g_ptr_array_index(have, k);

            /*
             * Only the ones that have finished arriving. Naming a
             * half-downloaded file would produce an agent that refuses
             * to provision for a reason nobody could see.
             */
            if (image->path == NULL || image->downloading)
                continue;

            g_string_append_printf(out, "  %s\n", image->path);
            ready++;
        }

        if (ready == 0) {
            g_string_append(out,
                "  none. A VM agent cannot be created until one is "
                "fetched -- tell the user, and make a container agent "
                "instead if that will do.\n");
        }
    }

    g_string_append(out,
        "\nKeys you may pass in `settings`, as key=value lines:\n");

    {
        gsize n_entries = 0;
        const ClawtSchemaEntry *schema = clawt_config_schema_get(&n_entries);

        for (i = 0; i < n_entries; i++) {
            const gchar *key = schema[i].key;
            g_autofree gchar *first = NULL;
            const gchar *newline;

            if (!g_str_has_prefix(key, "agents."))
                continue;

            if (schema[i].type == CLAWT_SCHEMA_SECTION ||
                schema[i].type == CLAWT_SCHEMA_MAPPING ||
                schema[i].type == CLAWT_SCHEMA_LIST_OF)
                continue;

            /*
             * Not the id: it is a separate argument, and offering it
             * here as well invites setting it to something other than
             * the agent being created -- which would name one agent and
             * configure another.
             */
            if (g_strcmp0(key, "agents.id") == 0)
                continue;

            /* One line each: the whole table is long enough as it is. */
            newline = (schema[i].doc != NULL)
                      ? strchr(schema[i].doc, '\n') : NULL;
            first = (schema[i].doc == NULL)
                    ? g_strdup("")
                    : (newline != NULL
                       ? g_strndup(schema[i].doc,
                                   (gsize)(newline - schema[i].doc))
                       : g_strdup(schema[i].doc));

            g_string_append_printf(out, "  %s = %s  -- %s\n",
                                   key + strlen("agents."),
                                   schema[i].default_value != NULL
                                   ? schema[i].default_value : "unset",
                                   first);
        }
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * An agent files a choice for its operator, and keeps working.
 *
 * The default is required rather than optional, and refusing without
 * one is the whole design: an item with no stated default is a stalled
 * piece of work with a nicer name, and a list of those trains an
 * operator to stop reading it.
 */
static gchar *
tool_ask_decision(ClawtMcpTools *self,
                  const gchar   *agent_id,
                  JsonObject    *arguments,
                  gboolean      *is_error)
{
    const gchar *question = argument_string(arguments, "question");
    const gchar *options = argument_string(arguments, "options");
    const gchar *fallback = argument_string(arguments, "default");
    const gchar *reason = argument_string(arguments, "default_reason");
    const gchar *task = argument_string(arguments, "task_id");
    gint64 hours = argument_int(arguments, "reversible_hours", 0);
    g_autoptr(ClawtDecision) decision = NULL;
    g_autoptr(GError) error = NULL;
    gchar *answer;

    if (question == NULL || *question == '\0') {
        *is_error = TRUE;
        return g_strdup("A decision needs a question.");
    }

    if (fallback == NULL || *fallback == '\0') {
        *is_error = TRUE;
        return g_strdup(
            "A decision needs a default -- what you will do if nobody "
            "answers. Without one this is a way of stalling rather than "
            "a question, and an inbox of stalled work stops being read. "
            "Decide what you would do, say so here, and carry on doing "
            "it.");
    }

    decision = clawt_decision_new(NULL, agent_id, question);
    clawt_decision_set_default(decision, fallback, reason);
    clawt_decision_set_task(decision, task);

    if (options != NULL && *options != '\0') {
        g_auto(GStrv) split = g_strsplit(options, "\n", -1);

        clawt_decision_set_options(decision,
                                   (const gchar * const *)split);
    }

    /*
     * Hours from now, resolved here rather than asking the agent for a
     * timestamp.  A model asked for an absolute time writes a plausible
     * one in the wrong year often enough to matter, and "how long until
     * this is hard to undo" is the thing it actually knows.
     */
    if (hours > 0)
        clawt_decision_set_reversible_until(
            decision,
            (g_get_real_time() / G_USEC_PER_SEC) + (hours * 3600));

    answer = self->ask_decision(agent_id, decision, self->ask_decision_data,
                                &error);

    if (answer == NULL) {
        *is_error = TRUE;
        return g_strdup(error != NULL ? error->message
                                      : "the decision could not be filed");
    }

    return answer;
}

static gchar *
tool_create_agent(ClawtMcpTools *self,
                  JsonObject    *arguments,
                  gboolean      *is_error)
{
    const gchar *agent_id = argument_string(arguments, "agent_id");
    const gchar *description = argument_string(arguments, "description");
    const gchar *settings_text = argument_string(arguments, "settings");
    g_autoptr(GHashTable) settings = NULL;
    g_autoptr(GError) error = NULL;
    gchar *result;
    gsize i;

    static const struct {
        const gchar *argument;
        const gchar *key;
    } direct[] = {
        { "name",            "name" },
        { "description",     "description" },
        { "provider",        "model.provider" },
        { "model",           "model.model" },
        { "computer",        "computer.type" },
        { "container_image", "computer.container.image" },
        { "vm_image",        "computer.vm.image" },
        { NULL, NULL }
    };

    if (agent_id == NULL || *agent_id == '\0') {
        *is_error = TRUE;
        return g_strdup("agent_id is required, and it cannot be changed "
                        "afterwards.");
    }

    if (description == NULL || *description == '\0') {
        *is_error = TRUE;
        return g_strdup("description is required: it is what the other "
                        "agents read when they are deciding who to "
                        "delegate to.");
    }

    settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    for (i = 0; direct[i].argument != NULL; i++) {
        const gchar *value = argument_string(arguments, direct[i].argument);

        if (value != NULL && *value != '\0')
            g_hash_table_insert(settings, g_strdup(direct[i].key),
                                g_strdup(value));
    }

    /*
     * The free-form half, so every option in the schema is reachable
     * without a list here that would drift from it the first time
     * somebody added a key.
     */
    if (settings_text != NULL && *settings_text != '\0') {
        g_auto(GStrv) lines = g_strsplit(settings_text, "\n", -1);
        gsize line;

        for (line = 0; lines[line] != NULL; line++) {
            g_autofree gchar *trimmed = g_strdup(g_strstrip(lines[line]));
            gchar *equals;

            if (*trimmed == '\0' || *trimmed == '#')
                continue;

            equals = strchr(trimmed, '=');

            if (equals == NULL) {
                *is_error = TRUE;
                return g_strdup_printf(
                    "settings line '%s' is not key=value. One per line, "
                    "using the keys clawtilla_agent_options lists.",
                    trimmed);
            }

            *equals = '\0';
            g_strstrip(trimmed);

            /*
             * Refused rather than ignored. Silently dropping it would
             * leave the agent believing it had named the thing it just
             * made.
             */
            if (g_strcmp0(trimmed, "id") == 0) {
                *is_error = TRUE;
                return g_strdup("the id is the agent_id argument, not a "
                                "setting -- setting it here would name one "
                                "agent and configure another.");
            }

            g_hash_table_insert(settings, g_strdup(trimmed),
                                g_strdup(g_strstrip(equals + 1)));
        }
    }

    /*
     * The persona travels on its own, not as a setting.
     *
     * Everything else here has a default that works; what the new agent
     * is *for* does not.  It used to be inserted into @settings as
     * `persona`, which is a section in the schema rather than a value --
     * so it was written to the config file, read by nothing, and the
     * whole persona an operator had written was discarded in silence.
     */
    result = self->create_agent(agent_id,
                                argument_string(arguments, "purpose"),
                                settings,
                                argument_boolean(arguments, "start", TRUE),
                                self->create_agent_data, &error);

    if (result == NULL) {
        *is_error = TRUE;
        return g_strdup(error != NULL ? error->message
                                      : "the agent could not be created");
    }

    return result;
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

/*
 * The delivery band a `priority` argument names.
 *
 * The argument is read here and judged in clawt_message_priority_from_nick(),
 * which is also what a pod's `message_agent` calls.  Two parsers would be
 * two vocabularies and two answers to what happens to "P1"; the one that
 * gets less use would be the one that is wrong.
 */
static gboolean
priority_from_arguments(JsonObject     *arguments,
                        ClawtPriority  *out_priority,
                        gchar         **out_refusal)
{
    return clawt_message_priority_from_nick(
        argument_string(arguments, "priority"), out_priority, out_refusal);
}

static gchar *
tool_message_agent(ClawtMcpTools *self,
                   const gchar   *agent_id,
                   JsonObject    *arguments,
                   gboolean      *is_error)
{
    const gchar *target = argument_string(arguments, "agent_id");
    const gchar *body = argument_string(arguments, "body");
    ClawtPriority priority = CLAWT_PRIORITY_NORMAL;
    g_autofree gchar *refusal = NULL;
    g_autoptr(GError) error = NULL;

    /*
     * clawtilla_ask_agent's schema calls it "message"; both are accepted
     * so a model following either description works.  The two used to
     * disagree, which meant every schema-conforming ask_agent call failed.
     */
    if (body == NULL)
        body = argument_string(arguments, "message");

    if (target == NULL || body == NULL) {
        *is_error = TRUE;
        return g_strdup("agent_id and body (or message) are both required.");
    }

    /*
     * Checked before anything is queued.  A message that went out at the
     * wrong band and then reported a bad priority would leave the agent
     * with no way to correct it -- resending is the only remedy it has,
     * and that would deliver the thing twice.
     */
    if (!priority_from_arguments(arguments, &priority, &refusal)) {
        *is_error = TRUE;
        return g_steal_pointer(&refusal);
    }

    if (self->deliver == NULL) {
        *is_error = TRUE;
        return g_strdup("Messaging is not available.");
    }

    if (!self->deliver(agent_id, target, body, NULL,
                       outbound_depth(self, agent_id), priority,
                       self->deliver_data, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    /*
     * The band is named back only when it is not the ordinary one.
     *
     * Saying "queued as normal" on every call trains an agent to skim
     * the sentence, and the line that matters -- that this one did jump
     * the queue -- is the one that would then be skimmed.  It is safe to
     * claim now because the band reaches the mailbox: while it did not,
     * this reply deliberately said nothing, since an agent told its
     * message was expedited has no reason to look again.
     */
    if (priority != CLAWT_PRIORITY_NORMAL)
        return g_strdup_printf("Queued for %s at %s priority. They will see "
                               "it when they are next running.", target,
                               clawt_enum_to_nick(CLAWT_TYPE_PRIORITY,
                                                  priority));

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

    /*
     * Checked here and not only when the tool was offered, because who
     * may assign to *whom* depends on the target: a lead has the tool
     * and still may not reach outside its own team. The refusal carries
     * the reason and says what to do instead -- an agent told only "no"
     * tries the same thing in a different shape.
     */
    {
        ClawtAgent *from = clawt_agent_manager_get(self->agents, agent_id);
        ClawtAgent *to = clawt_agent_manager_get(self->agents, assignee);
        g_autofree gchar *refusal = NULL;

        if (!clawt_team_may_assign(
                from != NULL ? clawt_agent_get_config(from) : NULL,
                to != NULL ? clawt_agent_get_config(to) : NULL, &refusal)) {
            *is_error = TRUE;
            return g_strdup(refusal != NULL ? refusal
                                            : "that is not yours to assign");
        }
    }

    task = clawt_task_manager_create(self->tasks, agent_id, assignee, work,
                                     NULL, &error);
    if (task == NULL) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    clawt_task_set_reason(task, reason);

    if (!self->deliver(agent_id, assignee, work, clawt_task_get_id(task),
                       outbound_depth(self, agent_id), CLAWT_PRIORITY_NORMAL,
                       self->deliver_data, &error)) {
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

/*
 * One `computer.exec` event, in the client path's own shape.
 *
 * The subject is the *agent*, and the details are the command and the
 * exit status and nothing else.  Verbatim from the client exec handler
 * on purpose: an audit trail whose two writers disagree about the
 * subject cannot be filtered by agent, and the whole reason this exists
 * is that half of it -- an agent running its own command -- reached no
 * bus at all while `docs/security.org` said both were recorded.
 *
 * No stdout, no stderr, no environment.  Nothing may write a secret's
 * value into a log line, and a command's output is the likeliest place
 * in this whole path for one to appear.
 */
static void
publish_exec(ClawtMcpTools *self, const gchar *agent_id,
             const gchar *command, gint exit_status)
{
    ClawtEvent *event;

    if (self->bus == NULL)
        return;

    event = clawt_event_new("computer.exec", agent_id);
    clawt_event_set_detail(event, "command", command);
    clawt_event_set_detail_int(event, "exit", exit_status);
    clawt_event_bus_publish(self->bus, event);
    clawt_event_free(event);
}

/*
 * One computer_exec, in three parts.
 *
 * The middle part is the only one that waits, and it is the one whose
 * duration the agent chooses rather than clawtilla: a build, a test run,
 * a command that reads from a terminal that is not there.  Splitting it
 * out is what lets the daemon run it on a worker thread and keep
 * dispatching -- see clawt_mcp_tools_call_async().
 *
 * The two ends stay on the main thread deliberately.  Looking an agent
 * up walks the manager's table and publishing walks the bus's handler
 * list, and both are touched by every other source on that context; the
 * worker touches nothing but the computer it was handed.  That is the
 * same division the agent-start path already draws.
 */
typedef struct {
    ClawtMcpTools  *tools;
    gchar          *agent_id;
    gchar          *command;
    gchar          *working_dir;
    guint           timeout;
    GStrv           argv;
    ClawtComputer  *computer;
    ClawtExecResult *result;
    GError         *error;
} ExecCall;

static void
exec_call_free(ExecCall *call)
{
    if (call == NULL)
        return;

    g_clear_object(&call->computer);
    g_clear_pointer(&call->result, clawt_exec_result_free);
    g_clear_error(&call->error);
    g_strfreev(call->argv);
    g_free(call->working_dir);
    g_free(call->command);
    g_free(call->agent_id);
    g_free(call);
}

/*
 * Everything that can be refused without waiting.
 *
 * Returns %NULL and sets @refusal when the call cannot proceed, so a bad
 * command never reaches a thread.
 */
static ExecCall *
exec_call_prepare(ClawtMcpTools *self, const gchar *agent_id,
                  JsonObject *arguments, gchar **refusal)
{
    const gchar *command = argument_string(arguments, "command");
    const gchar *working_dir = argument_string(arguments, "working_dir");
    ClawtAgent *agent;
    ClawtComputer *computer;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;
    ExecCall *call;

    if (command == NULL) {
        *refusal = g_strdup("command is required.");
        return NULL;
    }

    agent = clawt_agent_manager_get(self->agents, agent_id);
    computer = (agent != NULL) ? clawt_agent_get_computer(agent) : NULL;

    if (computer == NULL) {
        *refusal = g_strdup("You have no computer to run commands on.");
        return NULL;
    }

    if (!g_shell_parse_argv(command, NULL, &argv, &error)) {
        *refusal = g_strdup_printf("That command could not be parsed: %s",
                                   error->message);
        return NULL;
    }

    call = g_new0(ExecCall, 1);
    call->tools = self;
    call->agent_id = g_strdup(agent_id);
    call->command = g_strdup(command);
    call->working_dir = g_strdup(working_dir);
    call->timeout = (guint)argument_int(arguments, "timeout", 120);
    call->argv = g_steal_pointer(&argv);

    /*
     * A reference of its own.  The agent can be stopped while the command
     * is still running, and the worker must not be left holding a
     * computer the manager has dropped.
     */
    call->computer = g_object_ref(computer);

    return call;
}

/* The blocking half.  Safe to call from a worker thread. */
static void
exec_call_run(ExecCall *call)
{
    call->result = clawt_computer_exec(call->computer,
                                       (const gchar * const *)call->argv,
                                       call->working_dir, call->timeout,
                                       NULL, &call->error);
}

/* Back on the main thread: the trail, then the answer. */
static gchar *
exec_call_reply(ExecCall *call, gboolean *is_error)
{
    if (call->result == NULL) {
        /*
         * Recorded before the refusal is returned.  A command that never
         * ran is exactly the one somebody looks up afterwards, and a
         * trail that only holds the successes answers the wrong
         * question -- so the exit is -1 rather than the entry being
         * absent, which reads as "we do not know what it did", not as
         * "it did not happen".
         */
        publish_exec(call->tools, call->agent_id, call->command, -1);

        *is_error = TRUE;
        return g_strdup(call->error->message);
    }

    publish_exec(call->tools, call->agent_id, call->command,
                 clawt_exec_result_get_exit_status(call->result));

    if (clawt_exec_result_succeeded(call->result))
        return g_strdup(clawt_exec_result_get_stdout(call->result));

    /*
     * Exit status and stderr together.  A failing command whose reply is
     * only its stdout tells the agent nothing about why it failed.
     */
    *is_error = TRUE;
    return g_strdup_printf("Exit status %d.\n%s%s",
                           clawt_exec_result_get_exit_status(call->result),
                           clawt_exec_result_get_stderr(call->result),
                           clawt_exec_result_get_stdout(call->result));
}

/*
 * The synchronous form, for callers that have no way to wait.
 *
 * It holds whichever thread it is called on for the length of the
 * command, which is why the daemon's own tool.rpc path does not use it.
 */
static gchar *
tool_computer_exec(ClawtMcpTools *self,
                   const gchar   *agent_id,
                   JsonObject    *arguments,
                   gboolean      *is_error)
{
    ExecCall *call;
    g_autofree gchar *refusal = NULL;
    gchar *text;

    call = exec_call_prepare(self, agent_id, arguments, &refusal);

    if (call == NULL) {
        *is_error = TRUE;
        return g_steal_pointer(&refusal);
    }

    exec_call_run(call);
    text = exec_call_reply(call, is_error);
    exec_call_free(call);

    return text;
}

static gchar *
tool_computer_state(ClawtMcpTools *self, const gchar *agent_id)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent == NULL)
        return g_strdup("You have no computer.");

    return clawt_agent_describe_computer(agent);
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

    /*
     * "Empty" needs the reason attached, because while you are running
     * it is almost always empty and that means nothing at all.
     *
     * A message is acknowledged the moment it reaches your socket and
     * arrives as an ordinary turn; the mailbox holds what was queued
     * while you were stopped. An agent that checked here to find out
     * whether a peer had answered concluded, correctly and uselessly,
     * that nothing had -- while the answer was in the turn it had just
     * been handed.
     */
    if (items->len == 0)
        return g_strdup(
            "Your mailbox is empty. That is the normal state while you "
            "are running: a message is delivered straight into your "
            "conversation as soon as it arrives, and only queues here "
            "when you are stopped. To see what someone said to you, or "
            "whether they have replied, use clawtilla_room_history with "
            "their agent id.");

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


/*
 * Reaching the person, which is not the same as replying.
 *
 * An agent's reply goes back into the room the message came from, so an
 * agent asked by its operator to consult a peer answered the peer and
 * the operator heard nothing -- they had to ask "did you get a reply?"
 * every time. The room between an agent and the user is a room like any
 * other; this posts into it, and the client is already watching it.
 */
static gchar *
tool_message_user(ClawtMcpTools *self, const gchar *agent_id,
                  JsonObject *arguments, gboolean *is_error)
{
    const gchar *body = argument_string(arguments, "body");
    ClawtRoom *room;
    g_autofree gchar *full = NULL;
    g_autoptr(GError) error = NULL;

    if (body == NULL || body[0] == '\0') {
        *is_error = TRUE;
        return g_strdup("A message needs a body.");
    }

    if (self->room_manager == NULL || self->deliver == NULL) {
        *is_error = TRUE;
        return g_strdup("There is no way to reach your operator from here.");
    }

    /*
     * Files, if any were named.
     *
     * The bytes are taken now rather than the path being passed through:
     * a path only resolves when the client and the file are on the same
     * machine, and the failure when they are not looks like a broken
     * image rather than an unsupported configuration.  A file that
     * cannot be read is reported rather than dropped -- an attachment
     * that silently did not arrive is worse than one that was refused.
     */
    {
        g_autoptr(GString) block = NULL;
        JsonArray *files = NULL;
        guint i;

        if (arguments != NULL && json_object_has_member(arguments,
                                                        "attachments")) {
            JsonNode *node = json_object_get_member(arguments, "attachments");

            if (JSON_NODE_HOLDS_ARRAY(node))
                files = json_node_get_array(node);
        }

        for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
            const gchar *path = json_array_get_string_element(files, i);
            g_autoptr(GError) store_error = NULL;
            g_autofree gchar *id = NULL;
            g_autofree gchar *name = NULL;

            if (self->attachment_dir == NULL) {
                *is_error = TRUE;
                return g_strdup("This daemon cannot carry files, so nothing "
                                "was sent. Say what you would have "
                                "attached.");
            }

            id = clawt_attachment_store(self->attachment_dir, path,
                                        &store_error);

            if (id == NULL) {
                *is_error = TRUE;
                return g_strdup_printf("Nothing was sent: %s",
                                       store_error != NULL
                                           ? store_error->message
                                           : "that file could not be read");
            }

            if (block == NULL) {
                block = g_string_new("\n\n");
                g_string_append(block, CLAWT_ATTACHMENT_MARKER);
                g_string_append_c(block, '\n');
            }

            name = clawt_attachment_name(id);
            g_string_append_printf(block, "- %s\n  clawt:%s\n", name, id);
        }

        if (block != NULL) {
            full = g_strconcat(body, block->str, NULL);
            body = full;
        }
    }

    room = clawt_room_manager_get_direct(self->room_manager, agent_id,
                                         "user");

    if (!self->deliver(agent_id, clawt_room_get_id(room), body, NULL,
                       outbound_depth(self, agent_id), CLAWT_PRIORITY_NORMAL,
                       self->deliver_data, &error)) {
        *is_error = TRUE;
        return g_strdup_printf("Could not reach your operator: %s",
                               error != NULL ? error->message : "unknown");
    }

    return g_strdup("Told them.");
}

/* ── Memory ──────────────────────────────────────────────────────── */

/*
 * Whose memories the caller may read.
 *
 * Its own always. Somebody else's only when that agent has named it in
 * memory.readers -- which is empty by default, so the answer is almost
 * always "your own or nothing". Reading only: there is no path by which
 * one agent writes into another's memory, because a memory you did not
 * form is not a memory.
 */
static ClawtMemoryStore *
memory_for(ClawtMcpTools *self, const gchar *caller, const gchar *wanted,
           GError **error)
{
    ClawtAgent *owner;
    const gchar *readers;
    g_auto(GStrv) allowed = NULL;
    gsize i;

    if (wanted == NULL || wanted[0] == '\0' ||
        g_strcmp0(wanted, caller) == 0) {
        ClawtAgent *mine = clawt_agent_manager_get(self->agents, caller);

        return (mine != NULL) ? clawt_agent_get_memory(mine) : NULL;
    }

    owner = clawt_agent_manager_get(self->agents, wanted);

    if (owner == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "There is no agent called '%s'.", wanted);
        return NULL;
    }

    readers = clawt_agent_config_get_string(clawt_agent_get_config(owner),
                                            "memories.readers");
    allowed = (readers != NULL) ? g_strsplit(readers, ",", -1) : NULL;

    for (i = 0; allowed != NULL && allowed[i] != NULL; i++) {
        if (g_strcmp0(g_strstrip(allowed[i]), caller) == 0)
            return clawt_agent_get_memory(owner);
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                "'%s' has not shared its memories with you. Agents keep "
                "their own; ask the operator if you need theirs.", wanted);
    return NULL;
}

/* One memory, formatted the same way everywhere it is shown. */
static void
append_memory(GString *out, ClawtMemory *memory, gboolean full)
{
    g_string_append_printf(out, "%s [%s/%s]%s", memory->id,
                           memory->category, memory->importance,
                           memory->pinned ? " (pinned)" : "");

    if (memory->tags != NULL && memory->tags[0] != '\0')
        g_string_append_printf(out, " tags: %s", memory->tags);

    g_string_append_c(out, '\n');

    if (memory->summary != NULL && memory->summary[0] != '\0')
        g_string_append_printf(out, "  %s\n", memory->summary);

    if (full || memory->summary == NULL || memory->summary[0] == '\0')
        g_string_append_printf(out, "  %s\n", memory->content);
}

static gchar *
memories_to_text(GPtrArray *memories, const gchar *nothing)
{
    g_autoptr(GString) out = NULL;
    guint i;

    if (memories == NULL || memories->len == 0)
        return g_strdup(nothing);

    out = g_string_new(NULL);

    for (i = 0; i < memories->len; i++)
        append_memory(out, g_ptr_array_index(memories, i), FALSE);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * The configured limit is a ceiling, not a default to be argued with:
 * every result lands in the agent's own context.
 */
static guint
memory_limit(ClawtMcpTools *self, const gchar *agent_id,
             JsonObject *arguments)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);
    gint configured = 20;
    gint asked;

    if (agent != NULL)
        configured = (gint)clawt_agent_config_get_int(
            clawt_agent_get_config(agent), "memories.max_results");

    if (configured <= 0)
        configured = 20;

    asked = (gint)argument_int(arguments, "limit", configured);

    return (guint)CLAMP(asked, 1, configured);
}

static ClawtMemoryStore *
own_memory(ClawtMcpTools *self, const gchar *agent_id)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);

    return (agent != NULL) ? clawt_agent_get_memory(agent) : NULL;
}

static gchar *
tool_memory_add(ClawtMcpTools *self, const gchar *agent_id,
                JsonObject *arguments, gboolean *is_error)
{
    ClawtMemoryStore *store = own_memory(self, agent_id);
    g_autoptr(ClawtMemory) memory = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    const gchar *category = argument_string(arguments, "category");
    const gchar *importance = argument_string(arguments, "importance");

    if (store == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no memory store.");
    }

    memory = clawt_memory_new(argument_string(arguments, "content"));
    memory->summary = g_strdup(argument_string(arguments, "summary"));
    memory->tags = g_strdup(argument_string(arguments, "tags"));

    if (category != NULL && category[0] != '\0') {
        g_free(memory->category);
        memory->category = g_strdup(category);
    }

    if (importance != NULL && importance[0] != '\0') {
        g_free(memory->importance);
        memory->importance = g_strdup(importance);
    }

    /*
     * Recorded rather than asked for: an agent should not have to
     * remember to say that it was itself.
     */
    memory->source = g_strdup(agent_id);

    id = clawt_memory_store_add(store, memory, &error);

    if (id == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("Could not remember that: %s",
                               error->message);
    }

    return g_strdup_printf("Remembered as %s.", id);
}

static gchar *
tool_memory_search(ClawtMcpTools *self, const gchar *agent_id,
                   JsonObject *arguments, gboolean *is_error)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) found = NULL;
    const gchar *whose = argument_string(arguments, "agent");
    ClawtMemoryStore *store = memory_for(self, agent_id, whose, &error);

    if (store == NULL) {
        *is_error = TRUE;
        return g_strdup(error != NULL ? error->message
                                      : "You have no memory store.");
    }

    found = clawt_memory_store_search(store,
                                      argument_string(arguments, "query"),
                                      argument_string(arguments, "category"),
                                      memory_limit(self, agent_id, arguments),
                                      &error);

    return memories_to_text(found, "Nothing remembered matches that.");
}

static gchar *
tool_memory_list(ClawtMcpTools *self, const gchar *agent_id,
                 JsonObject *arguments, gboolean *is_error)
{
    ClawtMemoryStore *store = own_memory(self, agent_id);
    g_autoptr(GPtrArray) memories = NULL;

    if (store == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no memory store.");
    }

    memories = clawt_memory_store_list(
        store, argument_string(arguments, "category"),
        argument_boolean(arguments, "pinned_only", FALSE),
        memory_limit(self, agent_id, arguments), NULL);

    return memories_to_text(memories,
                            "You have not remembered anything yet.");
}

static gchar *
tool_memory_get(ClawtMcpTools *self, const gchar *agent_id,
                JsonObject *arguments, gboolean *is_error)
{
    ClawtMemoryStore *store = own_memory(self, agent_id);
    g_autoptr(ClawtMemory) memory = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GString) out = NULL;

    if (store == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no memory store.");
    }

    memory = clawt_memory_store_get(store, argument_string(arguments, "id"),
                                    &error);

    if (memory == NULL) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    out = g_string_new(NULL);
    append_memory(out, memory, TRUE);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static gchar *
tool_memory_forget(ClawtMcpTools *self, const gchar *agent_id,
                   JsonObject *arguments, gboolean *is_error)
{
    ClawtMemoryStore *store = own_memory(self, agent_id);
    const gchar *id = argument_string(arguments, "id");
    g_autoptr(GError) error = NULL;

    if (store == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no memory store.");
    }

    if (!clawt_memory_store_forget(store, id, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    return g_strdup_printf("Forgotten: %s is out of every listing and "
                           "search, and still on disk.", id);
}

static gchar *
tool_memory_pin(ClawtMcpTools *self, const gchar *agent_id,
                JsonObject *arguments, gboolean *is_error)
{
    ClawtMemoryStore *store = own_memory(self, agent_id);
    const gchar *id = argument_string(arguments, "id");
    gboolean pinned = argument_boolean(arguments, "pinned", TRUE);
    g_autoptr(GError) error = NULL;

    if (store == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no memory store.");
    }

    if (!clawt_memory_store_pin(store, id, pinned, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    return g_strdup_printf("%s is now %s.", id,
                           pinned ? "pinned" : "unpinned");
}

/* ── Rooms ───────────────────────────────────────────────────────── */

/*
 * These six were listed in the tool table -- so they appeared in
 * tools/list, passed the permission checks and were offered to every
 * agent -- and had no branch in the dispatch below, so calling any of
 * them answered "there is no tool called that".  An agent cannot work
 * around a tool that lies about existing.
 */
static ClawtRoom *
room_for(ClawtMcpTools *self, const gchar *room_id, const gchar *caller)
{
    ClawtRoom *room;

    if (self->room_manager == NULL || room_id == NULL)
        return NULL;

    room = clawt_room_manager_get(self->room_manager, room_id);

    if (room != NULL)
        return room;

    /*
     * Not a room, so try it as an agent: the conversation between two
     * agents lives in the direct room between them, and how that room
     * is named is the daemon's business.
     *
     * Without this an agent asked to "message test and see what they
     * say" had no way to look. Its mailbox is empty -- delivery drains
     * it -- and reading the exchange meant knowing to type
     * "dm:<sorted>:<pair>", which is exactly the internal naming a
     * caller is told not to depend on. It reported that nothing had
     * come back while the reply sat in the transcript.
     */
    if (caller == NULL ||
        clawt_agent_manager_get(self->agents, room_id) == NULL)
        return NULL;

    return clawt_room_manager_get_direct(self->room_manager, caller,
                                         room_id);
}

static gchar *
tool_post_room(ClawtMcpTools *self, const gchar *agent_id,
               JsonObject *arguments, gboolean *is_error)
{
    const gchar *room_id = argument_string(arguments, "room_id");
    const gchar *body = argument_string(arguments, "body");
    g_autoptr(GError) error = NULL;

    if (room_id == NULL || body == NULL) {
        *is_error = TRUE;
        return g_strdup("room_id and body are both required.");
    }

    if (room_for(self, room_id, agent_id) == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no room called '%s'.", room_id);
    }

    if (self->deliver == NULL) {
        *is_error = TRUE;
        return g_strdup("Posting is not available.");
    }

    if (!self->deliver(agent_id, room_id, body, NULL,
                       outbound_depth(self, agent_id), CLAWT_PRIORITY_NORMAL,
                       self->deliver_data, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    return g_strdup_printf("Posted to %s.", room_id);
}

static gchar *
tool_create_room(ClawtMcpTools *self, JsonObject *arguments,
                 gboolean *is_error)
{
    const gchar *room_id = argument_string(arguments, "room_id");
    const gchar *members = argument_string(arguments, "members");
    g_autoptr(GError) error = NULL;
    ClawtRoom *room;

    if (room_id == NULL || members == NULL) {
        *is_error = TRUE;
        return g_strdup("room_id and members are both required.");
    }

    if (self->room_manager == NULL) {
        *is_error = TRUE;
        return g_strdup("Rooms cannot be created from here.");
    }

    room = clawt_room_manager_create(self->room_manager, room_id, NULL,
                                     &error);

    if (room == NULL) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    {
        g_auto(GStrv) parts = g_strsplit(members, ",", -1);
        gsize i;

        for (i = 0; parts[i] != NULL; i++) {
            const gchar *member = g_strstrip(parts[i]);

            if (*member != '\0')
                clawt_room_add_member(room, member);
        }
    }

    return g_strdup_printf("Created %s.", room_id);
}

static gchar *
tool_room_history(ClawtMcpTools *self, const gchar *agent_id,
                  JsonObject *arguments, gboolean *is_error)
{
    const gchar *room_id = argument_string(arguments, "room_id");
    ClawtRoom *room = room_for(self, room_id, agent_id);
    g_autoptr(GPtrArray) history = NULL;
    g_autoptr(GString) out = NULL;
    guint i;

    if (room == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no room or agent called '%s'.",
                               room_id != NULL ? room_id : "(none)");
    }

    history = clawt_room_get_history(
        room, (guint)argument_int(arguments, "limit", 20));

    if (history->len == 0)
        return g_strdup("Nothing has been said in that room yet.");

    out = g_string_new(NULL);

    for (i = 0; i < history->len; i++) {
        ClawtMessage *message = g_ptr_array_index(history, i);

        g_string_append_printf(out, "%s: %s\n",
                               clawt_message_get_sender_id(message),
                               clawt_message_get_body(message));
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Mailbox ─────────────────────────────────────────────────────── */

static ClawtMailbox *
mailbox_of(ClawtMcpTools *self, const gchar *agent_id)
{
    ClawtAgent *agent = (self->agents != NULL)
                        ? clawt_agent_manager_get(self->agents, agent_id)
                        : NULL;

    return (agent != NULL) ? clawt_agent_get_mailbox(agent) : NULL;
}

static gchar *
tool_mailbox_read(ClawtMcpTools *self, const gchar *agent_id,
                  JsonObject *arguments, gboolean *is_error)
{
    ClawtMailbox *mailbox = mailbox_of(self, agent_id);
    const gchar *message_id = argument_string(arguments, "message_id");
    g_autoptr(ClawtMailboxItem) item = NULL;

    if (mailbox == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no mailbox.");
    }

    if (message_id == NULL) {
        *is_error = TRUE;
        return g_strdup("message_id is required.");
    }

    item = clawt_mailbox_get(mailbox, message_id);

    if (item == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no message %s in your mailbox.",
                               message_id);
    }

    return g_strdup_printf("From %s%s%s:\n\n%s",
                           clawt_mailbox_item_get_from(item),
                           clawt_mailbox_item_get_room(item) != NULL
                               ? " in " : "",
                           clawt_mailbox_item_get_room(item) != NULL
                               ? clawt_mailbox_item_get_room(item) : "",
                           clawt_mailbox_item_get_body(item));
}

static gchar *
tool_mailbox_ack(ClawtMcpTools *self, const gchar *agent_id,
                 JsonObject *arguments, gboolean *is_error)
{
    ClawtMailbox *mailbox = mailbox_of(self, agent_id);
    const gchar *message_id = argument_string(arguments, "message_id");
    g_autoptr(GError) error = NULL;

    if (mailbox == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no mailbox.");
    }

    if (message_id == NULL) {
        *is_error = TRUE;
        return g_strdup("message_id is required.");
    }

    if (!clawt_mailbox_ack(mailbox, message_id, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    return g_strdup_printf("%s is dealt with.", message_id);
}

static gchar *
tool_mailbox_reply(ClawtMcpTools *self, const gchar *agent_id,
                   JsonObject *arguments, gboolean *is_error)
{
    ClawtMailbox *mailbox = mailbox_of(self, agent_id);
    const gchar *message_id = argument_string(arguments, "message_id");
    const gchar *body = argument_string(arguments, "body");
    g_autoptr(ClawtMailboxItem) item = NULL;
    g_autoptr(GError) error = NULL;

    if (mailbox == NULL) {
        *is_error = TRUE;
        return g_strdup("You have no mailbox.");
    }

    if (message_id == NULL || body == NULL) {
        *is_error = TRUE;
        return g_strdup("message_id and body are both required.");
    }

    item = clawt_mailbox_get(mailbox, message_id);

    if (item == NULL) {
        *is_error = TRUE;
        return g_strdup_printf("There is no message %s in your mailbox.",
                               message_id);
    }

    if (self->deliver == NULL) {
        *is_error = TRUE;
        return g_strdup("Replying is not available.");
    }

    if (!self->deliver(agent_id, clawt_mailbox_item_get_from(item), body,
                       clawt_mailbox_item_get_task_id(item),
                       outbound_depth(self, agent_id), CLAWT_PRIORITY_NORMAL,
                       self->deliver_data, &error)) {
        *is_error = TRUE;
        return g_strdup(error->message);
    }

    /*
     * Acknowledged only after the reply is away.  Acknowledging first
     * would lose the message if the send failed.
     */
    clawt_mailbox_ack(mailbox, message_id, NULL);

    return g_strdup_printf("Replied to %s.",
                           clawt_mailbox_item_get_from(item));
}

/* ── Dispatch ────────────────────────────────────────────────────── */

/*
 * Is this the one call that must not be answered from the main context?
 *
 * Stated as a question about the request rather than as a list the
 * daemon keeps, so the two cannot drift: a tool that starts blocking
 * declares it here, beside the tool.
 */
gboolean
clawt_mcp_tools_call_defers(ClawtMcpTools *self,
                            const gchar   *agent_id,
                            JsonNode      *request)
{
    JsonObject *root;
    JsonObject *params;
    const gchar *tool_name;

    g_return_val_if_fail(CLAWT_IS_MCP_TOOLS(self), FALSE);

    if (request == NULL || !JSON_NODE_HOLDS_OBJECT(request))
        return FALSE;

    root = json_node_get_object(request);

    if (!json_object_has_member(root, "method") ||
        g_strcmp0(json_object_get_string_member(root, "method"),
                  "tools/call") != 0)
        return FALSE;

    if (!json_object_has_member(root, "params"))
        return FALSE;

    params = json_object_get_object_member(root, "params");
    tool_name = argument_string(params, "name");

    if (g_strcmp0(tool_name, "clawtilla_computer_exec") != 0)
        return FALSE;

    /*
     * A refusal is not worth a thread.  Saying FALSE here sends it back
     * through the synchronous path, which answers at once and in exactly
     * the words the permitted check already uses.
     */
    return clawt_mcp_tools_is_permitted(self, agent_id, tool_name);
}

typedef struct {
    ExecCall *call;
    JsonNode *request_id;
} ExecAsync;

static void
exec_async_free(gpointer data)
{
    ExecAsync *async = data;

    exec_call_free(async->call);
    g_clear_pointer(&async->request_id, json_node_unref);
    g_free(async);
}

static void
exec_call_worker(GTask *task, gpointer source, gpointer data,
                 GCancellable *cancellable)
{
    ExecAsync *async = data;

    (void)source;
    (void)cancellable;

    exec_call_run(async->call);
    g_task_return_boolean(task, TRUE);
}

static void
on_exec_call_finished(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    ExecAsync *async = g_task_get_task_data(G_TASK(result));
    GTask *outer = user_data;
    gboolean is_error = FALSE;
    g_autofree gchar *text = NULL;

    (void)source;

    text = exec_call_reply(async->call, &is_error);

    g_task_return_pointer(outer, make_response(async->request_id, text,
                                               is_error),
                          (GDestroyNotify)json_node_unref);
    g_object_unref(outer);
}

/**
 * clawt_mcp_tools_call_async:
 *
 * Handles one tool call without holding the calling context while the
 * command runs.  Only valid for a request clawt_mcp_tools_call_defers()
 * accepted.
 */
void
clawt_mcp_tools_call_async(ClawtMcpTools       *self,
                           const gchar         *agent_id,
                           JsonNode            *request,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    JsonObject *root;
    JsonObject *params;
    JsonObject *arguments = NULL;
    JsonNode *request_id = NULL;
    g_autofree gchar *refusal = NULL;
    GTask *outer;
    GTask *inner;
    ExecAsync *async;
    ExecCall *call;

    g_return_if_fail(CLAWT_IS_MCP_TOOLS(self));
    g_return_if_fail(request != NULL && JSON_NODE_HOLDS_OBJECT(request));

    outer = g_task_new(self, NULL, callback, user_data);

    root = json_node_get_object(request);

    if (json_object_has_member(root, "id"))
        request_id = json_object_get_member(root, "id");

    params = json_object_get_object_member(root, "params");

    if (json_object_has_member(params, "arguments"))
        arguments = json_object_get_object_member(params, "arguments");

    call = exec_call_prepare(self, agent_id, arguments, &refusal);

    /*
     * Refused without waiting, and still answered through the callback:
     * a caller that has already deferred its IPC frame has no other way
     * to reply, and a synchronous return here would leave that frame
     * unanswered for ever.
     */
    if (call == NULL) {
        g_task_return_pointer(outer,
                              make_response(request_id, refusal, TRUE),
                              (GDestroyNotify)json_node_unref);
        g_object_unref(outer);
        return;
    }

    async = g_new0(ExecAsync, 1);
    async->call = call;
    async->request_id = (request_id != NULL) ? json_node_ref(request_id)
                                             : NULL;

    /*
     * The inner task carries the wait; the outer one carries the answer.
     * Both are created on the context this was dispatched from -- the
     * daemon's own -- rather than on the process default, because
     * dispatching a source does not make its context thread-default and
     * a task that completes on a loop nobody runs never completes at all.
     */
    inner = g_task_new(self, NULL, on_exec_call_finished, outer);
    g_task_set_task_data(inner, async, exec_async_free);
    g_task_run_in_thread(inner, exec_call_worker);
    g_object_unref(inner);
}

/**
 * clawt_mcp_tools_call_finish:
 *
 * Returns: (transfer full): the JSON-RPC response
 */
JsonNode *
clawt_mcp_tools_call_finish(ClawtMcpTools *self, GAsyncResult *result)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), NULL);
}

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
        g_autofree gchar *message = g_strdup_printf(
            "You are not permitted to use %s.", tool_name);

        return make_response(request_id, message, TRUE);
    }

    if (g_strcmp0(tool_name, "clawtilla_list_agents") == 0)
        text = tool_list_agents(self, agent_id);
    else if (g_strcmp0(tool_name, "clawtilla_agent_options") == 0)
        text = tool_agent_options(self);
    else if (g_strcmp0(tool_name, "clawtilla_create_agent") == 0)
        text = tool_create_agent(self, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_get_agent") == 0)
        text = tool_get_agent(self, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_message_agent") == 0 ||
             g_strcmp0(tool_name, "clawtilla_ask_agent") == 0)
        text = tool_message_agent(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_ask_decision") == 0)
        text = tool_ask_decision(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_fleet_cost") == 0)
        text = tool_fleet_cost(self, agent_id);
    else if (g_strcmp0(tool_name, "clawtilla_list_teams") == 0)
        text = tool_list_teams(self, agent_id);
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
    } else if (g_strcmp0(tool_name, "clawtilla_message_user") == 0)
        text = tool_message_user(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_memory_add") == 0)
        text = tool_memory_add(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_memory_search") == 0)
        text = tool_memory_search(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_memory_list") == 0)
        text = tool_memory_list(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_memory_get") == 0)
        text = tool_memory_get(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_memory_forget") == 0)
        text = tool_memory_forget(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_memory_pin") == 0)
        text = tool_memory_pin(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_computer_exec") == 0)
        text = tool_computer_exec(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_computer_state") == 0)
        text = tool_computer_state(self, agent_id);
    else if (g_strcmp0(tool_name, "clawtilla_mailbox_list") == 0)
        text = tool_mailbox_list(self, agent_id, arguments);
    else if (g_strcmp0(tool_name, "clawtilla_mailbox_read") == 0)
        text = tool_mailbox_read(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_mailbox_ack") == 0)
        text = tool_mailbox_ack(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_mailbox_reply") == 0)
        text = tool_mailbox_reply(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_post_room") == 0)
        text = tool_post_room(self, agent_id, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_create_room") == 0)
        text = tool_create_room(self, arguments, &is_error);
    else if (g_strcmp0(tool_name, "clawtilla_room_history") == 0)
        text = tool_room_history(self, agent_id, arguments,
                                 &is_error);
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

    g_clear_pointer(&self->attachment_dir, g_free);

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
