/*
 * daemon-agent-socket.c - Each agent's own IPC socket, and what it serves
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything an agent's CLI starts to reach the daemon --
 * clawtilla-mcp-server, the Claude Code policy hook, the connector and
 * desktop relays -- used to dial the operator's daemon.socket, and a
 * confined agent's bwrap box bound that socket read-write so they could.
 * A unix client there is the operator: the kernel's word is the only
 * authentication, and SO_PEERCRED cannot tell an agent's process from
 * the person's, because the uid is the same.  So anything in the box
 * that could exec could answer its own policy ask, rewrite its own rules
 * and confinement, add a trigger or unblock anybody's task, all recorded
 * as the operator (VENTURE #72).
 *
 * Now each agent gets a socket of its own, which the daemon makes for
 * that agent alone, and a box binds only its own.  The identity of every
 * request on it is the socket it arrived on, never an agent named in the
 * payload, and it serves the agent verbs in the one table below and
 * refuses every other verb by name.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>

/*
 * How a verb's payload says which agent it is about, and so what the
 * filter has to make true of it before the handler sees it.
 */
typedef enum {
    /* Names no agent (the handshake). */
    AGENT_VERB_BARE,

    /*
     * `agent` is the caller: filled in from the connection when absent,
     * and refused when it names anybody else.
     */
    AGENT_VERB_SELF,

    /*
     * As AGENT_VERB_SELF, and `name` must be a connector the calling
     * agent holds: the relay asks about its own connectors and nobody
     * else's.
     */
    AGENT_VERB_CONNECTOR
} AgentVerbScope;

/*
 * Whether a verb changes anything.  Said in the table itself, so the next
 * reader sees why each row is there: an act is one the agent's own
 * clients need to do their job, a read only reports fleet state.
 */
typedef enum {
    AGENT_VERB_READ,
    AGENT_VERB_ACT
} AgentVerbEffect;

typedef struct {
    const gchar     *kind;
    AgentVerbEffect  effect;
    AgentVerbScope   scope;
    const gchar     *why;     /* who sends it, or why it is safe to read */
} AgentVerb;

/*
 * The agent verbs -- the ONE table.  The filter, the refusal's list and
 * the tests all read it, so changing what an agent may send is a
 * one-line edit here, and a verb added to the daemon later is refused on
 * an agent socket until somebody adds a row for it.  No patterns: every
 * verb is spelled out.
 *
 * The rule (Zach's decision ba0011f1, 2026-11-28): an agent may read
 * fleet state, may not change it, and may not read secrets.
 *
 * - The act rows are exactly what an agent's own CLI and the processes it
 *   starts send today (VENTURE #72's trace): clawtilla-mcp-server, the
 *   policy hook, and the connector and desktop relays.  Each acts for the
 *   connection's agent only (AGENT_VERB_SELF / AGENT_VERB_CONNECTOR).
 * - The read rows report state and change nothing, and answer with what
 *   the operator would get.  None of them names an agent whose private
 *   data it would reveal: an `agent` in task.list or wait.list is a
 *   filter over fleet state, not an identity.
 *
 * Refused although they read, because a reply can carry a secret or
 * another agent's private data (docs/security.org, "The agent socket"):
 * config.show, agent.show (every setting, env: values included),
 * connector.list, integration.list, account.list, vault.list,
 * trigger.list (a webhook's address is a bearer capability),
 * room.history, room.list (each room's last message body), event.list
 * and notice.list (every conversation, not only the agent's own),
 * mailbox.list, memory.list, memory.search, operator.get.
 * Refused although they look like reads, because they write: model.list
 * (refresh warms the cache), steps.list, orchestration.get, notice.read,
 * room.read.
 */
static const AgentVerb agent_verbs[] = {
    /* Every ClawtClient says this first; a unix one with no token. */
    { "control.hello",            AGENT_VERB_READ, AGENT_VERB_BARE,
      "every client's handshake" },

    /* ── act: what the agent's own clients send ─────────────────── */
    { "tool.rpc",                 AGENT_VERB_ACT,  AGENT_VERB_SELF,
      "clawtilla-mcp-server: the orchestration tools, token and all" },
    { "policy.check",             AGENT_VERB_ACT,  AGENT_VERB_SELF,
      "policy-hook and the relays; an ask posts a decision" },
    { "connector.refresh",        AGENT_VERB_ACT,  AGENT_VERB_CONNECTOR,
      "connector relay: renew its own connector's credential" },
    { "connector.unauthorized",   AGENT_VERB_ACT,  AGENT_VERB_CONNECTOR,
      "connector relay: a 401 from its own connector" },
    { "connector.tools_seen",     AGENT_VERB_ACT,  AGENT_VERB_CONNECTOR,
      "connector relay: its own connector's tool count" },
    { "connector.result_trimmed", AGENT_VERB_ACT,  AGENT_VERB_SELF,
      "connector relay: a notice in the agent's own turn" },
    { "connector.app",            AGENT_VERB_ACT,  AGENT_VERB_SELF,
      "connector relay: an app into the agent's own conversation" },
    { "computer.desktop",         AGENT_VERB_ACT,  AGENT_VERB_SELF,
      "desktop relay: the agent's own screen" },
    { "computer.control",         AGENT_VERB_ACT,  AGENT_VERB_SELF,
      "desktop relay: may the agent act on its own screen now" },

    /* ── read: fleet state, no secrets ──────────────────────────── */
    { "control.status",           AGENT_VERB_READ, AGENT_VERB_BARE,
      "the daemon's version and counts" },
    { "agent.list",               AGENT_VERB_READ, AGENT_VERB_BARE,
      "the roster: ids, states, teams, computers; no settings" },
    { "team.list",                AGENT_VERB_READ, AGENT_VERB_BARE,
      "teams, leads and members" },
    { "task.list",                AGENT_VERB_READ, AGENT_VERB_BARE,
      "tasks and their states" },
    { "task.show",                AGENT_VERB_READ, AGENT_VERB_BARE,
      "one task, its result and notes" },
    { "incident.list",            AGENT_VERB_READ, AGENT_VERB_BARE,
      "the incidents raised on tasks and routines" },
    { "decision.list",            AGENT_VERB_READ, AGENT_VERB_BARE,
      "the decisions waiting on the operator; answering is refused" },
    { "wait.list",                AGENT_VERB_READ, AGENT_VERB_BARE,
      "pending waits" },
    { "routine.list",             AGENT_VERB_READ, AGENT_VERB_BARE,
      "routines and their schedules" },
    { "routine.upcoming",         AGENT_VERB_READ, AGENT_VERB_BARE,
      "the next runs" },
    { "routine.occurrences",      AGENT_VERB_READ, AGENT_VERB_BARE,
      "runs in a window" },
    { "project.list",             AGENT_VERB_READ, AGENT_VERB_BARE,
      "projects and their budgets" },
    { "usage.summary",            AGENT_VERB_READ, AGENT_VERB_BARE,
      "spend and token counts" },
    { "skill.list",               AGENT_VERB_READ, AGENT_VERB_BARE,
      "the skill library's names and descriptions" }
};

const gchar *
clawt_daemon_agent_verb_nth(guint index)
{
    if (index >= G_N_ELEMENTS(agent_verbs))
        return NULL;

    return agent_verbs[index].kind;
}

static const AgentVerb *
find_agent_verb(const gchar *kind)
{
    guint i;

    for (i = 0; kind != NULL && i < G_N_ELEMENTS(agent_verbs); i++) {
        if (g_strcmp0(agent_verbs[i].kind, kind) == 0)
            return &agent_verbs[i];
    }

    return NULL;
}

/* The verbs, comma-separated, for a refusal that says what *is* served. */
static gchar *
agent_verb_list(void)
{
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; i < G_N_ELEMENTS(agent_verbs); i++) {
        if (i > 0)
            g_string_append(out, ", ");
        g_string_append(out, agent_verbs[i].kind);
    }

    return g_string_free(out, FALSE);
}

/*
 * Whether @agent_id holds the connector called @name.  The same answer
 * the connector handlers give for an agent's own calls, asked of the
 * config rather than of anything the request says.
 */
static gboolean
agent_holds_connector(ClawtDaemon *self, const gchar *agent_id,
                      const gchar *name)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(GPtrArray) bindings = NULL;
    guint i;

    if (agent == NULL || name == NULL)
        return FALSE;

    bindings = clawt_integration_resolve_for_agent(self->config, agent);

    for (i = 0; bindings != NULL && i < bindings->len; i++) {
        ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

        if (g_strcmp0(clawt_integration_binding_get_name(binding), name) == 0 &&
            g_strcmp0(clawt_integration_binding_get_info(binding)->id,
                      "connector") == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * The agent filter (ClawtIpcAgentFilter): one request on @agent_id's own
 * socket, served or refused.
 *
 * Refused unless its verb is in agent_verbs[].  Served ones have their
 * payload's `agent` made the connection's -- set when it is absent,
 * refused when it names another agent -- so a handler that reads the
 * agent from the payload, as every one of these does, reads the
 * connection's.  The handlers' own checks (tool.rpc's token, the
 * per-agent gates inside the tools) still run after this.
 */
static gboolean
agent_socket_filter(ClawtIpcServer  *server,
                    const gchar     *agent_id,
                    JsonNode        *request,
                    GError         **error,
                    gpointer         user_data)
{
    ClawtDaemon *self = user_data;
    const gchar *kind = clawt_ipc_frame_get_kind(request);
    const AgentVerb *verb = find_agent_verb(kind);
    JsonObject *payload;

    (void)server;

    if (verb == NULL) {
        g_autofree gchar *served = agent_verb_list();

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "'%s' is not served on this socket: it is agent '%s''s "
                    "own, and serves only the agent verbs (%s). Operator "
                    "verbs go to the operator's daemon socket, which an "
                    "agent is not given",
                    kind != NULL ? kind : "(no kind)", agent_id, served);
        return FALSE;
    }

    /*
     * The frame's own `acting_agent` (the CLI sets it from
     * CLAWTILLA_AGENT_ID) names who a save is credited to.  On this
     * socket that is the connection's agent and nobody else, for every
     * verb -- a read included -- so one naming another agent is refused
     * the way a payload's `agent` is.  The daemon credits the connection's
     * agent whatever the frame says (clawt_daemon_handle_request()).
     */
    if (JSON_NODE_HOLDS_OBJECT(request) &&
        json_object_has_member(json_node_get_object(request),
                               "acting_agent")) {
        JsonNode *named = json_object_get_member(
            json_node_get_object(request), "acting_agent");

        if (!JSON_NODE_HOLDS_VALUE(named) ||
            json_node_get_value_type(named) != G_TYPE_STRING ||
            g_strcmp0(json_node_get_string(named), agent_id) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                        "this socket belongs to agent '%s', and a request "
                        "on it is credited to '%s' only: acting_agent "
                        "cannot name another agent", agent_id, agent_id);
            return FALSE;
        }
    }

    if (verb->scope == AGENT_VERB_BARE)
        return TRUE;

    payload = clawt_ipc_frame_get_payload(request);

    /* No payload at all: one is made, so the agent can be put in it. */
    if (payload == NULL) {
        JsonNode *fresh = json_node_new(JSON_NODE_OBJECT);

        json_node_take_object(fresh, json_object_new());
        clawt_ipc_frame_set_payload(request, fresh);
        payload = clawt_ipc_frame_get_payload(request);
    }

    if (payload == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "'%s' needs an object payload", kind);
        return FALSE;
    }

    /*
     * The identity is the connection's.  An `agent` naming somebody else
     * is refused rather than quietly overwritten, so a relay that is
     * misconfigured says so instead of acting as the wrong agent -- and
     * anything that is not a plain string is not an agent id at all.
     */
    if (json_object_has_member(payload, "agent")) {
        JsonNode *named = json_object_get_member(payload, "agent");

        if (!JSON_NODE_HOLDS_VALUE(named) ||
            json_node_get_value_type(named) != G_TYPE_STRING ||
            g_strcmp0(json_node_get_string(named), agent_id) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                        "this socket belongs to agent '%s', and a request "
                        "on it acts as '%s' only: it cannot name another "
                        "agent", agent_id, agent_id);
            return FALSE;
        }
    } else {
        json_object_set_string_member(payload, "agent", agent_id);
    }

    if (verb->scope == AGENT_VERB_CONNECTOR &&
        !agent_holds_connector(self, agent_id,
                               clawt_ipc_payload_string(payload, "name"))) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "agent '%s' holds no connector called '%s'",
                    agent_id,
                    clawt_ipc_payload_string(payload, "name") != NULL
                        ? clawt_ipc_payload_string(payload, "name") : "");
        return FALSE;
    }

    return TRUE;
}

void
clawt_daemon_agent_sockets_setup(ClawtDaemon *self)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(GDir) listing = NULL;
    const gchar *name;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->ipc_server == NULL)
        return;

    clawt_ipc_server_set_agent_filter(self->ipc_server, agent_socket_filter,
                                      self, NULL);

    /*
     * Stale sockets, left by a daemon that did not shut down cleanly.
     * Every one of them is stale: this runs once the operator socket is
     * bound, which proves no other daemon is serving this directory, and
     * before any agent has started.  Only sockets are removed, so a file
     * somebody put here by hand is left alone and reported by the bind
     * that trips over it.
     */
    dir = clawt_config_agent_socket_dir(self->config);
    listing = g_dir_open(dir, 0, NULL);

    while (listing != NULL && (name = g_dir_read_name(listing)) != NULL) {
        g_autofree gchar *path = g_build_filename(dir, name, NULL);
        GStatBuf info;

        if (!g_str_has_suffix(name, ".sock") ||
            g_lstat(path, &info) != 0 || !S_ISSOCK(info.st_mode))
            continue;

        if (g_unlink(path) == 0)
            g_info("agent sockets: removed a stale socket at %s", path);
    }
}

gboolean
clawt_daemon_agent_socket_open(ClawtDaemon  *self,
                               const gchar  *agent_id,
                               GError      **error)
{
    g_autofree gchar *path = NULL;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    /* An embedded daemon with no IPC has no clients to give a socket to. */
    if (self->ipc_server == NULL)
        return TRUE;

    path = clawt_config_agent_socket_path(self->config, agent_id);

    if (!clawt_ipc_server_add_agent_socket(self->ipc_server, agent_id, path,
                                           error)) {
        g_prefix_error(error, "agent %s: its own daemon socket: ", agent_id);
        return FALSE;
    }

    return TRUE;
}

void
clawt_daemon_agent_socket_close(ClawtDaemon *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->ipc_server == NULL || agent_id == NULL)
        return;

    clawt_ipc_server_remove_agent_socket(self->ipc_server, agent_id);
}
