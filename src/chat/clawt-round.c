/*
 * clawt-round.c - Rounds and threads of replies in a group room
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The arithmetic of a chain, kept apart from the router so it can be
 * tested without one, and the words every client draws for it, kept in
 * one place so three clients say the same thing.
 */

#include "clawtilla.h"
#include "chat/clawt-round.h"

#include <string.h>

guint
clawt_round_chain_length(
	const gchar *chain
){
	g_auto(GStrv) parts = NULL;
	guint count = 0;
	guint i;

	if (chain == NULL || *chain == '\0')
		return 0;

	parts = g_strsplit(chain, ",", -1);

	/* Empty entries are tolerated rather than counted: ",bob," is bob. */
	for (i = 0; parts[i] != NULL; i++) {
		if (*parts[i] != '\0')
			count++;
	}

	return count;
}

gboolean
clawt_round_chain_has(
	const gchar *chain,
	const gchar *member
){
	g_auto(GStrv) parts = NULL;
	guint i;

	if (chain == NULL || *chain == '\0' || member == NULL || *member == '\0')
		return FALSE;

	/*
	 * Split and compared whole, not strstr(): "bob" must not be found
	 * in "bobby", which is the mention matcher's own rule about word
	 * boundaries applied to the list it feeds.
	 */
	parts = g_strsplit(chain, ",", -1);

	for (i = 0; parts[i] != NULL; i++) {
		if (g_strcmp0(parts[i], member) == 0)
			return TRUE;
	}

	return FALSE;
}

gchar *
clawt_round_chain_extend(
	const gchar *chain,
	const gchar *member
){
	g_return_val_if_fail(member != NULL, g_strdup(chain));

	if (chain == NULL || *chain == '\0')
		return g_strdup(member);

	if (clawt_round_chain_has(chain, member))
		return g_strdup(chain);

	return g_strconcat(chain, ",", member, NULL);
}

gchar *
clawt_round_chain_union(
	const gchar *first,
	const gchar *second
){
	g_autofree gchar *out = NULL;
	g_auto(GStrv) parts = NULL;
	guint i;

	if (first != NULL && *first != '\0')
		out = g_strdup(first);

	if (second == NULL || *second == '\0')
		return g_steal_pointer(&out);

	parts = g_strsplit(second, ",", -1);

	for (i = 0; parts[i] != NULL; i++) {
		gchar *grown;

		if (*parts[i] == '\0')
			continue;

		grown = clawt_round_chain_extend(out, parts[i]);
		g_free(out);
		out = grown;
	}

	return g_steal_pointer(&out);
}

gchar *
clawt_round_label(
	guint round
){
	return g_strdup_printf("Round %u", round);
}

gchar *
clawt_round_skip_note(
	const gchar *skipped,
	const gchar *reason
){
	g_auto(GStrv) parts = NULL;
	g_autoptr(GString) out = NULL;
	guint total;
	guint seen = 0;
	guint i;

	total = clawt_round_chain_length(skipped);

	if (total == 0)
		return NULL;

	/*
	 * "@a", "@a and @b", "@a, @b and @c" -- a list read aloud, because
	 * this line sits under a message somebody is reading, not in a log.
	 */
	parts = g_strsplit(skipped, ",", -1);
	out = g_string_new("Not delivered to ");

	for (i = 0; parts[i] != NULL; i++) {
		if (*parts[i] == '\0')
			continue;

		if (seen > 0)
			g_string_append(out, (seen + 1 == total) ? " and " : ", ");

		g_string_append_printf(out, "@%s", parts[i]);
		seen++;
	}

	if (reason != NULL && *reason != '\0')
		g_string_append_printf(out, " \xe2\x80\x94 %s", reason);

	return g_string_free(g_steal_pointer(&out), FALSE);
}

const gchar *
clawt_round_skip_tooltip(void)
{
	return "A round of this room starts at each message from a person, a "
	       "routine or a trigger. Within one thread of replies a member "
	       "answers once, and a thread stops after rooms.max_chain "
	       "replies; a mention past either is kept here and not "
	       "delivered, so two agents naming each other cannot run the "
	       "room to its hop limit.";
}
