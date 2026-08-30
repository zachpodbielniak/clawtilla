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
#include "memory/clawt-memory-store.h"

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

void clawt_agent_revalidate(ClawtAgent *self);

/**
 * clawt_agent_get_mailbox:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): the agent's queue, or %NULL if it
 *   could not be opened
 */
ClawtMailbox     *clawt_agent_get_mailbox(ClawtAgent *self);

/**
 * clawt_agent_set_memory:
 * @self: a #ClawtAgent
 * @memory: (nullable): the agent's memory store
 *
 * Gives the agent somewhere to remember things.
 */
void              clawt_agent_set_memory(ClawtAgent       *self,
                                         ClawtMemoryStore *memory);

/**
 * clawt_agent_get_memory:
 * @self: a #ClawtAgent
 *
 * The agent's own memory store, and only its own.
 *
 * Returns: (transfer none) (nullable): the store, or %NULL when memory
 *   is turned off for this agent
 */
ClawtMemoryStore *clawt_agent_get_memory(ClawtAgent *self);

/**
 * clawt_agent_set_activity:
 * @self: a #ClawtAgent
 * @busy: whether a turn is in progress
 * @peer: (nullable): who the turn is for, or %NULL for the operator
 *
 * Records what the agent is doing at this moment.
 *
 * libreclaw raises its typing indicator for the whole turn, which is
 * what @busy follows. @peer is who the message being answered came
 * from, which is the part a person cannot otherwise see: an agent can
 * be busy for minutes on something another agent asked it, and from
 * outside that is indistinguishable from being busy for you.
 */
void              clawt_agent_set_activity(ClawtAgent  *self,
                                           gboolean     busy,
                                           const gchar *peer);

/**
 * clawt_agent_get_busy:
 * @self: a #ClawtAgent
 *
 * Returns: %TRUE while a turn is in progress
 */
gboolean          clawt_agent_get_busy(ClawtAgent *self);

/**
 * clawt_agent_get_activity_peer:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): who the current or last turn was
 *   for, or %NULL when it was the operator
 */
const gchar      *clawt_agent_get_activity_peer(ClawtAgent *self);

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
 * clawt_agent_set_desktop:
 * @self: a #ClawtAgent
 * @desktop: (nullable) (transfer none): the desktop it may drive
 *
 * Kept beside the computer rather than inside it, because a desktop is an
 * add-on: an agent can have one alongside whichever computer it was given,
 * and an agent with no computer at all can still be pointed at the host's
 * screen.
 */
void clawt_agent_set_desktop(ClawtAgent   *self,
                             ClawtDesktop *desktop);

/**
 * clawt_agent_get_desktop:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): the desktop, or %NULL
 */
ClawtDesktop *clawt_agent_get_desktop(ClawtAgent *self);

/**
 * clawt_agent_describe_computer:
 * @self: a #ClawtAgent
 *
 * What this agent has to work with, for its own prompt: the computer and
 * the desktop together.
 *
 * The two are described in one place because they are separate objects
 * and the agent does not experience them separately.  A desktop is an
 * add-on the computer has never heard of, so asking the computer alone
 * produced a description that never mentioned the screen the agent had
 * just been handed the tools to drive.
 *
 * Returns: (transfer full): the description
 */
gchar *clawt_agent_describe_computer(ClawtAgent *self);

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
 * clawt_agent_deliver_turn:
 * @self: a #ClawtAgent
 * @depth: how far the message being delivered had already travelled
 * @replies: %TRUE if the turn's closing text should be delivered
 * @from: (nullable): who the message came from
 *
 * Records one delivery, and the turn it will set up.
 *
 * The router calls this once per message it hands over.  One call is one
 * entry, and clawt_agent_begin_turn() takes one entry per turn, because
 * libreclaw runs one turn per message: a drain that hands over five
 * messages produces five turns and each has to know what it is
 * answering.
 *
 * Distinct from the three setters below, which amend the delivery
 * already at the tail rather than starting another.  Those exist for a
 * caller describing a single delivery a field at a time; describing
 * several that way would produce one entry for all of them, since there
 * is no end-of-delivery edge for them to find.
 */
void clawt_agent_deliver_turn(ClawtAgent  *self,
                              gint         depth,
                              gboolean     replies,
                              const gchar *from);

/**
 * clawt_agent_set_turn_origin:
 * @self: a #ClawtAgent
 * @from: (nullable): who the message being delivered came from
 *
 * Records who started the turn about to run.
 *
 * Set at delivery beside the hop depth, and spent by the same
 * clawt_agent_begin_turn() -- the two answer the same question one field
 * apart, and setting them together is what keeps them from disagreeing
 * about which turn they describe.  They land in one queued entry per
 * delivery, so a burst describes each of its turns rather than only the
 * first.
 *
 * Distinct from clawt_agent_get_activity_peer(), which is kept after the
 * turn ends so a stopped agent can still say who its last one was for.
 * This one is only true of the turn actually running.
 */
void clawt_agent_set_turn_origin(ClawtAgent *self, const gchar *from);

/**
 * clawt_agent_get_turn_origin:
 * @self: a #ClawtAgent
 *
 * Returns: (transfer none) (nullable): who started this turn, or %NULL
 *   when nothing the daemon can see did
 */
const gchar *clawt_agent_get_turn_origin(ClawtAgent *self);

/**
 * clawt_agent_set_turn_replies:
 * @self: a #ClawtAgent
 * @replies: %TRUE if this turn's ordinary output should be delivered
 *
 * Says whether the text this turn ends with is a message to send.
 *
 * Set at delivery beside the hop depth and the turn origin, from
 * clawt_mailbox_item_get_invites_reply(), and spent by the same
 * clawt_agent_begin_turn().  Per delivery, not per drain: libreclaw runs
 * one turn per message, so an acknowledgement queued behind a question
 * gets its own turn and its own answer to this.
 *
 * A turn cannot decline to produce text: an AI CLI answers whatever it
 * is handed, so "reply only if you have something to say" was advice no
 * agent could follow, and two of them acknowledging each other ran until
 * `orchestration.max_hops` cut it off eight turns later.  This is the
 * mechanism behind that advice -- a reply earns no reply, so an exchange
 * settles at one -- and the delivery preamble says so, because an agent
 * whose closing text goes nowhere has to be told to use
 * clawtilla_message_agent when it genuinely does need to reach somebody.
 */
void clawt_agent_set_turn_replies(ClawtAgent *self, gboolean replies);

/**
 * clawt_agent_get_turn_replies:
 * @self: a #ClawtAgent
 *
 * Returns: %TRUE if the daemon should route what this turn says
 */
gboolean clawt_agent_get_turn_replies(ClawtAgent *self);

/**
 * clawt_agent_begin_turn:
 * @self: a #ClawtAgent
 *
 * Tells the agent a turn is starting, so it knows which chain it is on.
 *
 * Takes the oldest delivery that has not had a turn yet and makes it
 * this turn's: the depth, who asked, and whether the closing text is a
 * message.  One entry, because libreclaw runs one turn per message --
 * LcSession queues them and pops a single entry per turn -- so a drain
 * that hands over five messages produces five turns and each has to know
 * what it is answering.  Taking them all at once, which is what a single
 * "a delivery set this up" flag amounted to, left four turns looking
 * like turns nothing delivered into: depth zero, no origin, and free to
 * reply.
 *
 * A depth set by a delivery is kept for the whole turn -- every message
 * the agent sends counts from the same place, because a turn is not one
 * message.  A turn that no delivery preceded starts from zero rather
 * than inheriting the last one, which is what stops an agent that
 * answers Matrix between two peer messages from running out of hops.
 */
void clawt_agent_begin_turn(ClawtAgent *self);

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
