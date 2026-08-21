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
