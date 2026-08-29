/*
 * clawt-daemon.h - The fleet, assembled
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

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"
#include "agent/clawt-agent-manager.h"
#include "ai/clawt-agent-designer.h"
#include "chat/clawt-loop-guard.h"
#include "chat/clawt-room-manager.h"
#include "computer/clawt-exchange.h"
#include "config/clawt-config.h"
#include "core/clawt-event-bus.h"
#include "core/clawt-event-log.h"
#include "ipc/clawt-ipc-server.h"
#include "link/clawt-link-server.h"
#include "mailbox/clawt-mailbox-router.h"
#include "mcp/clawt-mcp-tools.h"
#include "plugin/clawt-plugin-manager.h"
#include "task/clawt-task-manager.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_DAEMON (clawt_daemon_get_type())

G_DECLARE_FINAL_TYPE(ClawtDaemon, clawt_daemon, CLAWT, DAEMON, GObject)

/**
 * clawt_daemon_new:
 * @config_path: (nullable): the fleet configuration, or %NULL for the default
 * @main_context: (nullable): the context to attach sources to, or %NULL for
 *   the default one
 *
 * Assembles a fleet without starting it.
 *
 * @main_context is what makes embedding work: a host that owns its own
 * loop -- cmacs, say -- passes its context and gets the whole daemon with
 * no process and no socket in between.
 *
 * Returns: (transfer full): a new #ClawtDaemon
 */
ClawtDaemon *clawt_daemon_new(const gchar  *config_path,
                              GMainContext *main_context);

/**
 * clawt_daemon_set_bind_addresses:
 * @self: a #ClawtDaemon
 * @addresses: (nullable) (array zero-terminated=1): addresses to listen
 *   on, each `IP` or `IP:PORT`, or %NULL to listen on none
 * @error: (out) (optional): return location for a #GError
 *
 * Replaces whatever the configuration says about network listeners.
 *
 * Calling this *at all* is the override; @addresses says what to bind.
 * %NULL or an empty list therefore means "no network listener", which is
 * what `clawtillad --no-bind` asks for, and is a different answer from
 * never calling this -- which leaves `daemon.tcp_enabled` and
 * `daemon.tailscale` in charge.
 *
 * An override replaces rather than adds: a daemon told to bind one
 * address that also brought up the tailnet address would be listening
 * somewhere the person neither asked for nor saw. For the same reason
 * these are never optional, so an address that will not bind fails the
 * start rather than being skipped with a warning.
 *
 * Addresses are parsed here rather than at start, so a typo is refused
 * while the person is still looking at the command line instead of after
 * the state directory and every agent workspace have been written.
 *
 * Must be called before clawt_daemon_start().
 *
 * Returns: %TRUE if every address was understood
 */
gboolean clawt_daemon_set_bind_addresses(ClawtDaemon        *self,
                                         const gchar *const *addresses,
                                         GError            **error);

/**
 * clawt_daemon_start:
 * @self: a #ClawtDaemon
 * @error: (out) (optional): return location for a #GError
 *
 * Loads the config, brings up the listeners and starts every agent marked
 * to autostart.
 *
 * Returns: %TRUE if the daemon is running
 */
gboolean clawt_daemon_start(ClawtDaemon *self, GError **error);

/**
 * clawt_daemon_stop:
 * @self: a #ClawtDaemon
 *
 * Stops every agent and closes the listeners.
 */
void clawt_daemon_stop(ClawtDaemon *self);

/**
 * clawt_daemon_run:
 * @self: a #ClawtDaemon
 *
 * Starts the daemon and runs a main loop until it is told to quit.
 *
 * For the standalone `clawtillad` only.  An embedding host runs its own
 * loop and uses clawt_daemon_start() instead.
 *
 * Returns: the process exit status
 */
gint clawt_daemon_run(ClawtDaemon *self);

/**
 * clawt_daemon_quit:
 * @self: a #ClawtDaemon
 *
 * Asks a running clawt_daemon_run() to return.
 */
void clawt_daemon_quit(ClawtDaemon *self);

/**
 * clawt_daemon_reload:
 * @self: a #ClawtDaemon
 * @error: (out) (optional): return location for a #GError
 *
 * Re-reads the configuration and re-renders every agent's file.
 *
 * Running agents are left running: a config reload that restarted the
 * whole fleet would make editing one agent's description interrupt the
 * other nine mid-turn.
 *
 * Returns: %TRUE if the new configuration was accepted
 */
gboolean clawt_daemon_reload(ClawtDaemon *self, GError **error);

/**
 * clawt_daemon_handle_request:
 * @self: a #ClawtDaemon
 * @request: a client frame
 *
 * Answers one client request.
 *
 * Exposed so an embedding host can drive the daemon directly, and so the
 * whole client surface can be tested without a socket.
 *
 * Returns: (transfer full): the reply frame
 */
JsonNode *clawt_daemon_handle_request(ClawtDaemon *self, JsonNode *request);

/*
 * Component accessors, for embedding hosts and clients in-process.
 *
 * All transfer none: the daemon owns these for its whole life, and a
 * caller that unreffed one would take a piece of the running fleet with
 * it.
 */

/**
 * clawt_daemon_get_config:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtConfig *clawt_daemon_get_config(ClawtDaemon *self);

/**
 * clawt_daemon_get_agents:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtAgentManager *clawt_daemon_get_agents(ClawtDaemon *self);

/**
 * clawt_daemon_get_decisions:
 * @self: a #ClawtDaemon
 *
 * The choices agents have asked a person to make.
 *
 * Returns: (transfer none) (nullable): the store, or %NULL if it could
 *   not be opened -- which is a warning at start rather than a refusal,
 *   since losing the inbox is bad and losing the fleet over it is worse
 */
ClawtDecisionStore *clawt_daemon_get_decisions(ClawtDaemon *self);

/**
 * clawt_daemon_get_rooms:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtRoomManager *clawt_daemon_get_rooms(ClawtDaemon *self);

/**
 * clawt_daemon_get_tasks:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtTaskManager *clawt_daemon_get_tasks(ClawtDaemon *self);

/**
 * clawt_daemon_sweep:
 * @self: a #ClawtDaemon
 *
 * Runs the periodic maintenance pass now.
 *
 * Expires mailbox items, returns abandoned leases, trims the event log
 * and applies `defaults.exchange_max_bytes`.  The daemon calls this on a
 * timer; it is public because a pass that only ever runs from a
 * one-minute timeout cannot be tested at all, and every one of the
 * things it drives has been added to it at some point by somebody who
 * could not tell whether the previous ones were being driven.
 */
void clawt_daemon_sweep(ClawtDaemon *self);

/**
 * clawt_daemon_get_router:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtMailboxRouter *clawt_daemon_get_router(ClawtDaemon *self);

/**
 * clawt_daemon_get_event_bus:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtEventBus *clawt_daemon_get_event_bus(ClawtDaemon *self);

/**
 * clawt_daemon_get_event_log:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtEventLog *clawt_daemon_get_event_log(ClawtDaemon *self);

/**
 * clawt_daemon_get_exchange:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtExchange *clawt_daemon_get_exchange(ClawtDaemon *self);

/**
 * clawt_daemon_get_link_server:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtLinkServer *clawt_daemon_get_link_server(ClawtDaemon *self);

/**
 * clawt_daemon_get_ipc_server:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtIpcServer *clawt_daemon_get_ipc_server(ClawtDaemon *self);

/**
 * clawt_daemon_get_loop_guard:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtLoopGuard *clawt_daemon_get_loop_guard(ClawtDaemon *self);

/**
 * clawt_daemon_get_plugins:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtPluginManager *clawt_daemon_get_plugins(ClawtDaemon *self);

/**
 * clawt_daemon_get_mcp_tools:
 * @self: a #ClawtDaemon
 *
 * Returns: (transfer none) (nullable): the component, or %NULL before
 *   clawt_daemon_start()
 */
ClawtMcpTools *clawt_daemon_get_mcp_tools(ClawtDaemon *self);

/**
 * clawt_daemon_start_agent:
 * @self: a #ClawtDaemon
 * @agent_id: which agent
 * @error: (out) (optional): return location for a #GError
 *
 * Renders the agent's configuration, provisions its computer and starts
 * its runtime.
 *
 * Returns: %TRUE if it was started
 */
gboolean clawt_daemon_start_agent(ClawtDaemon  *self,
                                  const gchar  *agent_id,
                                  GError      **error);

/**
 * clawt_daemon_stop_agent:
 * @self: a #ClawtDaemon
 * @agent_id: which agent
 * @stop_machine: also take its container or VM down
 *
 * Stops the agent's process, and optionally the machine it was using.
 *
 * The two are separate because the callers mean different things. `agent
 * stop` and `agent restart` mean "this agent is finished for now", and
 * leaving a container running for an agent nobody is running is a
 * resource nobody can see -- `podman ps` full of machines whose agents
 * are stopped. `agent reset` and `agent remove` stop the process as a
 * *step* in something larger: reset needs the sqlite connection closed
 * before it clears a session, and remove has its own teardown.
 *
 * @stop_machine is **synchronous**, and blocking deliberately, in the
 * same way clawt_daemon_start_agent() is: every caller that passes %TRUE
 * has somebody waiting on the answer, and the wait is bounded by
 * podomation's socket timeout. The two IPC handlers a person drives pass
 * %FALSE and take the machine down through
 * clawt_computer_lifecycle_async() instead, because a handler may not
 * wait on the network.
 *
 * Whether the machine survives being stopped is the backend's business:
 * a VM always does, and a container does unless
 * `computer.container.keep` is turned off.
 *
 * Returns: %TRUE if it was running
 */
gboolean clawt_daemon_stop_agent(ClawtDaemon *self,
                                 const gchar *agent_id,
                                 gboolean     stop_machine);

/**
 * clawt_daemon_interrupt_agent:
 * @self: a #ClawtDaemon
 * @agent_id: which agent
 * @out_killed: (out) (optional): how many processes were signalled
 * @error: (out) (optional): return location for a #GError
 *
 * Stops what the agent is doing right now without stopping the agent.
 *
 * The AI CLI carrying out the turn is killed, along with everything it
 * had spawned; the agent's own process keeps its link, its session and
 * its mailbox, so it is idle rather than stopped and the next message
 * reaches it with no start in between.
 *
 * The agent is marked idle here rather than left to libreclaw's typing
 * indicator. Killing a turn mid-flight is exactly the case where the
 * indicator may never be lowered, and an agent that shows as working for
 * ever after somebody pressed stop is the failure the button exists to
 * prevent.
 *
 * Returns: %TRUE if the turn was interrupted; @out_killed may still be 0
 *   when the agent was between turns, which is not a failure
 */
gboolean clawt_daemon_interrupt_agent(ClawtDaemon  *self,
                                      const gchar  *agent_id,
                                      guint        *out_killed,
                                      GError      **error);

/**
 * clawt_daemon_set_libreclaw_binary:
 * @self: a #ClawtDaemon
 * @path: (nullable): the libreclaw binary, or %NULL to search PATH
 *
 * Overrides which libreclaw process agents run.  For tests, and for
 * running against a build tree rather than an installed copy.
 */
void clawt_daemon_set_libreclaw_binary(ClawtDaemon *self,
                                       const gchar *path);

G_END_DECLS
