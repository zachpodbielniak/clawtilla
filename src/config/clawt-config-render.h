/*
 * clawt-config-render.h - Rendering an agent's libreclaw configuration
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

#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * clawt_config_agent_state_dir:
 * @config: the fleet configuration
 * @agent_id: the agent
 *
 * Where everything belonging to one agent lives: its rendered config, its
 * mailbox, its sessions, its credentials.
 *
 * Returns: (transfer full): the expanded directory path
 */
gchar *clawt_config_agent_state_dir(ClawtConfig *config,
                                    const gchar *agent_id);

/**
 * clawt_config_agent_socket_dir:
 * @config: the fleet configuration
 *
 * The directory that holds every agent's own socket: `agent-sockets`
 * beside `daemon.socket`.  The daemon makes it 0700 and owns everything
 * in it; no box is granted the directory, only an agent's own socket.
 *
 * Returns: (transfer full): the expanded directory path
 */
gchar *clawt_config_agent_socket_dir(ClawtConfig *config);

/**
 * clawt_config_agent_socket_path:
 * @config: the fleet configuration
 * @agent_id: the agent
 *
 * Where the daemon listens for one agent's own IPC connections:
 * `agent-sockets/<agent>.sock` beside `daemon.socket` (its default
 * included).
 *
 * What clawtilla-mcp-server, the policy hook and the connector and
 * desktop relays are pointed at, and the one daemon socket an agent's
 * bwrap box binds.  A connection there is that agent and serves only the
 * agent verbs (docs/security.org, "The agent socket"); the operator's
 * `daemon.socket` is never bound into a box.
 *
 * Returns: (transfer full): the expanded socket path
 */
gchar *clawt_config_agent_socket_path(ClawtConfig *config,
                                      const gchar *agent_id);

/**
 * clawt_config_display_dir:
 * @config: the fleet configuration
 * @agent_id: the agent
 *
 * Where a display computer's server keeps its logs, its framebuffer and
 * gowl's runtime directory: `<state_dir>/displays/<agent>` (since
 * 0.51.4).  Deliberately not under clawt_config_agent_state_dir(), which
 * an agent's bwrap box has writable: from there an agent could reach
 * gowl's control sockets, whose `spawn` runs outside the box, and plant
 * a symlink where the unconfined daemon opens a log.  No box is granted
 * this directory; only a screen's socket is bound in, by itself.
 *
 * Returns: (transfer full): the expanded directory path
 */
gchar *clawt_config_display_dir(ClawtConfig *config, const gchar *agent_id);

/**
 * clawt_config_render_agent:
 * @config: the fleet configuration
 * @agent: the agent to render
 * @link_socket: the daemon's agent socket, which this agent dials
 * @state_dir: the agent's state directory
 * @error: (out) (optional): return location for a #GError
 *
 * Renders the libreclaw `config.yaml` this agent runs against.
 *
 * clawtilla.yaml is the source of truth and this file is derived from it,
 * so it carries a header saying so.  Hand-editing it would be lost on the
 * next start, and silently -- which is exactly the kind of thing that
 * costs an afternoon.
 *
 * The output is deterministic: same input, same bytes.  That is what lets
 * the daemon rewrite it on every start without churning mtimes or
 * restarting agents whose configuration did not actually change.
 *
 * Returns: (transfer full) (nullable): the YAML, or %NULL on error
 */
gchar *clawt_config_render_agent(ClawtConfig       *config,
                                 ClawtAgentConfig  *agent,
                                 const gchar       *link_socket,
                                 const gchar       *state_dir,
                                 GError           **error);

/**
 * clawt_config_render_agent_check:
 * @config: the fleet configuration
 * @agent: the agent to render
 * @link_socket: the daemon's agent socket, which this agent dials
 * @state_dir: the agent's state directory
 * @error: (out) (optional): return location for a #GError
 *
 * Asks whether @agent would render, as clawt_config_render_agent() would
 * answer, and throws the YAML away.
 *
 * It is quiet: a value the render replaces rather than refuses (a bad
 * `session.timeout_minutes`, say) is not logged, because this render is
 * never used -- the real one that follows says it once.  A refusal is
 * still returned in @error.
 *
 * Returns: %TRUE when the agent renders
 */
gboolean clawt_config_render_agent_check(ClawtConfig       *config,
                                         ClawtAgentConfig  *agent,
                                         const gchar       *link_socket,
                                         const gchar       *state_dir,
                                         GError           **error);

/**
 * clawt_config_write_agent_files:
 * @config: the fleet configuration
 * @agent: the agent
 * @link_socket: the daemon's agent socket
 * @out_config_path: (out) (optional) (transfer full): where the rendered
 *   config was written
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the agent's state directory, resolves its secrets into
 * credential files, writes its link token and renders its config.yaml.
 *
 * Credential files are written 0600 in a 0700 directory.  Secrets are
 * resolved here rather than being written into the YAML so the config
 * stays safe to copy about, and so a rotated secret takes effect on
 * restart without editing anything.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_config_write_agent_files(ClawtConfig       *config,
                                        ClawtAgentConfig  *agent,
                                        const gchar       *link_socket,
                                        gchar            **out_config_path,
                                        GError           **error);

G_END_DECLS
