/*
 * test-chat-room.c - Counting what the operator has not read, and where
 * the "New messages" rule goes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon derives every unread count from a room's history and a read
 * mark (clawt_room_count_unread()), and every client places the "New
 * messages" rule from the same mark (clawt_unread_rule_index()).  Both
 * are pure over their inputs, so every branch is exercised here without a
 * daemon -- a room with no transcript writes nothing to disk.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

/*
 * Puts a message in @room with a fixed id and second, so the tests below
 * say exactly which message a mark names and which second it is in.
 */
static void
put(ClawtRoom *room, const gchar *id, const gchar *sender, gint64 ts)
{
    g_autoptr(ClawtMessage) message = clawt_message_new(
        clawt_room_get_id(room), sender, "body");

    clawt_message_set_id(message, id);
    clawt_message_set_timestamp(message, ts);
    clawt_room_restore(room, message);
}

/*
 * The everyday case: a mark on a message, two arrivals after it in the
 * same second, one of them the operator's own.  Only the agent's counts.
 * Regressing this either counts the operator's own messages as waiting
 * for them or loses arrivals that share a second with the mark.
 */
static void
test_counts_after_the_mark_by_id(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("dm:alpha:user", NULL);

    put(room, "m1", "alpha", 100);
    put(room, "m2", "user", 100);
    put(room, "m3", "alpha", 100);
    put(room, "m4", "user", 100);
    put(room, "m5", "alpha", 100);

    g_assert_cmpuint(clawt_room_count_unread(room, "m2", 100), ==, 2);
    g_assert_cmpuint(clawt_room_count_unread(room, "m5", 100), ==, 0);
    g_assert_cmpuint(clawt_room_count_unread(room, "m1", 100), ==, 2);
}

/*
 * A room nobody has read counts every message from somebody else; the
 * operator's own never.  Regressing this makes a brand-new conversation
 * either silent or count the operator's own questions.
 */
static void
test_a_room_never_read_counts_everything_not_yours(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("standup", NULL);

    put(room, "m1", "user", 10);
    put(room, "m2", "alpha", 11);
    put(room, "m3", "beta", 12);

    g_assert_cmpuint(clawt_room_count_unread(room, NULL, 0), ==, 2);
}

/*
 * The mark names a message no longer in the history: the count falls
 * back to the timestamp, strictly after the mark's second.  Regressing
 * it counts the whole history as unread once a marked message is gone.
 */
static void
test_a_missing_id_falls_back_to_the_time(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("dm:alpha:user", NULL);

    put(room, "m1", "alpha", 100);
    put(room, "m2", "alpha", 200);
    put(room, "m3", "alpha", 300);
    put(room, "m4", "user", 400);

    g_assert_cmpuint(clawt_room_count_unread(room, "gone", 200), ==, 1);
    g_assert_cmpuint(clawt_room_count_unread(room, "gone", 50), ==, 3);
    g_assert_cmpuint(clawt_room_count_unread(room, "gone", 999), ==, 0);
}

/* An empty room has nothing unread whatever the mark says. */
static void
test_an_empty_room_counts_nothing(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("empty", NULL);

    g_assert_cmpuint(clawt_room_count_unread(room, NULL, 0), ==, 0);
    g_assert_cmpuint(clawt_room_count_unread(room, "m1", 100), ==, 0);
    g_assert_false(clawt_room_get_latest(room, NULL, NULL));
    g_assert_cmpint(clawt_room_message_index(room, "m1", NULL), ==, -1);
}

/*
 * Where a "read everything" mark goes, and the check a client's `upto`
 * is held to.  Regressing either marks the wrong message or lets a
 * client name one the room does not hold.
 */
static void
test_latest_and_find(void)
{
    g_autoptr(ClawtRoom) room = clawt_room_new("dm:alpha:user", NULL);
    const gchar *id = NULL;
    gint64 ts = 0;

    put(room, "m1", "alpha", 100);
    put(room, "m2", "user", 150);

    g_assert_true(clawt_room_get_latest(room, &id, &ts));
    g_assert_cmpstr(id, ==, "m2");
    g_assert_cmpint(ts, ==, 150);

    g_assert_cmpint(clawt_room_message_index(room, "m1", &ts), ==, 0);
    g_assert_cmpint(ts, ==, 100);
    g_assert_cmpint(clawt_room_message_index(room, "m2", NULL), ==, 1);
    g_assert_cmpint(clawt_room_message_index(room, "nope", &ts), ==, -1);
    g_assert_cmpint(ts, ==, 0);
}

/* ── The "New messages" rule ──────────────────────────────────────── */

static JsonArray *
history(JsonParser *parser, const gchar *json)
{
    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));

    return json_node_get_array(json_parser_get_root(parser));
}

/*
 * The rule goes above the first message after the mark that somebody
 * else sent -- skipping the operator's own -- and nowhere when nothing
 * after the mark is from anybody else.  Regressing it draws the rule
 * above the operator's own reply, or above everything.
 */
static void
test_the_rule_goes_after_the_mark(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonArray *messages = history(parser,
        "[{\"id\":\"a1\",\"sender\":\"alpha\"},"
        " {\"id\":\"a2\",\"sender\":\"user\"},"
        " {\"id\":\"a3\",\"sender\":\"user\"},"
        " {\"id\":\"a4\",\"sender\":\"alpha\"},"
        " {\"id\":\"a5\",\"sender\":\"alpha\"}]");

    g_assert_cmpint(clawt_unread_rule_index(messages, "a1"), ==, 3);
    g_assert_cmpint(clawt_unread_rule_index(messages, "a3"), ==, 3);
    g_assert_cmpint(clawt_unread_rule_index(messages, "a4"), ==, 4);
    g_assert_cmpint(clawt_unread_rule_index(messages, "a5"), ==, -1);
}

/*
 * The edges: a daemon without marks (NULL) draws no rule; a room never
 * read ("") puts it above the first message from somebody else; a mark
 * older than the window loaded is placed by id, which sorts by time.
 * Regressing the NULL case draws a rule above every transcript on an
 * older daemon.
 */
static void
test_the_rule_edges(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonArray *messages = history(parser,
        "[{\"id\":\"b2\",\"sender\":\"user\"},"
        " {\"id\":\"b3\",\"sender\":\"alpha\"}]");

    g_assert_cmpint(clawt_unread_rule_index(messages, NULL), ==, -1);
    g_assert_cmpint(clawt_unread_rule_index(messages, ""), ==, 1);
    g_assert_cmpint(clawt_unread_rule_index(messages, "b1"), ==, 1);
    g_assert_cmpint(clawt_unread_rule_index(messages, "b9"), ==, -1);
    g_assert_cmpint(clawt_unread_rule_index(NULL, ""), ==, -1);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/chat-room/unread/by-id", test_counts_after_the_mark_by_id);
    g_test_add_func("/chat-room/unread/never-read",
                    test_a_room_never_read_counts_everything_not_yours);
    g_test_add_func("/chat-room/unread/time-fallback",
                    test_a_missing_id_falls_back_to_the_time);
    g_test_add_func("/chat-room/unread/empty", test_an_empty_room_counts_nothing);
    g_test_add_func("/chat-room/unread/latest-and-find", test_latest_and_find);
    g_test_add_func("/chat-room/rule/after-the-mark",
                    test_the_rule_goes_after_the_mark);
    g_test_add_func("/chat-room/rule/edges", test_the_rule_edges);

    return g_test_run();
}
