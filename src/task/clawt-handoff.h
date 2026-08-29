/*
 * clawt-handoff.h - Handing ownership of a task to somebody else
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A handoff is not a delegation.  Delegating creates *new* work and
 * keeps the delegator responsible for it; handing off moves work that
 * already exists onto somebody else's list and stops being yours.  They
 * are different enough that sharing one verb was the reason a chief of
 * staff kept re-delegating things it had already passed on -- the task
 * came back with the same origin, so it read as work it still owed.
 *
 * Two properties are the whole design:
 *
 *   **It runs when the source turn settles, not when it is called.**  An
 *   agent asking for a handoff must not wait for the recipient, because
 *   the recipient may be mid-turn and a turn is minutes.  So the call
 *   queues and answers at once, and #ClawtDaemon drains the queue from
 *   clawt_daemon_turn_settle().
 *
 *   **It leaves a durable receipt.**  #ClawtTaskManager is in memory, so
 *   a task handed over just before a restart is a task nothing can
 *   answer questions about afterwards -- and "I do not know" from
 *   clawtilla_task_status reads to an agent as "it never happened",
 *   which is how you get two of everything.  #ClawtHandoffStore keeps a
 *   terminal receipt per handoff for orchestration.handoff_receipt_days.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_HANDOFF (clawt_handoff_get_type())

GType clawt_handoff_get_type(void) G_GNUC_CONST;

/**
 * clawt_handoff_new:
 * @task_id: the task whose ownership is moving
 * @from_agent: who is giving it up
 * @to_agent: who is taking it on
 * @reason: (nullable): why, for the audit trail and for the recipient
 *
 * Creates a handoff in the %CLAWT_HANDOFF_QUEUED state.
 *
 * Ordinary code goes through #ClawtHandoffStore, which is what makes it
 * survive a restart; this exists so the record can be built and reasoned
 * about without a database.
 *
 * Returns: (transfer full): a new #ClawtHandoff
 */
ClawtHandoff *clawt_handoff_new(const gchar *task_id,
                                const gchar *from_agent,
                                const gchar *to_agent,
                                const gchar *reason);

/**
 * clawt_handoff_copy:
 * @self: a #ClawtHandoff
 *
 * Returns: (transfer full): a copy
 */
ClawtHandoff *clawt_handoff_copy(ClawtHandoff *self);
void          clawt_handoff_free(ClawtHandoff *self);

/**
 * clawt_handoff_get_id:
 * @self: a #ClawtHandoff
 *
 * The accessors below are plain reads.  Every string getter is
 * (transfer none) and may be %NULL for the optional fields -- reason,
 * room and verdict are all absent until something sets them.
 *
 * Returns: (transfer none): the handoff's identifier
 */
const gchar *clawt_handoff_get_id(ClawtHandoff *self);
const gchar *clawt_handoff_get_task_id(ClawtHandoff *self);
const gchar *clawt_handoff_get_from_agent(ClawtHandoff *self);
const gchar *clawt_handoff_get_to_agent(ClawtHandoff *self);
const gchar *clawt_handoff_get_reason(ClawtHandoff *self);
const gchar *clawt_handoff_get_room(ClawtHandoff *self);

/**
 * clawt_handoff_get_verdict:
 * @self: a #ClawtHandoff
 *
 * What happened, in a sentence somebody can act on.
 *
 * A sentence and not the enum's nickname, because this is what an agent
 * reads back out of clawtilla_task_status.  `too_deep` tells it nothing
 * it can do; "delegation chains are limited to one hop -- do this one
 * yourself" tells it exactly what to do next.  The enum is for
 * filtering and counting; this is for reading.
 *
 * Returns: (transfer none) (nullable): the verdict, or %NULL while queued
 */
const gchar *clawt_handoff_get_verdict(ClawtHandoff *self);

ClawtHandoffState clawt_handoff_get_state(ClawtHandoff *self);
guint             clawt_handoff_get_attempts(ClawtHandoff *self);

/**
 * clawt_handoff_get_depth:
 * @self: a #ClawtHandoff
 *
 * How far from the original request the delivery to the new owner will
 * be stamped.
 *
 * Recorded when the handoff is queued rather than worked out when it
 * runs, because by then the turn that asked for it has ended and the
 * agent's own hop depth has been dropped for the next one.  A handoff
 * delivered at depth 1 would restart the chain, and a fleet could pass
 * one task round for ever without ever reaching orchestration.max_hops.
 *
 * Returns: the depth to deliver at
 */
gint              clawt_handoff_get_depth(ClawtHandoff *self);
gint64            clawt_handoff_get_created_at(ClawtHandoff *self);
gint64            clawt_handoff_get_settled_at(ClawtHandoff *self);

/**
 * clawt_handoff_set_id:
 * @self: a #ClawtHandoff
 * @id: the identifier
 *
 * The setters below all take %NULL to clear the field, and exist for the
 * store to read a row back into a record.
 */
void clawt_handoff_set_id(ClawtHandoff *self, const gchar *id);
void clawt_handoff_set_reason(ClawtHandoff *self, const gchar *reason);
void clawt_handoff_set_room(ClawtHandoff *self, const gchar *room);
void clawt_handoff_set_verdict(ClawtHandoff *self, const gchar *verdict);
void clawt_handoff_set_attempts(ClawtHandoff *self, guint attempts);
void clawt_handoff_set_depth(ClawtHandoff *self, gint depth);
void clawt_handoff_set_created_at(ClawtHandoff *self, gint64 created_at);
void clawt_handoff_set_settled_at(ClawtHandoff *self, gint64 settled_at);

/**
 * clawt_handoff_set_state:
 * @self: a #ClawtHandoff
 * @state: the new state
 *
 * Stamps @settled_at the first time @state is terminal, so "how long did
 * this wait" stays answerable.
 */
void clawt_handoff_set_state(ClawtHandoff *self, ClawtHandoffState state);

/**
 * clawt_handoff_is_settled:
 * @self: a #ClawtHandoff
 *
 * Returns: %TRUE if the handoff will not change state again
 */
gboolean clawt_handoff_is_settled(ClawtHandoff *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtHandoff, clawt_handoff_free)

G_END_DECLS
