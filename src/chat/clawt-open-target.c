/*
 * clawt-open-target.c - What a `clawtilla:` link or `--open` names
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A link is written once -- in an org file, a bookmark, a script -- and
 * followed long after.  So the agent or room it names may have been
 * removed, and the client must say so in a sentence rather than open
 * an empty pane or fall over.  Both halves are here, testable without a
 * window: the spelling, and the lookup against the daemon's listings.
 */

#include "chat/clawt-open-target.h"
#include "chat/clawt-chat-run.h"
#include "chat/clawt-room.h"
#include "clawt-error.h"

#include <string.h>

/* ── The spelling ────────────────────────────────────────────────── */

static gboolean
target_char_ok(
	gchar c
){
	return g_ascii_islower(c) || g_ascii_isdigit(c) ||
	       c == '-' || c == '_' || c == '.' || c == ':';
}

gchar *
clawt_open_target_normalize(
	const gchar  *text,
	GError      **error
){
	g_autofree gchar *work = NULL;
	const gchar *start;
	const gchar *p;
	gsize length;

	if (text == NULL) {
		g_set_error_literal(error, CLAWT_ERROR,
				    CLAWT_ERROR_INVALID_ARGUMENT,
				    "Name an agent or a room to open");
		return NULL;
	}

	work = g_strstrip(g_strdup(text));
	start = work;

	/*
	 * The link's own scheme, in either of the two ways it gets written.
	 * org-mode hands a `clawtilla:chief` link's path over without the
	 * scheme, a shell user pastes it with, and a browser-style
	 * `clawtilla://chief/` has the slashes too; all three name `chief`.
	 */
	if (g_str_has_prefix(start, "clawtilla:"))
		start += strlen("clawtilla:");

	while (*start == '/')
		start++;

	length = strlen(start);

	while (length > 0 && start[length - 1] == '/')
		length--;

	if (length == 0) {
		g_set_error_literal(error, CLAWT_ERROR,
				    CLAWT_ERROR_INVALID_ARGUMENT,
				    "Name an agent or a room to open");
		return NULL;
	}

	if (length > CLAWT_OPEN_TARGET_MAX) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			    "That is %" G_GSIZE_FORMAT " characters long; an "
			    "agent or room id is at most %d",
			    length, CLAWT_OPEN_TARGET_MAX);
		return NULL;
	}

	/*
	 * The characters ids are made of, and nothing else.  Checked here
	 * rather than left to the lookup because the target is shown back
	 * in the refusal, and a control character in that sentence is a
	 * worse problem than the missing room.
	 */
	for (p = start; p < start + length; p++) {
		if (!target_char_ok(*p)) {
			g_set_error_literal(error, CLAWT_ERROR,
					    CLAWT_ERROR_INVALID_ARGUMENT,
					    "An agent or room id is lower-case "
					    "letters, digits, '-', '_', '.' and "
					    "':' only");
			return NULL;
		}
	}

	return g_strndup(start, length);
}

/* ── The lookup ──────────────────────────────────────────────────── */

static JsonObject *
array_object(
	JsonArray *array,
	guint      index
){
	JsonNode *node;

	node = json_array_get_element(array, index);

	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	return json_node_get_object(node);
}

static const gchar *
object_string(
	JsonObject  *object,
	const gchar *member
){
	if (object == NULL)
		return NULL;

	return json_object_get_string_member_with_default(object, member,
							  NULL);
}

/*
 * A room's members as a NULL-terminated vector of borrowed strings, for
 * clawt_chat_conversation_peer() -- the one rule for which rooms are a
 * direct conversation.
 */
static GPtrArray *
room_members(
	JsonObject *room
){
	g_autoptr(GPtrArray) members = NULL;
	JsonArray *array;
	guint i;

	members = g_ptr_array_new();

	array = (room != NULL && json_object_has_member(room, "members") &&
		 JSON_NODE_HOLDS_ARRAY(json_object_get_member(room, "members")))
		? json_object_get_array_member(room, "members") : NULL;

	for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
		JsonNode *node = json_array_get_element(array, i);

		if (node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
		    json_node_get_value_type(node) == G_TYPE_STRING)
			g_ptr_array_add(members,
					(gpointer)json_node_get_string(node));
	}

	g_ptr_array_add(members, NULL);

	return (GPtrArray *)g_steal_pointer(&members);
}

gboolean
clawt_open_target_resolve(
	const gchar  *target,
	JsonArray    *agents,
	JsonArray    *rooms,
	gchar       **out_agent,
	gchar       **out_peer,
	gchar       **out_room,
	GError      **error
){
	guint i;

	if (out_agent != NULL)
		*out_agent = NULL;
	if (out_peer != NULL)
		*out_peer = NULL;
	if (out_room != NULL)
		*out_room = NULL;

	if (target == NULL || *target == '\0') {
		g_set_error_literal(error, CLAWT_ERROR,
				    CLAWT_ERROR_INVALID_ARGUMENT,
				    "Name an agent or a room to open");
		return FALSE;
	}

	/*
	 * A room with a row of its own first, so `/r/<room>` and a link to
	 * a declared room keep opening the room even where an agent of the
	 * same name exists; the agent is one more character away
	 * (its `dm_room`).
	 */
	for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
		JsonObject *room = array_object(rooms, i);

		if (g_strcmp0(object_string(room, "id"), target) == 0 &&
		    clawt_room_is_listed(target)) {
			if (out_room != NULL)
				*out_room = g_strdup(target);
			return TRUE;
		}
	}

	/* an agent, by its id or by its own conversation's room */
	for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
		JsonObject *agent = array_object(agents, i);
		const gchar *id = object_string(agent, "id");

		if (id == NULL)
			continue;

		if (g_strcmp0(id, target) == 0 ||
		    g_strcmp0(object_string(agent, "dm_room"), target) == 0) {
			if (out_agent != NULL)
				*out_agent = g_strdup(id);
			return TRUE;
		}
	}

	for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
		JsonObject *room = array_object(rooms, i);
		g_autoptr(GPtrArray) members = NULL;
		const gchar *first;
		const gchar *other;

		if (g_strcmp0(object_string(room, "id"), target) != 0)
			continue;

		/*
		 * A direct room between two agents is one agent's
		 * conversation with the other; which agent's page it opens
		 * on is the first member's, which is how the daemon lists
		 * the pair.  A direct room with the operator in it is that
		 * agent's own chat.
		 */
		members = room_members(room);
		first = (members->len == 3) ? g_ptr_array_index(members, 0)
					    : NULL;
		other = (first != NULL)
			? clawt_chat_conversation_peer(
				(const gchar * const *)members->pdata, first)
			: NULL;

		if (first != NULL && other != NULL) {
			if (g_strcmp0(first, "user") == 0) {
				if (out_agent != NULL)
					*out_agent = g_strdup(other);
			} else if (g_strcmp0(other, "user") == 0) {
				if (out_agent != NULL)
					*out_agent = g_strdup(first);
			} else {
				if (out_agent != NULL)
					*out_agent = g_strdup(first);
				if (out_peer != NULL)
					*out_peer = g_strdup(other);
			}

			return TRUE;
		}

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			    "\"%s\" is a room no client opens directly; it "
			    "belongs to a routine or a trigger", target);
		return FALSE;
	}

	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		    "There is no agent or room called \"%s\" on this daemon",
		    target);
	return FALSE;
}

/* ── Another invocation's command line ───────────────────────────── */

gchar *
clawt_open_target_from_argv(
	const gchar * const *argv
){
	gchar *target = NULL;
	GOptionEntry entries[] = {
		{ "open", 'o', 0, G_OPTION_ARG_STRING, NULL, NULL, NULL },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
	};
	g_autoptr(GOptionContext) context = NULL;
	g_auto(GStrv) copy = NULL;

	if (argv == NULL || argv[0] == NULL)
		return NULL;

	/*
	 * Parsed with GOption rather than by hand, so every spelling the
	 * typing invocation accepted (`-o x`, `--open=x`) is read the same
	 * way here.  On a copy, through the _strv form: plain parse drops
	 * the strings it consumes from the vector without freeing them.
	 */
	copy = g_strdupv((gchar **)argv);
	entries[0].arg_data = &target;

	context = g_option_context_new(NULL);
	g_option_context_set_help_enabled(context, FALSE);
	g_option_context_set_ignore_unknown_options(context, TRUE);
	g_option_context_add_main_entries(context, entries, NULL);

	if (!g_option_context_parse_strv(context, &copy, NULL))
		g_clear_pointer(&target, g_free);

	return target;
}

gchar *
clawt_open_target_hold(
	gchar    **pending,
	gchar     *target,
	gboolean   connected
){
	g_return_val_if_fail(pending != NULL, target);

	if (connected)
		return target;

	g_free(*pending);
	*pending = target;

	return NULL;
}
