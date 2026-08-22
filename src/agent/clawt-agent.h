/*
 * clawt-agent.h - One agent in the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Ties together the four things an agent is made of: its configuration, the
 * runtime hosting its libreclaw instance, its mailbox, and its link to the
 * daemon once it connects.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "agent/clawt-agent-runtime.h"
#include "config/clawt-config.h"
#include "link/clawt-link.h"
#include "mailbox/clawt-mailbox.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_AGENT (clawt_agent_get_type())

G_DECLARE_FINAL_TYPE(ClawtAgent, clawt_agent, CLAWT, AGENT, GObject)

/**
 * clawt_agent_new:
 * @config: (transfer none): the agent's configuration
 * @mailbox: (transfer none) (nullable): its queue
 *
 * Creates an agent in the stopped state, or shadow if its configuration
 * could not be understood.
 *
 * Returns: (transfer full): a new #ClawtAgent
 */
ClawtAgent *clawt_agent_new(ClawtAgentConfig *config,
                            ClawtMailbox     *mailbox);

const gchar      *clawt_agent_get_id(ClawtAgent *self);
const gchar      *clawt_agent_get_name(ClawtAgent *self);
const gchar      *clawt_agent_get_description(ClawtAgent *self);
ClawtAgentState   clawt_agent_get_state(ClawtAgent *self);
ClawtAgentCaps    clawt_agent_get_caps(ClawtAgent *self);

/**
 * clawt_agent_get_config:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none): the agent's configuration
 */
ClawtAgentConfig *clawt_agent_get_config(ClawtAgent *self);

/**
 * clawt_agent_set_config:
 * @self: a #ClawtAgent
 * @config: (transfer none): the agent's configuration
 *
 * Replaces the agent's configuration and recomputes its capabilities.
 *
 * Used when the fleet configuration is reloaded: the agent keeps running,
 * with its runtime, computer and link intact, but its settings come from
 * the newly parsed file.  The old configuration belongs to a #ClawtConfig
 * that is about to be freed, so holding on to it is not an option.
 *
 * Changes that only take effect at start -- the model, the computer type
 * -- apply on the agent's next restart, which is deliberate: a reload
 * that restarted every agent mid-turn would make editing one description
 * cost the whole fleet's work.
 */
void clawt_agent_set_config(ClawtAgent *self, ClawtAgentConfig *config);

/**
 * clawt_agent_get_mailbox:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): the agent's queue, or %NULL if it
 *   could not be opened
 */
ClawtMailbox     *clawt_agent_get_mailbox(ClawtAgent *self);

/**
 * clawt_agent_is_chief_of_staff:
 * @self: a #ClawtAgent
 *
 * Returns: %TRUE if work addressed to the fleet comes here
 */
gboolean clawt_agent_is_chief_of_staff(ClawtAgent *self);

/**
 * clawt_agent_get_status_detail:
 * @self: a #ClawtAgent
 *
 * A sentence explaining the current state: why it is a shadow, why it
 * stopped, or what it is waiting for.
 *
 * Returns: (transfer none) (nullable): the explanation
 */
const gchar *clawt_agent_get_status_detail(ClawtAgent *self);

/**
 * clawt_agent_set_runtime:
 * @self: a #ClawtAgent
 * @runtime: (transfer none) (nullable): how to host it
 *
 * Attaches the runtime.  Capabilities are recomputed, since what an agent
 * can do depends partly on how it is hosted.
 */
void clawt_agent_set_runtime(ClawtAgent        *self,
                             ClawtAgentRuntime *runtime);

/**
 * clawt_agent_get_runtime:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): the runtime
 */
ClawtAgentRuntime *clawt_agent_get_runtime(ClawtAgent *self);

/**
 * clawt_agent_set_computer:
 * @self: a #ClawtAgent
 * @computer: (transfer none) (nullable): what it can run commands on
 *
 * Attaches the computer and recomputes capabilities.
 */
void clawt_agent_set_computer(ClawtAgent    *self,
                              ClawtComputer *computer);

/**
 * clawt_agent_get_computer:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): the computer
 */
ClawtComputer *clawt_agent_get_computer(ClawtAgent *self);

/**
 * clawt_agent_set_link:
 * @self: a #ClawtAgent
 * @link_: (transfer none) (nullable): the connection, or %NULL when it goes
 *
 * Attaches or clears the link.  An agent whose runtime is up but which has
 * not connected is degraded, not running -- it cannot be reached.
 */
void clawt_agent_set_link(ClawtAgent *self,
                          ClawtLink  *link_);

/**
 * clawt_agent_get_link:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): the link
 */
ClawtLink *clawt_agent_get_link(ClawtAgent *self);

/**
 * clawt_agent_get_hop_depth:
 * @self: a #ClawtAgent
 *
 * How far the message this agent is currently handling had already
 * travelled.
 *
 * Anything the agent sends in response is one hop further on.  Without
 * this the depth had to be guessed, and the guess was always 1 -- which
 * made max_hops unreachable no matter how long the chain got, since every
 * message looked like the first.
 *
 * Returns: the depth of the message last delivered to this agent
 */
gint clawt_agent_get_hop_depth(ClawtAgent *self);

/**
 * clawt_agent_set_hop_depth:
 * @self: a #ClawtAgent
 * @depth: the depth of the message being delivered
 *
 * Recorded by the router as it hands a message over.
 */
void clawt_agent_set_hop_depth(ClawtAgent *self, gint depth);

/**
 * clawt_agent_start:
 * @self: a #ClawtAgent
 * @error: (out) (optional): return location for a #GError
 *
 * Starts the agent.  A shadow refuses, explaining why.
 *
 * Returns: %TRUE if it was started
 */
gboolean clawt_agent_start(ClawtAgent  *self,
                           GError     **error);

/**
 * clawt_agent_stop:
 * @self: a #ClawtAgent
 *
 * Stops the agent.  Its mailbox is untouched: messages queued for it wait
 * for it to come back, which is the point of having a mailbox.
 */
void clawt_agent_stop(ClawtAgent *self);

/**
 * clawt_agent_mark_shadow:
 * @self: a #ClawtAgent
 * @reason: why it cannot run
 *
 * Puts the agent into shadow state.
 */
void clawt_agent_mark_shadow(ClawtAgent  *self,
                             const gchar *reason);

/**
 * clawt_agent_set_error:
 * @self: a #ClawtAgent
 * @reason: what went wrong
 *
 * Puts the agent into error state.
 *
 * Use this rather than clawt_agent_mark_shadow() for anything that could
 * succeed on a retry -- a container runtime that is down, an image that
 * is not pulled. Shadow means the configuration itself cannot be
 * understood, and it refuses every later start with the reason frozen
 * from the first failure.
 */
void clawt_agent_set_error(ClawtAgent  *self,
                           const gchar *reason);

G_END_DECLS
