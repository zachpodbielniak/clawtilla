/*
 * clawt-hold.h - Putting the fleet down without losing what it was doing
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

G_BEGIN_DECLS

#define CLAWT_TYPE_HOLD (clawt_hold_get_type())

G_DECLARE_FINAL_TYPE(ClawtHold, clawt_hold, CLAWT, HOLD, GObject)

/**
 * clawt_hold_new:
 * @state_path: where to remember it, normally <state_dir>/hold.yaml
 *
 * A record of which agents are being held, and of what was running when
 * the hold landed.
 *
 * Pause is not stop, and that distinction is the whole feature.  A stop
 * closes the link and takes the process down, losing whatever turn was
 * in flight; a hold stops *delivery* and leaves everything alive, so the
 * turn that is running finishes and the queue behind it stays queued.
 * The mailbox is durable precisely so work can outlive an agent that
 * cannot take it yet.
 *
 * Remembered on disk because the restart is the thing it exists to
 * serve.  An in-memory hold cannot survive the event it was taken for,
 * and after a restart which agents come back is decided by each one's
 * `runtime.autostart` -- that is, by configuration rather than by what
 * was actually running a second earlier.  An operator running six of
 * twenty-four gets back whatever the config says, which is a different
 * six.
 *
 * Returns: (transfer full): a new #ClawtHold, empty until loaded
 */
ClawtHold *clawt_hold_new(const gchar *state_path);

/**
 * clawt_hold_load:
 * @self: a #ClawtHold
 *
 * Reads the record back, if there is one.
 *
 * A file that cannot be read is a warning and an empty hold, never a
 * refusal to start: this is bookkeeping about a pause, and failing to
 * bring a fleet up because a note about it is corrupt trades a small
 * loss for a total one.
 */
void clawt_hold_load(ClawtHold *self);

/**
 * clawt_hold_save:
 * @self: a #ClawtHold
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the record, atomically.  An empty hold removes the file rather
 * than writing an empty one, so "there is no hold" and "there is a file
 * saying nothing" cannot become two answers to one question.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_hold_save(ClawtHold *self, GError **error);

/**
 * clawt_hold_apply:
 * @self: a #ClawtHold
 * @agent_id: (nullable): one agent, or %NULL for the whole fleet
 *
 * Puts a hold on.  A fleet hold covers agents that do not exist yet,
 * which is deliberate: an agent added while the fleet is held must not
 * start taking work the moment it appears.
 */
void clawt_hold_apply(ClawtHold *self, const gchar *agent_id);

/**
 * clawt_hold_release:
 * @self: a #ClawtHold
 * @agent_id: (nullable): one agent, or %NULL for everything
 *
 * Takes a hold off.
 *
 * Releasing one agent while the fleet is held is refused rather than
 * silently doing nothing: the fleet hold would still cover it, so a
 * caller told it succeeded would be watching an agent that never moves.
 *
 * Returns: %TRUE if anything changed
 */
gboolean clawt_hold_release(ClawtHold *self, const gchar *agent_id);

/**
 * clawt_hold_covers:
 * @self: a #ClawtHold
 * @agent_id: an agent
 *
 * Returns: %TRUE when delivery to @agent_id is being held
 */
gboolean clawt_hold_covers(ClawtHold *self, const gchar *agent_id);

/**
 * clawt_hold_is_fleet:
 * @self: a #ClawtHold
 *
 * Returns: %TRUE when the hold is on the whole fleet rather than on
 *   named agents
 */
gboolean clawt_hold_is_fleet(ClawtHold *self);

/**
 * clawt_hold_is_any:
 * @self: a #ClawtHold
 *
 * Returns: %TRUE when anything at all is held
 */
gboolean clawt_hold_is_any(ClawtHold *self);

/**
 * clawt_hold_get_since:
 * @self: a #ClawtHold
 *
 * Returns: when the hold was taken, in microseconds, or 0
 */
gint64 clawt_hold_get_since(ClawtHold *self);

/**
 * clawt_hold_set_running:
 * @self: a #ClawtHold
 * @agent_ids: (element-type utf8): the agents that were running
 *
 * Remembers what to put back.
 *
 * Recorded at the moment the hold lands rather than worked out at resume
 * time, because by then the answer is gone -- and it is the *actual*
 * running set, not the autostart list, which is the whole point.
 */
void clawt_hold_set_running(ClawtHold *self, GPtrArray *agent_ids);

/**
 * clawt_hold_get_running:
 * @self: a #ClawtHold
 *
 * Returns: (transfer none) (element-type utf8): the remembered running
 *   set, which may be empty
 */
GPtrArray *clawt_hold_get_running(ClawtHold *self);

/**
 * clawt_hold_held_agents:
 * @self: a #ClawtHold
 *
 * Returns: (transfer container) (element-type utf8): the individually
 *   named agents, in a stable order.  Empty for a fleet hold, which
 *   names nobody by design
 */
GPtrArray *clawt_hold_held_agents(ClawtHold *self);

/**
 * clawt_hold_label:
 * @held: whether an operator is holding this agent
 * @draining: whether it is still finishing a turn
 *
 * What a client says about a held agent, in the one spelling all three
 * use.
 *
 * Held is not an agent state and must not be drawn as one: a held agent
 * is running -- process up, link attached -- and only *delivery* has
 * stopped.  Drawing it as stopped is the thing this feature exists to
 * avoid, because held and stopped mean opposite things about whether
 * work was lost.
 *
 * "draining" and "held" are two different answers to the only question
 * an operator is actually asking, which is whether it is safe to
 * restart yet.
 *
 * Returns: (transfer full) (nullable): a short phrase, or %NULL when
 *   nothing is being held -- so an unheld agent costs a caller no badge
 *   and no column text
 */
gchar *clawt_hold_label(gboolean held, gboolean draining);

G_END_DECLS
