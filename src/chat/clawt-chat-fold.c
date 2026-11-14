/*
 * clawt-chat-fold.c - Folding long agent-to-agent requests, and the
 *                     exchange between two agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Both rules live here rather than in each client because three clients
 * draw them: two copies of "is this long enough to fold" differ exactly
 * once, on the message nobody looked at, and a fold decided in one
 * client and not another is the same transcript reading differently
 * depending on which window it was opened in.
 */

#include "chat/clawt-chat-fold.h"
#include "chat/clawt-message.h"
#include "clawt-util.h"

#include <string.h>

/* ── Who is an agent ─────────────────────────────────────────────── */

/*
 * clawt_chat_sender_is_agent:
 *
 * The reserved senders are walked from clawt_agent_id_is_reserved(),
 * which is the one list of them; a colon is how a derived sender is
 * spelled, and no agent id may contain one (clawt_is_valid_id()).
 */
gboolean
clawt_chat_sender_is_agent(
	const gchar *sender
){
	if (sender == NULL || *sender == '\0')
		return FALSE;

	if (clawt_agent_id_is_reserved(sender))
		return FALSE;

	return strchr(sender, ':') == NULL;
}

/* ── How long a message reads ────────────────────────────────────── */

/*
 * clawt_chat_fold_line_count:
 *
 * Counted in characters rather than bytes, for the same reason the
 * preview is: a brief in a script with multi-byte characters would
 * otherwise count two or three times as long as it reads.
 */
guint
clawt_chat_fold_line_count(
	const gchar *body
){
	g_autofree gchar *copy = NULL;
	g_auto(GStrv) lines = NULL;
	guint count;
	guint i;

	if (body == NULL)
		return 0;

	/* blank lines at either end are not something a reader reads */
	copy = g_strdup(body);
	g_strstrip(copy);

	if (*copy == '\0')
		return 0;

	lines = g_strsplit(copy, "\n", -1);
	count = 0;

	for (i = 0; lines[i] != NULL; i++) {
		glong chars;

		chars = g_utf8_validate(lines[i], -1, NULL)
			? g_utf8_strlen(lines[i], -1)
			: (glong)strlen(lines[i]);

		/*
		 * An empty line is still a line: a paragraph break costs
		 * a reader a line of the screen, which is what this counts.
		 */
		if (chars <= CLAWT_CHAT_FOLD_WRAP)
			count++;
		else
			count += (guint)((chars + CLAWT_CHAT_FOLD_WRAP - 1) /
					 CLAWT_CHAT_FOLD_WRAP);
	}

	return count;
}

/*
 * clawt_chat_should_fold:
 *
 * The viewer has to be an agent too.  A group room passes none, and
 * the operator's own chat passes the agent -- whose messages are its
 * own and never fold -- so the only messages that can fold are another
 * agent's, in a conversation between two agents.
 */
gboolean
clawt_chat_should_fold(
	const gchar *sender,
	const gchar *viewer,
	const gchar *body
){
	if (!clawt_chat_sender_is_agent(sender))
		return FALSE;

	if (!clawt_chat_sender_is_agent(viewer))
		return FALSE;

	if (g_strcmp0(sender, viewer) == 0)
		return FALSE;

	return clawt_chat_fold_line_count(body) > CLAWT_CHAT_FOLD_LINES;
}

/* ── The folded line ─────────────────────────────────────────────── */

/*
 * clawt_chat_fold_preview:
 *
 * Collapsed with GLib's Unicode space test, so a non-breaking space or
 * an ideographic space does not survive into the line as a gap.
 */
gchar *
clawt_chat_fold_preview(
	const gchar *body
){
	g_autoptr(GString) flat = NULL;
	const gchar *p;
	gboolean in_space;
	glong length;

	flat = g_string_new(NULL);

	if (body == NULL || !g_utf8_validate(body, -1, NULL))
		return g_string_free(g_steal_pointer(&flat), FALSE);

	/* runs of whitespace become one space; leading space is dropped */
	in_space = TRUE;

	for (p = body; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_isspace(c)) {
			if (!in_space)
				g_string_append_c(flat, ' ');
			in_space = TRUE;
			continue;
		}

		g_string_append_unichar(flat, c);
		in_space = FALSE;
	}

	/* and the space a trailing run left behind */
	if (flat->len > 0 && flat->str[flat->len - 1] == ' ')
		g_string_truncate(flat, flat->len - 1);

	length = g_utf8_strlen(flat->str, -1);

	if (length > CLAWT_CHAT_FOLD_PREVIEW) {
		const gchar *cut;

		/*
		 * Cut on a character boundary, then step back over the
		 * space the cut may have landed after, so the ellipsis
		 * sits against a word rather than floating after a gap.
		 */
		cut = g_utf8_offset_to_pointer(flat->str,
					       CLAWT_CHAT_FOLD_PREVIEW);
		g_string_truncate(flat, (gsize)(cut - flat->str));

		while (flat->len > 0 && flat->str[flat->len - 1] == ' ')
			g_string_truncate(flat, flat->len - 1);

		g_string_append(flat, "\xe2\x80\xa6");
	}

	return g_string_free(g_steal_pointer(&flat), FALSE);
}

const gchar *
clawt_chat_fold_show_less_label(void)
{
	return "Show less";
}

const gchar *
clawt_chat_fold_view_exchange_label(void)
{
	return "View exchange";
}

gchar *
clawt_chat_exchange_title(
	const gchar *agent,
	const gchar *peer
){
	return g_strdup_printf("Between %s and %s",
			       peer != NULL ? peer : "?",
			       agent != NULL ? agent : "?");
}

/* ── The exchange ────────────────────────────────────────────────── */

/*
 * clawt_chat_exchange:
 *
 * One pass, with one piece of state: whether @agent_id's next written
 * line is an answer to @peer_id.  A request from @peer_id sets it; any
 * other speaker but the daemon clears it.  That is the whole of "this
 * agent's answers to that peer", and it reads the same over a direct
 * room (where it keeps nearly everything) as over a group room (where it
 * keeps one thread out of many).
 */
JsonArray *
clawt_chat_exchange(
	JsonArray   *messages,
	const gchar *agent_id,
	const gchar *peer_id
){
	JsonArray *out;
	gboolean answering;
	guint length;
	guint i;

	out = json_array_new();

	if (messages == NULL || agent_id == NULL || peer_id == NULL ||
	    g_strcmp0(agent_id, peer_id) == 0)
		return out;

	answering = FALSE;
	length = json_array_get_length(messages);

	for (i = 0; i < length; i++) {
		JsonNode *node;
		JsonObject *message;
		const gchar *sender;
		const gchar *body;

		node = json_array_get_element(messages, i);

		/* a node of object type can still hold no object */
		if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node) ||
		    json_node_get_object(node) == NULL)
			continue;

		message = json_node_get_object(node);
		sender = json_object_get_string_member_with_default(
			message, "sender", NULL);
		body = json_object_get_string_member_with_default(
			message, "body", NULL);

		if (sender == NULL)
			continue;

		/*
		 * The daemon's notices -- a settle, a merge note -- are not
		 * either agent speaking, and they arrive between a request
		 * and its answer often enough that ending the answer at one
		 * would drop the answer.
		 */
		if (g_strcmp0(sender, CLAWT_SYSTEM_SENDER) == 0)
			continue;

		if (g_strcmp0(sender, peer_id) == 0) {
			answering = TRUE;
		} else if (g_strcmp0(sender, agent_id) != 0) {
			answering = FALSE;
			continue;
		} else if (!answering) {
			continue;
		}

		if (body == NULL || *body == '\0')
			continue;

		json_array_add_object_element(out, json_object_ref(message));
	}

	return out;
}
