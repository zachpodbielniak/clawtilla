/*
 * test-chat-fold.c - Folding long agent-to-agent requests, the exchange
 *                    view, and what an open-room link names
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three clients draw these, so the rules are library functions and the
 * tests are here, against no window and no daemon: a fold decided in
 * one client and not the next is the same transcript reading two ways.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* A body of @lines short lines. */
static gchar *
lines_of(
	guint lines
){
	g_autoptr(GString) text = NULL;
	guint i;

	text = g_string_new(NULL);

	for (i = 0; i < lines; i++)
		g_string_append_printf(text, "%sstep %u: do the thing",
				       i > 0 ? "\n" : "", i + 1);

	return g_string_free(g_steal_pointer(&text), FALSE);
}

/* One `room.history` message object. */
static void
add_message(
	JsonArray   *messages,
	const gchar *id,
	const gchar *sender,
	const gchar *body
){
	JsonObject *message;

	message = json_object_new();
	json_object_set_string_member(message, "id", id);
	json_object_set_string_member(message, "sender", sender);
	json_object_set_string_member(message, "body", body);
	json_array_add_object_element(messages, message);
}

/* The ids an exchange kept, comma-separated, for one-line asserts. */
static gchar *
ids_of(
	JsonArray *array
){
	g_autoptr(GString) out = NULL;
	guint i;

	out = g_string_new(NULL);

	for (i = 0; i < json_array_get_length(array); i++) {
		JsonObject *message = json_array_get_object_element(array, i);

		g_string_append_printf(out, "%s%s", i > 0 ? "," : "",
				       json_object_get_string_member(message,
								     "id"));
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Who is an agent ─────────────────────────────────────────────── */

static void
test_sender_is_agent(void)
{
	g_assert_true(clawt_chat_sender_is_agent("chief"));
	g_assert_true(clawt_chat_sender_is_agent("lead-2"));

	/* the reserved senders, walked from the one list of them */
	g_assert_false(clawt_chat_sender_is_agent("user"));
	g_assert_false(clawt_chat_sender_is_agent(CLAWT_SYSTEM_SENDER));
	g_assert_false(clawt_chat_sender_is_agent("routine"));
	g_assert_false(clawt_chat_sender_is_agent("trigger"));

	/* a derived sender, and nobody at all */
	g_assert_false(clawt_chat_sender_is_agent("routine:nightly"));
	g_assert_false(clawt_chat_sender_is_agent(""));
	g_assert_false(clawt_chat_sender_is_agent(NULL));
}

/* ── Counting lines ──────────────────────────────────────────────── */

static void
test_line_count(void)
{
	g_autofree gchar *wall = NULL;
	g_autofree gchar *wide = NULL;

	g_assert_cmpuint(clawt_chat_fold_line_count(NULL), ==, 0);
	g_assert_cmpuint(clawt_chat_fold_line_count(""), ==, 0);
	g_assert_cmpuint(clawt_chat_fold_line_count("\n\n  \n"), ==, 0);
	g_assert_cmpuint(clawt_chat_fold_line_count("one"), ==, 1);

	/* blank lines at the ends are not read; one inside is */
	g_assert_cmpuint(clawt_chat_fold_line_count("\n\na\n\nb\n\n"), ==, 3);

	/*
	 * A brief written as one paragraph has no newline and fills a
	 * screen, which is the wall this exists to fold.  It counts once
	 * per wrap width, rounded up.
	 */
	wall = g_strnfill(CLAWT_CHAT_FOLD_WRAP * 3 + 1, 'x');
	g_assert_cmpuint(clawt_chat_fold_line_count(wall), ==, 4);

	/*
	 * In characters, not bytes: a hundred two-byte characters are one
	 * line on screen, not two.
	 */
	{
		g_autoptr(GString) text = g_string_new(NULL);
		guint i;

		for (i = 0; i < CLAWT_CHAT_FOLD_WRAP; i++)
			g_string_append(text, "\xc3\xa9");

		wide = g_string_free(g_steal_pointer(&text), FALSE);
	}
	g_assert_cmpuint(clawt_chat_fold_line_count(wide), ==, 1);
}

/* ── The fold rule ───────────────────────────────────────────────── */

static void
test_should_fold(void)
{
	g_autofree gchar *longer = lines_of(CLAWT_CHAT_FOLD_LINES + 1);
	g_autofree gchar *exactly = lines_of(CLAWT_CHAT_FOLD_LINES);

	/* another agent's long request to this one folds */
	g_assert_true(clawt_chat_should_fold("chief", "lead", longer));

	/* a short one stays open, up to and including the limit */
	g_assert_false(clawt_chat_should_fold("chief", "lead", exactly));
	g_assert_false(clawt_chat_should_fold("chief", "lead", "ok?"));

	/* the agent's own answer, however long, is its own: never folded */
	g_assert_false(clawt_chat_should_fold("lead", "lead", longer));

	/* the operator, and the daemon's notices, are never folded */
	g_assert_false(clawt_chat_should_fold("user", "lead", longer));
	g_assert_false(clawt_chat_should_fold(CLAWT_SYSTEM_SENDER, "lead",
					      longer));
	g_assert_false(clawt_chat_should_fold("trigger", "lead", longer));

	/*
	 * No viewer is a group room or the Flow tab, and a viewer that is
	 * the operator is a group room seen as the operator: neither folds,
	 * because every member's message there is addressed to the room.
	 */
	g_assert_false(clawt_chat_should_fold("chief", NULL, longer));
	g_assert_false(clawt_chat_should_fold("chief", "user", longer));
}

/* ── The folded line ─────────────────────────────────────────────── */

static void
test_preview(void)
{
	g_autofree gchar *flat = NULL;
	g_autofree gchar *empty = NULL;
	g_autofree gchar *cut = NULL;
	g_autofree gchar *wall = NULL;
	g_autofree gchar *invalid = NULL;

	/* every run of whitespace, newlines included, is one space */
	flat = clawt_chat_fold_preview("  Please\n\n  build\tthe\xc2\xa0thing.\n");
	g_assert_cmpstr(flat, ==, "Please build the thing.");

	empty = clawt_chat_fold_preview(NULL);
	g_assert_cmpstr(empty, ==, "");

	/* cut in characters, ending in one ellipsis */
	wall = g_strnfill(CLAWT_CHAT_FOLD_PREVIEW * 2, 'y');
	cut = clawt_chat_fold_preview(wall);
	g_assert_true(g_utf8_validate(cut, -1, NULL));
	g_assert_cmpint(g_utf8_strlen(cut, -1), ==, CLAWT_CHAT_FOLD_PREVIEW + 1);
	g_assert_true(g_str_has_suffix(cut, "\xe2\x80\xa6"));

	/* a body that is not UTF-8 previews as nothing, not as garbage */
	invalid = clawt_chat_fold_preview("bad \xff byte");
	g_assert_cmpstr(invalid, ==, "");

	/* and the labels every client spells the same way */
	g_assert_cmpstr(clawt_chat_fold_show_less_label(), ==, "Show less");
	g_assert_cmpstr(clawt_chat_fold_view_exchange_label(), ==,
			"View exchange");
}

static void
test_preview_does_not_split_characters(void)
{
	g_autoptr(GString) text = NULL;
	g_autofree gchar *cut = NULL;
	guint i;

	/* three-byte characters past the limit: a byte cut would split one */
	text = g_string_new(NULL);

	for (i = 0; i < CLAWT_CHAT_FOLD_PREVIEW + 20; i++)
		g_string_append(text, "\xe2\x82\xac");

	cut = clawt_chat_fold_preview(text->str);
	g_assert_true(g_utf8_validate(cut, -1, NULL));
	g_assert_cmpint(g_utf8_strlen(cut, -1), ==, CLAWT_CHAT_FOLD_PREVIEW + 1);
}

/* ── The exchange ────────────────────────────────────────────────── */

static void
test_exchange_filters_to_the_two_agents(void)
{
	g_autoptr(JsonArray) messages = json_array_new();
	g_autoptr(JsonArray) exchange = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *title = NULL;

	/*
	 * lead's own line before chief has asked anything is not an answer
	 * to chief, so it is not in the exchange.
	 */
	add_message(messages, "m1", "lead", "starting up");
	add_message(messages, "m2", "chief", "Please do the long thing.");
	add_message(messages, "m3", CLAWT_SYSTEM_SENDER, "[clawtilla] settled");
	add_message(messages, "m4", "lead", "Done: here is what I found.");
	add_message(messages, "m5", "lead", "");
	add_message(messages, "m6", "user", "and while you are at it?");
	add_message(messages, "m7", "lead", "Sure, answering the operator.");
	add_message(messages, "m8", "chief", "Second request.");
	add_message(messages, "m9", "other", "unrelated");
	add_message(messages, "m10", "lead", "Answering other, not chief.");
	add_message(messages, "m11", "chief", "Third request.");
	add_message(messages, "m12", "lead", "Third answer.");

	exchange = clawt_chat_exchange(messages, "lead", "chief");
	ids = ids_of(exchange);

	/*
	 * Requests from chief and lead's written answers to them, in order;
	 * the daemon's notice neither appears nor ends the answer; an empty
	 * body is skipped; the operator and a third agent end the answer.
	 */
	g_assert_cmpstr(ids, ==, "m2,m4,m8,m11,m12");

	/* references to the history's own objects, not copies */
	g_assert_true(json_array_get_object_element(exchange, 0) ==
		      json_array_get_object_element(messages, 1));

	title = clawt_chat_exchange_title("lead", "chief");
	g_assert_cmpstr(title, ==, "Between chief and lead");
}

static void
test_exchange_degenerate(void)
{
	g_autoptr(JsonArray) messages = json_array_new();
	g_autoptr(JsonArray) none = NULL;
	g_autoptr(JsonArray) self = NULL;
	g_autoptr(JsonArray) empty = NULL;

	add_message(messages, "m1", "chief", "hello");
	json_array_add_null_element(messages);
	json_array_add_string_element(messages, "not an object");

	none = clawt_chat_exchange(NULL, "lead", "chief");
	g_assert_cmpuint(json_array_get_length(none), ==, 0);

	/* an agent's exchange with itself is nothing, not its whole room */
	self = clawt_chat_exchange(messages, "chief", "chief");
	g_assert_cmpuint(json_array_get_length(self), ==, 0);

	/* elements that are not objects are passed over, not fatal */
	empty = clawt_chat_exchange(messages, "lead", "chief");
	g_assert_cmpuint(json_array_get_length(empty), ==, 1);
}

/* ── Open-room links: the spelling ───────────────────────────────── */

static void
test_open_target_normalize(void)
{
	static const struct {
		const gchar *text;
		const gchar *want;
	} good[] = {
		{ "chief", "chief" },
		{ "  chief\n", "chief" },
		{ "clawtilla:chief", "chief" },
		{ "clawtilla://chief", "chief" },
		{ "clawtilla://chief/", "chief" },
		{ "clawtilla:dm:chief:lead", "dm:chief:lead" },
		{ "standup.v2", "standup.v2" },
	};
	static const gchar *bad[] = {
		"", "   ", "clawtilla:", "clawtilla://", "Chief",
		"chief lead", "chief;rm", "../etc", "chief\tx", "caf\xc3\xa9",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(good); i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *got = NULL;

		got = clawt_open_target_normalize(good[i].text, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(got, ==, good[i].want);
	}

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *got = NULL;

		got = clawt_open_target_normalize(bad[i], &error);
		g_assert_null(got);
		g_assert_error(error, CLAWT_ERROR,
			       CLAWT_ERROR_INVALID_ARGUMENT);
	}

	/* NULL, and one character past the limit */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *too_long = NULL;
		g_autofree gchar *got = NULL;

		g_assert_null(clawt_open_target_normalize(NULL, &error));
		g_assert_error(error, CLAWT_ERROR,
			       CLAWT_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);

		too_long = g_strnfill(CLAWT_OPEN_TARGET_MAX + 1, 'a');
		got = clawt_open_target_normalize(too_long, &error);
		g_assert_null(got);
		g_assert_error(error, CLAWT_ERROR,
			       CLAWT_ERROR_INVALID_ARGUMENT);
	}
}

/* ── Open-room links: what exists ────────────────────────────────── */

static JsonArray *
fixture_agents(void)
{
	JsonArray *agents;
	JsonObject *agent;

	agents = json_array_new();

	agent = json_object_new();
	json_object_set_string_member(agent, "id", "chief");
	json_object_set_string_member(agent, "dm_room", "dm:chief:user");
	json_array_add_object_element(agents, agent);

	agent = json_object_new();
	json_object_set_string_member(agent, "id", "lead");
	json_object_set_string_member(agent, "dm_room", "dm:lead:user");
	json_array_add_object_element(agents, agent);

	/* an agent with the same name as a declared room */
	agent = json_object_new();
	json_object_set_string_member(agent, "id", "ops");
	json_object_set_string_member(agent, "dm_room", "dm:ops:user");
	json_array_add_object_element(agents, agent);

	return agents;
}

static void
add_room(
	JsonArray   *rooms,
	const gchar *id,
	const gchar *first,
	const gchar *second
){
	JsonObject *room;
	JsonArray *members;

	room = json_object_new();
	members = json_array_new();
	json_object_set_string_member(room, "id", id);

	if (first != NULL)
		json_array_add_string_element(members, first);
	if (second != NULL)
		json_array_add_string_element(members, second);

	json_object_set_array_member(room, "members", members);
	json_array_add_object_element(rooms, room);
}

static JsonArray *
fixture_rooms(void)
{
	JsonArray *rooms;

	rooms = json_array_new();
	add_room(rooms, "standup", "chief", "lead");
	add_room(rooms, "ops", "chief", "ops");
	add_room(rooms, "dm:chief:lead", "chief", "lead");
	add_room(rooms, "dm:lead:user", "lead", "user");
	add_room(rooms, "routine:nightly", "chief", NULL);

	return rooms;
}

static void
test_open_target_resolve(void)
{
	g_autoptr(JsonArray) agents = fixture_agents();
	g_autoptr(JsonArray) rooms = fixture_rooms();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *agent = NULL;
	g_autofree gchar *peer = NULL;
	g_autofree gchar *room = NULL;

	/* an agent by id: its own chat */
	g_assert_true(clawt_open_target_resolve("chief", agents, rooms, &agent,
						&peer, &room, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(agent, ==, "chief");
	g_assert_null(peer);
	g_assert_null(room);
	g_clear_pointer(&agent, g_free);

	/* an agent by its own conversation's room */
	g_assert_true(clawt_open_target_resolve("dm:lead:user", agents, rooms,
						&agent, &peer, &room, &error));
	g_assert_cmpstr(agent, ==, "lead");
	g_assert_null(peer);
	g_clear_pointer(&agent, g_free);

	/* a declared group room: the room */
	g_assert_true(clawt_open_target_resolve("standup", agents, rooms,
						&agent, &peer, &room, &error));
	g_assert_null(agent);
	g_assert_cmpstr(room, ==, "standup");
	g_clear_pointer(&room, g_free);

	/* a direct room between two agents: the first's conversation */
	g_assert_true(clawt_open_target_resolve("dm:chief:lead", agents, rooms,
						&agent, &peer, &room, &error));
	g_assert_cmpstr(agent, ==, "chief");
	g_assert_cmpstr(peer, ==, "lead");
	g_assert_null(room);
	g_clear_pointer(&agent, g_free);
	g_clear_pointer(&peer, g_free);

	/*
	 * A declared room and an agent with one name: the room, so a room's
	 * link keeps working when somebody later names an agent the same;
	 * the agent is still reachable by its own conversation's room.
	 */
	g_assert_true(clawt_open_target_resolve("ops", agents, rooms, &agent,
						&peer, &room, &error));
	g_assert_null(agent);
	g_assert_cmpstr(room, ==, "ops");
	g_clear_pointer(&room, g_free);

	g_assert_true(clawt_open_target_resolve("dm:ops:user", agents, rooms,
						&agent, &peer, &room, &error));
	g_assert_cmpstr(agent, ==, "ops");
	g_assert_null(room);
	g_clear_pointer(&agent, g_free);
}

/*
 * A second `clawtilla-gtk --open` hands its whole command line to the
 * window already running, which takes only --open out of it: every
 * spelling the typing invocation accepted, the last one winning, other
 * options ignored, and nothing when there is no --open (which opens a
 * window, as a second invocation always has).
 */
static void
test_open_target_from_another_invocation(void)
{
	static const gchar *const plain[] = {
		"clawtilla-gtk", "--open", "chief", NULL
	};
	static const gchar *const shortopt[] = {
		"clawtilla-gtk", "-o", "standup", NULL
	};
	static const gchar *const equals[] = {
		"clawtilla-gtk", "--open=clawtilla:dm:chief:lead", NULL
	};
	static const gchar *const mixed[] = {
		"clawtilla-gtk", "--host", "box:7777", "--open", "first",
		"--profile", "work", "--open", "second", NULL
	};
	static const gchar *const none[] = {
		"clawtilla-gtk", "--host", "box:7777", NULL
	};
	static const gchar *const empty[] = { NULL };
	g_autofree gchar *a = clawt_open_target_from_argv(plain);
	g_autofree gchar *b = clawt_open_target_from_argv(shortopt);
	g_autofree gchar *c = clawt_open_target_from_argv(equals);
	g_autofree gchar *d = clawt_open_target_from_argv(mixed);
	g_autofree gchar *e = clawt_open_target_from_argv(none);
	g_autofree gchar *f = clawt_open_target_from_argv(empty);

	g_assert_cmpstr(a, ==, "chief");
	g_assert_cmpstr(b, ==, "standup");
	g_assert_cmpstr(c, ==, "clawtilla:dm:chief:lead");
	g_assert_cmpstr(d, ==, "second");
	g_assert_null(e);
	g_assert_null(f);
	g_assert_null(clawt_open_target_from_argv(NULL));

	/* The caller's vector is not rearranged by the parse. */
	g_assert_cmpstr(mixed[1], ==, "--host");
	g_assert_cmpstr(mixed[4], ==, "first");
}

/*
 * A link followed before the daemon answers is held, and a second one
 * replaces it -- clicking twice means the second.  Once connected, a
 * link opens at once and leaves what was waiting for the connection's
 * own open.
 */
static void
test_open_target_waits_for_the_connection(void)
{
	g_autofree gchar *pending = NULL;
	g_autofree gchar *now = NULL;

	g_assert_null(clawt_open_target_hold(&pending, g_strdup("chief"),
	                                     FALSE));
	g_assert_cmpstr(pending, ==, "chief");

	g_assert_null(clawt_open_target_hold(&pending, g_strdup("standup"),
	                                     FALSE));
	g_assert_cmpstr(pending, ==, "standup");

	now = clawt_open_target_hold(&pending, g_strdup("lead"), TRUE);
	g_assert_cmpstr(now, ==, "lead");
	g_assert_cmpstr(pending, ==, "standup");
}

static void
test_open_target_unknown(void)
{
	g_autoptr(JsonArray) agents = fixture_agents();
	g_autoptr(JsonArray) rooms = fixture_rooms();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *agent = NULL;
	g_autofree gchar *room = NULL;

	/* nothing by that name: a sentence that names it, nothing set */
	g_assert_false(clawt_open_target_resolve("ghost", agents, rooms, &agent,
						 NULL, &room, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_assert_nonnull(strstr(error->message, "\"ghost\""));
	g_assert_null(agent);
	g_assert_null(room);
	g_clear_error(&error);

	/* a routine's room exists but has nowhere to open */
	g_assert_false(clawt_open_target_resolve("routine:nightly", agents,
						 rooms, &agent, NULL, &room,
						 &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* no listings yet (a daemon not answering) is not a crash */
	g_assert_false(clawt_open_target_resolve("chief", NULL, NULL, &agent,
						 NULL, &room, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	g_assert_false(clawt_open_target_resolve(NULL, agents, rooms, NULL,
						 NULL, NULL, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/chat-fold/sender-is-agent", test_sender_is_agent);
	g_test_add_func("/chat-fold/line-count", test_line_count);
	g_test_add_func("/chat-fold/should-fold", test_should_fold);
	g_test_add_func("/chat-fold/preview", test_preview);
	g_test_add_func("/chat-fold/preview-utf8",
			test_preview_does_not_split_characters);
	g_test_add_func("/chat-fold/exchange",
			test_exchange_filters_to_the_two_agents);
	g_test_add_func("/chat-fold/exchange-degenerate",
			test_exchange_degenerate);
	g_test_add_func("/open-target/normalize", test_open_target_normalize);
	g_test_add_func("/open-target/resolve", test_open_target_resolve);
	g_test_add_func("/open-target/unknown", test_open_target_unknown);
	g_test_add_func("/open-target/from-another-invocation",
	                test_open_target_from_another_invocation);
	g_test_add_func("/open-target/waits-for-the-connection",
	                test_open_target_waits_for_the_connection);

	return g_test_run();
}
