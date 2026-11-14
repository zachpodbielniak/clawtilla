/*
 * clawt-round.h - Rounds and threads of replies in a group room
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_ROUND_H
#define CLAWT_ROUND_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/*
 * A *round* of a group room starts at a message from a person, a
 * routine or a trigger.  A *chain* is one thread of replies within it:
 * the members who answered, in order, each one reached by the one
 * before.  A chain is carried as a comma-separated list of agent ids,
 * because it travels through the mailbox's SQLite row, the room's JSON
 * transcript and the IPC wire, and every one of those already holds a
 * string.  Agent ids cannot contain a comma (clawt_is_valid_id()),
 * so the separator is unambiguous.
 */

/**
 * clawt_round_chain_length:
 * @chain: (nullable): a chain, or %NULL / "" for none
 *
 * How many members have answered in @chain.
 *
 * Returns: the number of members
 */
guint clawt_round_chain_length(const gchar *chain);

/**
 * clawt_round_chain_has:
 * @chain: (nullable): a chain
 * @member: (nullable): an agent id
 *
 * Whether @member has already answered in @chain.  An exact match on a
 * whole entry, so `bob` is not found in `bobby`.
 *
 * Returns: %TRUE if @member is in @chain
 */
gboolean clawt_round_chain_has(const gchar *chain, const gchar *member);

/**
 * clawt_round_chain_extend:
 * @chain: (nullable): the chain being answered
 * @member: the member answering it
 *
 * @chain with @member appended, or @chain unchanged when @member is
 * already in it -- a chain is a set of who has answered, kept in the
 * order they did.
 *
 * Returns: (transfer full): the extended chain
 */
gchar *clawt_round_chain_extend(const gchar *chain, const gchar *member);

/**
 * clawt_round_chain_union:
 * @first: (nullable): a chain
 * @second: (nullable): another chain
 *
 * Every member of either, @first's order first.  Used where several
 * deliveries become one turn, so the merged turn is held to every
 * thread it answers -- the safe direction, since a member skipped once
 * too often costs one mention and a member reached twice costs a turn.
 *
 * Returns: (transfer full) (nullable): the union, or %NULL when both
 *   are empty
 */
gchar *clawt_round_chain_union(const gchar *first, const gchar *second);

/**
 * clawt_round_label:
 * @round: a round number, from 1
 *
 * What every client draws where a round begins, from one place so three
 * clients cannot word it three ways.
 *
 * Returns: (transfer full): the label, "Round 4"
 */
gchar *clawt_round_label(guint round);

/**
 * clawt_round_skip_note:
 * @skipped: (nullable): the members a message was not delivered to,
 *   comma-separated
 * @reason: (nullable): why, as the router recorded it
 *
 * The line a client draws under a message whose mentions were recorded
 * and not delivered: "Not delivered to @bob and @carol -- they already
 * answered in this thread of replies."  One function for the three
 * clients, for the reason clawt_message_ack_label() is one.
 *
 * Returns: (transfer full) (nullable): the note, or %NULL when nobody
 *   was skipped
 */
gchar *clawt_round_skip_note(const gchar *skipped, const gchar *reason);

/**
 * clawt_round_skip_tooltip:
 *
 * The longer explanation behind clawt_round_skip_note().
 *
 * Returns: (transfer none): the explanation
 */
const gchar *clawt_round_skip_tooltip(void);

G_END_DECLS

#endif /* CLAWT_ROUND_H */
