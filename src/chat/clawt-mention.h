/*
 * clawt-mention.h - Who a message names
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * In a room that requires mentions, this is the whole of who receives a
 * message -- so it is the whole of what a turn costs, and a false
 * positive is a model call nobody asked for while a false negative is a
 * message that reaches nobody and says nothing about it.
 *
 * It lives here rather than in #ClawtRoom because both clients need the
 * same answer: one to offer a completion and highlight what will
 * actually match, the other to warn before a message is sent to nobody.
 * A rule two clients apply belongs where it can be tested without a
 * window or a browser.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_MENTION_ALL:
 *
 * The name that addresses every member of a room.
 *
 * Recognised only in its `@` form: a bare "all" is an ordinary English
 * word and "that's all for now" is not an instruction to wake five
 * agents.
 */
#define CLAWT_MENTION_ALL "all"

/**
 * clawt_mention_names:
 * @body: (nullable): the message text
 * @id: the agent id to look for
 * @display_name: (nullable): what that agent is called, when it differs
 *
 * Whether @body addresses @id, as `@name` or as a bare word.
 *
 * Both forms require a word boundary on both sides, and the `@` itself
 * must be at one -- without that last rule `zach@bob.com` addresses an
 * agent called `bob`, so every address in a pasted log wakes somebody.
 *
 * Matching is case-insensitive.  Ids are lowercase by construction, and
 * a model writing "Bob, can you check" -- the ordinary way anyone writes
 * a name -- would otherwise reach nobody and be told nothing.
 *
 * @display_name is only consulted when it is a single word: a name like
 * "Oryx" is an address, and "Oryx the Researcher" is a sentence
 * fragment that would match far more than it should.
 *
 * There is deliberately no syntax awareness -- a name inside a code
 * fence or a URL still matches.  The alternative is a markdown parser
 * on the routing path, and an extra turn is cheaper than a mention that
 * does not fire for a reason nobody can see.
 *
 * Returns: %TRUE if @body names @id
 */
gboolean clawt_mention_names(const gchar *body,
                             const gchar *id,
                             const gchar *display_name);

/**
 * clawt_mention_is_broadcast:
 * @body: (nullable): the message text
 *
 * Whether @body says `@all`.
 *
 * Says nothing about whether the sender is *allowed* to broadcast --
 * that is the room's business, and asking it here would put a
 * permission in a string matcher.
 *
 * Returns: %TRUE if @body addresses everybody
 */
gboolean clawt_mention_is_broadcast(const gchar *body);

/**
 * clawt_mention_list:
 * @body: (nullable): the message text
 * @candidates: (element-type utf8): the ids that could be named
 *
 * Which of @candidates @body names, in the order @candidates gives.
 *
 * A separate entry point rather than a loop over clawt_mention_names(),
 * because a client wants the list and the router wants the predicate,
 * and the caller that has a list has no reason to know how one member
 * is decided.
 *
 * Returns: (transfer full) (element-type utf8): the ids named, possibly
 *   empty
 */
GPtrArray *clawt_mention_list(const gchar *body, GPtrArray *candidates);

G_END_DECLS
