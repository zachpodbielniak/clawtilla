/*
 * clawt-takeover.h - A person taking the screen back from the agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two people cannot drive one pointer.  Watching an agent work is the
 * common case and needs nothing; stepping in and doing something
 * yourself needs the agent to stop, and stopping it needs saying which
 * of the two is holding the mouse.
 *
 * Three decisions are recorded here rather than left to each caller,
 * because each of them was arrived at by imagining the alternative:
 *
 * **A refused action is not a queued one.**  Queuing the agent's clicks
 * until the person lets go sounds kinder and is much worse: a click
 * queued during a takeover lands after the person has moved on, on
 * whatever happens to be under it then.  The agent is told nothing
 * happened, which is true and is something it can act on.
 *
 * **Only a person takes and releases.**  The agent can ask -- that is
 * clawt_takeover_request() -- and the asking is a message, not a
 * handover.  An agent that could hand itself the screen would be able to
 * take it back from somebody mid-sentence.
 *
 * **The lease expires.**  A browser tab closed mid-takeover would
 * otherwise lock the agent out for ever, with nothing on any screen to
 * say why, and the fleet would look like it had stopped working.
 *
 * The check that reads this **fails open**, and that is deliberate.
 * This is cooperation between an operator and their own agent, not a
 * boundary against a hostile one: nothing here is protecting a secret or
 * confining a program, and the worst a lost race does is put one click
 * somewhere unexpected on a screen a person is already looking at.  A
 * daemon that cannot be reached bricking every computer in the fleet
 * mid-turn costs a great deal more than that.  Every real permission
 * boundary in clawtilla -- `allow_input`, `allow_spawn`, the connector
 * grants -- fails closed, and none of them is this.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_TAKEOVER (clawt_takeover_get_type())

G_DECLARE_FINAL_TYPE(ClawtTakeover, clawt_takeover, CLAWT, TAKEOVER, GObject)

/**
 * clawt_takeover_new:
 *
 * Returns: (transfer full): a new #ClawtTakeover, holding nothing
 */
ClawtTakeover *clawt_takeover_new(void);

/**
 * clawt_takeover_take:
 * @self: a #ClawtTakeover
 * @agent_id: whose screen
 * @holder: (nullable): who is holding it, for the other clients to show
 * @lease_seconds: how long before it lapses
 * @error: (out) (optional): return location for a #GError
 *
 * Takes the screen for a person.
 *
 * Taking a screen somebody else already holds is refused rather than
 * stolen, and the refusal names them: two operators fighting over one
 * pointer is the same problem this exists to solve, one layer up.
 * Taking one you already hold is not an error -- it extends the lease,
 * which is what a client sends while somebody is still working.
 *
 * Returns: %TRUE if the screen is now held
 */
gboolean clawt_takeover_take(ClawtTakeover  *self,
                             const gchar    *agent_id,
                             const gchar    *holder,
                             gint64          lease_seconds,
                             GError        **error);

/**
 * clawt_takeover_release:
 * @self: a #ClawtTakeover
 * @agent_id: whose screen
 *
 * Gives the screen back.
 *
 * Releasing also settles any open request for hands, so there is no
 * separate "I am done helping" step to forget.  The two are the same
 * event from opposite sides, and a request left open after the person
 * has finished is an agent that goes on waiting for something that
 * already happened.
 *
 * Returns: %TRUE if it had been held
 */
gboolean clawt_takeover_release(ClawtTakeover *self, const gchar *agent_id);

/**
 * clawt_takeover_is_held:
 * @self: a #ClawtTakeover
 * @agent_id: whose screen
 *
 * Whether a person is holding this screen right now.
 *
 * The expiry is applied here rather than by a timer, so a lapsed lease
 * is never observable: a timer that had not run yet would leave an agent
 * refused for however long the main loop happened to be busy, which is
 * the sort of bug that only appears under load.
 *
 * Returns: %TRUE if the agent's input should be refused
 */
gboolean clawt_takeover_is_held(ClawtTakeover *self, const gchar *agent_id);

/**
 * clawt_takeover_get_holder:
 * @self: a #ClawtTakeover
 * @agent_id: whose screen
 *
 * Returns: (transfer none) (nullable): who is holding it
 */
const gchar *clawt_takeover_get_holder(ClawtTakeover *self,
                                       const gchar   *agent_id);

/**
 * clawt_takeover_get_expires_at:
 * @self: a #ClawtTakeover
 * @agent_id: whose screen
 *
 * Returns: when the hold lapses, in microseconds since the epoch, or 0
 *   if nobody holds it
 */
gint64 clawt_takeover_get_expires_at(ClawtTakeover *self,
                                     const gchar   *agent_id);

/**
 * clawt_takeover_request:
 * @self: a #ClawtTakeover
 * @agent_id: which agent is asking
 * @reason: (nullable): what it needs a person for
 *
 * The agent asking somebody to take the screen.
 *
 * All this does is record the ask and raise ::changed, so a client can
 * show it.  It emphatically does not hand the screen over: an agent that
 * could would be able to take the pointer out from under somebody
 * mid-sentence.
 *
 * Returns: %TRUE if the request was recorded
 */
gboolean clawt_takeover_request(ClawtTakeover *self,
                                const gchar   *agent_id,
                                const gchar   *reason);

/**
 * clawt_takeover_get_request:
 * @self: a #ClawtTakeover
 * @agent_id: which agent
 *
 * Returns: (transfer none) (nullable): why it wants a person, or %NULL
 */
const gchar *clawt_takeover_get_request(ClawtTakeover *self,
                                        const gchar   *agent_id);

/**
 * clawt_takeover_clear_agent:
 * @self: a #ClawtTakeover
 * @agent_id: which agent
 *
 * Forgets everything about this agent's screen.
 *
 * Called when the agent is removed, so a lease does not outlive the
 * thing it was about.
 */
void clawt_takeover_clear_agent(ClawtTakeover *self, const gchar *agent_id);

/**
 * clawt_takeover_refusal_text:
 *
 * What an agent is told when its input is refused because a person has
 * the screen.
 *
 * One invariant sentence rather than a message per call site, and it has
 * to say three things or the agent does the wrong thing with it:
 *
 * - **nothing happened**, so it does not carry on as if the click landed;
 * - **do not retry**, because retrying in a loop burns a turn and every
 *   attempt is refused for the same reason;
 * - **how to wait properly**, because an agent told only "no" invents a
 *   way to wait, and the ones it invents are polling loops.
 *
 * Returns: (transfer none): the refusal
 */
const gchar *clawt_takeover_refusal_text(void);

G_END_DECLS
