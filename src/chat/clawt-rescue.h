/*
 * clawt-rescue.h - A turn that has shown nothing at all, and what to do
 * about it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Somebody sends a message and the typing dot appears, and then nothing:
 * no step, no thought, no tool call.  Usually the CLI is slow to start or
 * the provider is queueing it; sometimes it never answers.  After
 * `agents.runtime.first_reply_seconds` with nothing at all, the clients
 * offer the two things that help -- stop it, or run the same message on
 * a fallback model -- instead of leaving a person to wonder.
 *
 * **What "nothing at all" can mean here, honestly.**  libreclaw streams a
 * turn as steps, and the daemon sees: a paragraph of reasoning (a
 * `thinking` step), a tool call, a failed tool.  It does *not* see the
 * CLI's own status lines -- libreclaw drops them, because their
 * vocabulary is per provider -- and it does not see prose being written,
 * which libreclaw holds until a tool call follows it or the turn ends.
 * So "no step yet" distinguishes "thinking out loud" from "silent", but
 * it cannot tell "the CLI emitted nothing" from "the CLI is writing its
 * answer" or "thinking without a paragraph break yet".  The offer is
 * worded as what is known -- no reply and no visible activity -- and
 * never claims the CLI is stuck.  With `stream_steps` off there is no
 * signal at all, and nothing is offered.
 *
 * **What "switch AI" can do.**  A model for one turn is a frame member
 * libreclaw applies when it takes the message off its queue, and it is a
 * model *name on the agent's own CLI*: libreclaw cannot move one turn to
 * another CLI or another login.  So the choices are the agent's
 * `model.fallbacks` entries on its own provider and login; the rest are
 * listed with the reason (clawt_rescue_switch_choices()).
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_RESCUE_WATCH (clawt_rescue_watch_get_type())

G_DECLARE_FINAL_TYPE(ClawtRescueWatch, clawt_rescue_watch, CLAWT,
                     RESCUE_WATCH, GObject)

/**
 * clawt_rescue_watch_new:
 *
 * Returns: (transfer full): a new #ClawtRescueWatch
 */
ClawtRescueWatch *
clawt_rescue_watch_new(
	void
);

/**
 * clawt_rescue_watch_begin:
 * @self: a #ClawtRescueWatch
 * @agent_id: whose turn
 * @room_id: where
 * @now_usec: monotonic microseconds
 *
 * A room's turn began.  Keyed on agent *and* room, because an agent runs
 * a turn per room and one room's silence says nothing about another's.
 */
void
clawt_rescue_watch_begin(
	ClawtRescueWatch *self,
	const gchar      *agent_id,
	const gchar      *room_id,
	gint64            now_usec
);

/**
 * clawt_rescue_watch_note_life:
 * @self: a #ClawtRescueWatch
 * @agent_id: whose turn
 * @room_id: (nullable): where, or %NULL for every room of the agent
 *
 * The turn showed something -- a step or a reply -- so it is not silent
 * and is no longer watched.
 *
 * Returns: %TRUE when the turn had already been offered to rescue, so
 *   the caller says the offer is withdrawn
 */
gboolean
clawt_rescue_watch_note_life(
	ClawtRescueWatch *self,
	const gchar      *agent_id,
	const gchar      *room_id
);

/**
 * clawt_rescue_watch_collect_due:
 * @self: a #ClawtRescueWatch
 * @now_usec: monotonic microseconds
 * @seconds_for: (scope call): how long @agent_id's turns may be silent
 *   before they are offered; 0 means never
 * @user_data: for @seconds_for
 *
 * The turns silent for their agent's whole allowance and not yet
 * offered, each marked offered as it is returned -- once only, so a
 * sweep every five seconds does not repeat the offer.
 *
 * Returns: (transfer full) (element-type utf8): "agent\x1froom" keys
 */
GPtrArray *
clawt_rescue_watch_collect_due(
	ClawtRescueWatch *self,
	gint64            now_usec,
	guint           (*seconds_for)(const gchar *agent_id, gpointer data),
	gpointer          user_data
);

/**
 * clawt_rescue_watch_offered:
 * @self: a #ClawtRescueWatch
 * @agent_id: (nullable): whose, or %NULL for every agent
 *
 * The turns currently offered to rescue, for a client that connects
 * after the event.
 *
 * Returns: (transfer full) (element-type utf8): "agent\x1froom" keys
 */
GPtrArray *
clawt_rescue_watch_offered(
	ClawtRescueWatch *self,
	const gchar      *agent_id
);

/**
 * clawt_rescue_watch_silent_for:
 * @self: a #ClawtRescueWatch
 * @agent_id: whose turn
 * @room_id: where
 * @now_usec: monotonic microseconds
 *
 * Returns: whole seconds since the turn began, or -1 when it is not
 *   being watched
 */
gint64
clawt_rescue_watch_silent_for(
	ClawtRescueWatch *self,
	const gchar      *agent_id,
	const gchar      *room_id,
	gint64            now_usec
);

/**
 * clawt_rescue_key_split:
 * @key: an "agent\x1froom" key
 * @agent_id: (out) (transfer full): the agent
 * @room_id: (out) (transfer full): the room
 *
 * The inverse of the keys this watch hands out.
 */
void
clawt_rescue_key_split(
	const gchar  *key,
	gchar       **agent_id,
	gchar       **room_id
);

/**
 * clawt_rescue_switch_choices:
 * @provider: the agent's `model.provider`
 * @account: (nullable): the agent's `model.account`
 * @fallbacks: (element-type ClawtModelFallback): its `model.fallbacks`
 * @refused: (out) (transfer full) (element-type utf8) (optional): one
 *   sentence per entry that cannot be used for one turn, saying why
 *
 * The models one turn can be moved to: fallback entries on the agent's
 * own CLI (compared through libreclaw's normaliser) and its own login.
 * An entry on another CLI or another login is refused, because a turn's
 * model is a name libreclaw hands the CLI it is already running.
 *
 * Returns: (transfer full) (element-type utf8): model names, in the
 *   fallback list's order, without duplicates
 */
GPtrArray *
clawt_rescue_switch_choices(
	const gchar  *provider,
	const gchar  *account,
	GPtrArray    *fallbacks,
	GPtrArray   **refused
);

/**
 * clawt_rescue_offer_text:
 * @agent_name: who
 * @seconds: how long it has been silent
 *
 * The sentence all three clients draw: what is known, and nothing more.
 *
 * Returns: (transfer full): the sentence
 */
gchar *
clawt_rescue_offer_text(
	const gchar *agent_name,
	gint64       seconds
);

/**
 * clawt_rescue_switch_label:
 * @model: a choice from clawt_rescue_switch_choices()
 *
 * Returns: (transfer full): "Try sonnet instead"
 */
gchar *
clawt_rescue_switch_label(
	const gchar *model
);

G_END_DECLS
