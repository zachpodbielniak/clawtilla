/*
 * clawt-agent-manager.h - The fleet
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
#include "agent/clawt-agent.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_AGENT_MANAGER (clawt_agent_manager_get_type())

G_DECLARE_FINAL_TYPE(ClawtAgentManager, clawt_agent_manager,
                     CLAWT, AGENT_MANAGER, GObject)

/**
 * clawt_agent_manager_new:
 * @config: (transfer none): the configuration
 *
 * Returns: (transfer full): a new #ClawtAgentManager
 */
ClawtAgentManager *clawt_agent_manager_new(ClawtConfig *config);

/**
 * clawt_agent_manager_load:
 * @self: a #ClawtAgentManager
 * @error: (out) (optional): return location for a #GError
 *
 * Reconciles the fleet against the configuration: agents that are new
 * are built, agents that are gone are stopped and dropped, and agents
 * that remain keep running with their settings refreshed.
 *
 * Deliberately not a rebuild.  It used to empty the fleet and construct
 * everything afresh, so adding one agent destroyed the live object --
 * runtime, computer and link -- of every other agent already working.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_agent_manager_load(ClawtAgentManager  *self,
                                  GError            **error);

/**
 * clawt_agent_manager_set_config:
 * @self: a #ClawtAgentManager
 * @config: (transfer none): the fleet configuration
 *
 * Points the manager at a newly loaded configuration.
 *
 * Needed because the manager holds its own reference: without this, a
 * reload swapped the daemon's configuration while the manager kept
 * reading the old one for ever, so nothing a reload changed ever reached
 * the fleet.
 */
void clawt_agent_manager_set_config(ClawtAgentManager *self,
                                    ClawtConfig       *config);

/**
 * clawt_agent_manager_list:
 * @self: a #ClawtAgentManager
 *
 * Returns: (transfer none) (element-type ClawtAgent): every agent,
 *   in configuration order
 */
/**
 * clawt_agent_manager_get_config:
 * @self: a #ClawtAgentManager
 *
 * The fleet configuration the manager built its agents from.
 *
 * Returns: (transfer none) (nullable): the configuration
 */
ClawtConfig *clawt_agent_manager_get_config(ClawtAgentManager *self);

GPtrArray *clawt_agent_manager_list(ClawtAgentManager *self);

/**
 * clawt_agent_manager_get:
 * @self: a #ClawtAgentManager
 * @agent_id: an agent id
 *
 * Returns: (transfer none) (nullable): the agent, or %NULL
 */
ClawtAgent *clawt_agent_manager_get(ClawtAgentManager *self,
                                    const gchar       *agent_id);

/**
 * clawt_agent_manager_get_chief_of_staff:
 * @self: a #ClawtAgentManager
 *
 * Returns: (transfer none) (nullable): the agent work addressed to the
 *   fleet goes to, or %NULL if none is designated
 */
ClawtAgent *clawt_agent_manager_get_chief_of_staff(ClawtAgentManager *self);

/**
 * clawt_agent_manager_start_all:
 * @self: a #ClawtAgentManager
 *
 * Starts every agent marked autostart.
 *
 * One agent failing to start does not stop the others: a fleet where a
 * single misconfigured agent prevents the rest from running would be worse
 * than one that starts nine of ten and says which failed.
 *
 * Returns: how many started
 */
guint clawt_agent_manager_start_all(ClawtAgentManager *self);

/**
 * clawt_agent_manager_stop_all:
 * @self: a #ClawtAgentManager
 *
 * Stops every running agent.
 */
void clawt_agent_manager_stop_all(ClawtAgentManager *self);

/**
 * clawt_agent_manager_set_state_dir:
 * @self: a #ClawtAgentManager
 * @state_dir: where mailboxes and per-agent state live
 *
 * Sets where per-agent state is kept.  Must be called before
 * clawt_agent_manager_load().
 */
void clawt_agent_manager_set_state_dir(ClawtAgentManager *self,
                                       const gchar       *state_dir);

G_END_DECLS
