/*
 * clawt-chat-fold.h - Folding long agent-to-agent requests, and the
 *                     exchange between two agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_CHAT_FOLD_H
#define CLAWT_CHAT_FOLD_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_CHAT_FOLD_LINES:
 *
 * How many lines a peer's message may run to and still be shown open.
 *
 * Six: a short request ("can you check the build on lead's machine?")
 * and a two-paragraph question both fit, and a chief's delegation brief
 * -- a page of numbered steps -- does not.  Lines are counted as a
 * reader sees them, see clawt_chat_fold_line_count().
 */
#define CLAWT_CHAT_FOLD_LINES (6)

/**
 * CLAWT_CHAT_FOLD_WRAP:
 *
 * The width, in characters, a source line is taken to wrap at when it
 * is counted.  A brief written as one paragraph has one newline and
 * fills a screen, so counting newlines alone would leave the longest
 * walls open.  One hundred is a little wider than the chat's measure,
 * so the count errs towards leaving a message open.
 */
#define CLAWT_CHAT_FOLD_WRAP (100)

/**
 * CLAWT_CHAT_FOLD_PREVIEW:
 *
 * How many characters of a folded message its one line shows before
 * the ellipsis.
 */
#define CLAWT_CHAT_FOLD_PREVIEW (160)

/**
 * clawt_chat_sender_is_agent:
 * @sender: (nullable): a message's sender id
 *
 * Whether @sender is an agent, as against the operator ("user"), the
 * daemon's own notices ("clawtilla"), automated work ("routine",
 * "trigger") or a derived sender spelled with a colon.  The reserved
 * names are clawt_agent_id_is_reserved()'s, so the next reserved sender
 * is added in one place.
 *
 * Returns: %TRUE when @sender names an agent
 */
gboolean
clawt_chat_sender_is_agent(
	const gchar *sender
);

/**
 * clawt_chat_fold_line_count:
 * @body: (nullable): a message body
 *
 * How many lines @body takes to read: each source line counts once,
 * and a line longer than %CLAWT_CHAT_FOLD_WRAP characters counts once
 * per %CLAWT_CHAT_FOLD_WRAP characters, rounded up.  Leading and
 * trailing blank lines are not counted.
 *
 * Returns: the count, 0 for an empty or %NULL body
 */
guint
clawt_chat_fold_line_count(
	const gchar *body
);

/**
 * clawt_chat_should_fold:
 * @sender: (nullable): who sent the message
 * @viewer: (nullable): the agent whose conversation is on screen, or
 *   %NULL where there is none (a group room, the Flow tab)
 * @body: (nullable): the message
 *
 * Whether a client draws this message folded to one line, with a click
 * to open it.
 *
 * A message folds when it is another agent's message to @viewer and it
 * is longer than %CLAWT_CHAT_FOLD_LINES lines.  The operator's own
 * lines, @viewer's own answers, the daemon's notices and anything short
 * stay open: the point is that a page-long brief from a chief does not
 * bury the lines around it, not that agent traffic is hidden.
 *
 * A group room passes no @viewer and never folds.  Every member's
 * message there is addressed to the room, and an agent answering the
 * person reads exactly like one agent asking another.
 *
 * Search results never ask: a hit is shown because somebody searched
 * for its text, and folding it would hide the reason it is there.
 *
 * Returns: %TRUE when the message is drawn folded
 */
gboolean
clawt_chat_should_fold(
	const gchar *sender,
	const gchar *viewer,
	const gchar *body
);

/**
 * clawt_chat_fold_preview:
 * @body: (nullable): a message body
 *
 * The one line a folded message shows: every run of whitespace, line
 * breaks included, collapsed to one space, cut at
 * %CLAWT_CHAT_FOLD_PREVIEW characters (not bytes) with an ellipsis.
 * Plain text, never markup: a client escapes it like any other text.
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_chat_fold_preview(
	const gchar *body
);

/**
 * clawt_chat_fold_show_less_label:
 *
 * The control under an opened message that folds it again.  One
 * spelling for every client.
 *
 * Returns: (transfer none): "Show less"
 */
const gchar *
clawt_chat_fold_show_less_label(void);

/**
 * clawt_chat_fold_view_exchange_label:
 *
 * The control under an opened message that shows the whole exchange
 * between the two agents.
 *
 * Returns: (transfer none): "View exchange"
 */
const gchar *
clawt_chat_fold_view_exchange_label(void);

/**
 * clawt_chat_exchange_title:
 * @agent: the agent whose conversation is on screen
 * @peer: the agent that sent the folded request
 *
 * The heading of the exchange view.
 *
 * Returns: (transfer full): "Between <peer> and <agent>"
 */
gchar *
clawt_chat_exchange_title(
	const gchar *agent,
	const gchar *peer
);

/**
 * clawt_chat_exchange:
 * @messages: (nullable): a room's history, as `room.history` sends it --
 *   objects with at least `sender` and `body`, oldest first
 * @agent_id: the agent whose conversation is on screen
 * @peer_id: the agent whose requests are being followed
 *
 * What the exchange view lists: every message @peer_id sent, and the
 * written answers @agent_id gave after each of them, in order.
 *
 * Filtered to the two agents.  The daemon's own notices are skipped and
 * do not end an answer; a message from anybody else -- the operator, a
 * third agent, a routine -- does, so @agent_id's next lines are an
 * answer to *them* and are left out until @peer_id speaks again.  An
 * empty body is skipped.  Tool chatter is never in it: turn steps are
 * fetched separately from a room's history, and nothing here asks for
 * them.
 *
 * Returns: (transfer full): a new array holding references to the
 *   chosen message objects
 */
JsonArray *
clawt_chat_exchange(
	JsonArray   *messages,
	const gchar *agent_id,
	const gchar *peer_id
);

G_END_DECLS

#endif /* CLAWT_CHAT_FOLD_H */
