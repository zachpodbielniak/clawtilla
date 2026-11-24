/*
 * test-lrg-models-chat.c - The sidebar's and the chat page's decisions
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clients/lrg/lrg-chat-model.c (and the notifications drawer's two
 * decisions in lrg-alerts-model.c), without libregnum.  Keyed state is
 * tested with two keys -- two rooms, two agents -- because a test with
 * one key cannot see a shared-scalar bug (AGENTS.md).
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "../clients/lrg/lrg-chat-model.c"
#include "../clients/lrg/lrg-agent-model.c"
#include "../clients/lrg/lrg-alerts-model.c"

/* ── Fixtures ─────────────────────────────────────────────────────── */

static JsonNode *
parse(const gchar *json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    if (!json_parser_load_from_data(parser, json, -1, &error))
        g_error("fixture does not parse: %s", error->message);

    return json_node_copy(json_parser_get_root(parser));
}

/*
 * Two teams, one of them empty; a teamless chief; two agents on "red";
 * a declared room on "red", a declared teamless room, an undeclared peer
 * room and a routine room.
 */
static const gchar *const AGENTS_JSON =
    "{\"agents\":["
    "{\"id\":\"chief\",\"name\":\"Chief\",\"state\":\"running\","
    " \"dm_room\":\"dm:chief:user\",\"chief_of_staff\":true,"
    " \"caps\":\"interrupt\",\"busy\":false},"
    "{\"id\":\"ann\",\"name\":\"Ann\",\"state\":\"running\",\"team\":\"red\","
    " \"team_role\":\"lead\",\"dm_room\":\"dm:ann:user\","
    " \"caps\":\"host-control,interrupt\",\"busy\":true,\"peer\":\"chief\","
    " \"description\":\"leads red\",\"mailbox_depth\":2},"
    "{\"id\":\"bob\",\"name\":\"Bob\",\"state\":\"stopped\",\"team\":\"red\","
    " \"dm_room\":\"dm:bob:user\",\"description\":\"does red work\","
    " \"computer\":\"container\",\"computer_machine\":true,"
    " \"computer_stop_removes\":true}"
    "]}";

static const gchar *const TEAMS_JSON =
    "{\"teams\":["
    "{\"id\":\"red\",\"name\":\"Red team\",\"description\":\"the reds\"},"
    "{\"id\":\"blue\",\"name\":\"Blue team\"}"
    "]}";

static const gchar *const ROOMS_JSON =
    "{\"rooms\":["
    "{\"id\":\"dm:ann:user\",\"members\":[\"ann\",\"user\"]},"
    "{\"id\":\"dm:ann:chief\",\"members\":[\"ann\",\"chief\"]},"
    "{\"id\":\"war\",\"name\":\"War room\",\"declared\":true,"
    " \"team\":\"red\",\"require_mention\":true,"
    " \"members\":[\"ann\",\"bob\",\"user\"]},"
    "{\"id\":\"lobby\",\"declared\":true,"
    " \"members\":[\"chief\",\"ann\",\"user\"]},"
    "{\"id\":\"routine:nightly\",\"members\":[\"bob\"]}"
    "]}";

/* ── Unread ───────────────────────────────────────────────────────── */

static GHashTable *
new_unread(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void
test_row_rooms(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GHashTable) rows = clawt_lrg_chat_row_rooms(agents, rooms);

    /* every agent's own conversation and every declared room... */
    g_assert_true(g_hash_table_contains(rows, "dm:chief:user"));
    g_assert_true(g_hash_table_contains(rows, "dm:ann:user"));
    g_assert_true(g_hash_table_contains(rows, "dm:bob:user"));
    g_assert_true(g_hash_table_contains(rows, "war"));
    g_assert_true(g_hash_table_contains(rows, "lobby"));

    /* ...and no peer or routine room, which has no row to clear it */
    g_assert_false(g_hash_table_contains(rows, "dm:ann:chief"));
    g_assert_false(g_hash_table_contains(rows, "routine:nightly"));
    g_assert_cmpuint(g_hash_table_size(rows), ==, 5);
}

static void
test_unread_two_rooms(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GHashTable) rows = clawt_lrg_chat_row_rooms(agents, rooms);
    g_autoptr(GHashTable) unread = new_unread();
    gint64 connected = 1000;

    /* two rooms, counted apart */
    g_assert_true(clawt_lrg_unread_note(unread, "dm:ann:user", "war", "ann", NULL,
                                        2000, connected, rows));
    g_assert_true(clawt_lrg_unread_note(unread, "dm:ann:user", "war", "ann", NULL,
                                        2001, connected, rows));
    g_assert_true(clawt_lrg_unread_note(unread, "lobby", "war", "chief", NULL,
                                        2002, connected, rows));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "dm:ann:user"), ==, 2);
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "lobby"), ==, 1);
    g_assert_cmpuint(clawt_lrg_unread_total(unread), ==, 3);

    /* the room on screen never counts, whatever the scroll position */
    g_assert_false(clawt_lrg_unread_note(unread, "war", "war", "ann", NULL, 2003,
                                         connected, rows));

    /* the operator's own words do not count */
    g_assert_false(clawt_lrg_unread_note(unread, "lobby", "war", "user", NULL, 2004,
                                         connected, rows));

    /* a replay from before this client connected does not count */
    g_assert_false(clawt_lrg_unread_note(unread, "lobby", "war", "chief", NULL, 10,
                                         connected, rows));

    /* peer traffic has no row to clear it, so it never counts */
    g_assert_false(clawt_lrg_unread_note(unread, "dm:ann:chief", "war", "ann", NULL,
                                         2005, connected, rows));

    /* a change note is a record, not a word from the agent: never counted */
    g_assert_false(clawt_lrg_unread_note(unread, "dm:ann:user", "war",
                                         CLAWT_SYSTEM_SENDER, "routine",
                                         2007, connected, rows));

    /* no listing yet: nothing is known, so nothing counts */
    g_assert_false(clawt_lrg_unread_note(unread, "lobby", NULL, "chief", NULL,
                                         2006, connected, NULL));
    g_assert_cmpuint(clawt_lrg_unread_total(unread), ==, 3);

    /* opening one clears that one only */
    g_assert_true(clawt_lrg_unread_clear(unread, "dm:ann:user"));
    g_assert_false(clawt_lrg_unread_clear(unread, "dm:ann:user"));
    g_assert_false(clawt_lrg_unread_clear(unread, NULL));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "lobby"), ==, 1);
    g_assert_cmpuint(clawt_lrg_unread_total(unread), ==, 1);

    /* the tab hides the number while the chat page is the one showing */
    g_assert_cmpuint(clawt_lrg_unread_badge(unread, NULL, TRUE), ==, 0);
    g_assert_cmpuint(clawt_lrg_unread_badge(unread, NULL, FALSE), ==, 1);
}

/*
 * The daemon's counts, taken from room.list: every room with a row gets
 * the daemon's number, the room on screen and a zero are dropped, and a
 * peer room -- no row to clear it from -- is never seeded.  Two rooms,
 * so a seed that wrote one key for all would show.  Regressing this is a
 * window that opens showing nothing for conversations read nowhere, or
 * one that keeps a count another client already cleared.
 */
static void
test_unread_seed(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(JsonNode) rooms = parse(
        "{\"rooms\":["
        "{\"id\":\"dm:ann:user\",\"unread\":3,\"read_upto\":\"\"},"
        "{\"id\":\"dm:ann:chief\",\"unread\":9,\"read_upto\":\"\"},"
        "{\"id\":\"war\",\"declared\":true,\"unread\":2,"
        " \"read_upto\":\"x\"},"
        "{\"id\":\"lobby\",\"declared\":true,\"unread\":0,"
        " \"read_upto\":\"y\"}"
        "]}");
    g_autoptr(JsonNode) old_daemon = parse(ROOMS_JSON);
    g_autoptr(GHashTable) unread = new_unread();

    /* a count the daemon now says is zero goes */
    g_hash_table_insert(unread, g_strdup("lobby"), GUINT_TO_POINTER(5));

    g_assert_true(clawt_lrg_unread_seed(unread, agents, rooms, NULL));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "dm:ann:user"), ==, 3);
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "war"), ==, 2);
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "lobby"), ==, 0);
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "dm:ann:chief"), ==, 0);
    g_assert_cmpuint(clawt_lrg_unread_total(unread), ==, 5);

    /* the room on screen never shows a count, whatever the daemon says */
    g_assert_true(clawt_lrg_unread_seed(unread, agents, rooms, "war"));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "war"), ==, 0);
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "dm:ann:user"), ==, 3);

    /* a daemon without marks leaves the live count alone */
    g_hash_table_insert(unread, g_strdup("lobby"), GUINT_TO_POINTER(1));
    g_assert_false(clawt_lrg_unread_seed(unread, agents, old_daemon, NULL));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "lobby"), ==, 1);
}

/*
 * A `room.read` event: that room takes the daemon's count -- 0 clears
 * it, a mark set short of the end leaves the rest -- and the other room
 * is untouched.  Regressing this leaves a badge lit in this client for
 * a conversation read in another one.
 */
static void
test_unread_apply_read(void)
{
    g_autoptr(GHashTable) unread = new_unread();

    g_hash_table_insert(unread, g_strdup("dm:ann:user"), GUINT_TO_POINTER(3));
    g_hash_table_insert(unread, g_strdup("war"), GUINT_TO_POINTER(2));

    g_assert_true(clawt_lrg_unread_apply_read(unread, "dm:ann:user", 0));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "dm:ann:user"), ==, 0);
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "war"), ==, 2);

    /* already clear: nothing changed, nothing to redraw */
    g_assert_false(clawt_lrg_unread_apply_read(unread, "dm:ann:user", 0));

    g_assert_true(clawt_lrg_unread_apply_read(unread, "war", 1));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "war"), ==, 1);
    g_assert_false(clawt_lrg_unread_apply_read(unread, "war", 1));
    g_assert_false(clawt_lrg_unread_apply_read(unread, NULL, 0));
}

static void
test_unread_prune(void)
{
    g_autoptr(GHashTable) unread = new_unread();
    g_autoptr(GHashTable) rows = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_insert(unread, g_strdup("gone"), GUINT_TO_POINTER(4));
    g_hash_table_insert(unread, g_strdup("kept"), GUINT_TO_POINTER(2));
    g_hash_table_add(rows, (gpointer)"kept");

    /* not knowing the fleet is not the fleet being empty */
    g_assert_false(clawt_lrg_unread_prune(unread, NULL));
    g_assert_cmpuint(clawt_lrg_unread_total(unread), ==, 6);

    g_assert_true(clawt_lrg_unread_prune(unread, rows));
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "gone"), ==, 0);
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "kept"), ==, 2);
    g_assert_false(clawt_lrg_unread_prune(unread, rows));
}

/* ── Typing ───────────────────────────────────────────────────────── */

static GHashTable *
new_typing(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                 (GDestroyNotify)g_hash_table_unref);
}

static void
test_typing_is_a_level_per_room(void)
{
    g_autoptr(GHashTable) typing = new_typing();

    /* nothing heard: the listing answers */
    g_assert_true(clawt_lrg_typing_busy(typing, "ann", TRUE));
    g_assert_false(clawt_lrg_typing_busy(typing, "ann", FALSE));

    /* ann starts in two rooms: one rising edge */
    g_assert_true(clawt_lrg_typing_note(typing, "ann", "dm:ann:user", TRUE));
    g_assert_false(clawt_lrg_typing_note(typing, "ann", "war", TRUE));

    /* the 25-second re-send is not a new turn */
    g_assert_false(clawt_lrg_typing_note(typing, "ann", "dm:ann:user", TRUE));
    g_assert_true(clawt_lrg_typing_busy(typing, "ann", FALSE));
    g_assert_true(clawt_lrg_typing_in_room(typing, "ann", "war"));

    /* bob is a second key and is untouched by ann */
    g_assert_false(clawt_lrg_typing_busy(typing, "bob", FALSE));
    g_assert_true(clawt_lrg_typing_note(typing, "bob", "dm:bob:user", TRUE));
    g_assert_false(clawt_lrg_typing_in_room(typing, "bob", "war"));

    /* ann ends one room: still busy, no edge */
    g_assert_false(clawt_lrg_typing_note(typing, "ann", "war", FALSE));
    g_assert_true(clawt_lrg_typing_busy(typing, "ann", FALSE));

    /* a lowering for a room never raised is nothing */
    g_assert_false(clawt_lrg_typing_note(typing, "ann", "elsewhere", FALSE));

    /* ann ends the last one: the falling edge, and "known idle" beats a
     * stale listing that still says busy */
    g_assert_true(clawt_lrg_typing_note(typing, "ann", "dm:ann:user", FALSE));
    g_assert_false(clawt_lrg_typing_busy(typing, "ann", TRUE));

    /* bob unaffected by all of that */
    g_assert_true(clawt_lrg_typing_busy(typing, "bob", FALSE));

    /* a frame without a room is one room, spelled "" */
    g_assert_true(clawt_lrg_typing_note(typing, "ann", NULL, TRUE));
    g_assert_true(clawt_lrg_typing_in_room(typing, "ann", ""));
}

static void
test_typing_forget(void)
{
    g_autoptr(GHashTable) typing = new_typing();

    g_assert_false(clawt_lrg_typing_forget(typing, "ann"));
    clawt_lrg_typing_note(typing, "ann", "a", TRUE);
    clawt_lrg_typing_note(typing, "ann", "b", TRUE);
    clawt_lrg_typing_note(typing, "bob", "c", TRUE);

    /* an agent that stopped: every room of it, and only it */
    g_assert_true(clawt_lrg_typing_forget(typing, "ann"));
    g_assert_false(clawt_lrg_typing_busy(typing, "ann", TRUE));
    g_assert_true(clawt_lrg_typing_busy(typing, "bob", FALSE));

    /* and the next frame is a rising edge again */
    g_assert_true(clawt_lrg_typing_note(typing, "ann", "a", TRUE));
}

static void
test_typing_activity(void)
{
    g_autofree gchar *mine = clawt_lrg_typing_activity(TRUE, NULL);
    g_autofree gchar *user = clawt_lrg_typing_activity(TRUE, "user");
    g_autofree gchar *peer = clawt_lrg_typing_activity(TRUE, "chief");

    g_assert_null(clawt_lrg_typing_activity(FALSE, "chief"));
    g_assert_cmpstr(mine, ==, "thinking");
    g_assert_cmpstr(user, ==, "thinking");
    g_assert_cmpstr(peer, ==, "working for chief");
}

/* ── Agent rows ───────────────────────────────────────────────────── */

static JsonObject *
agent_at(JsonNode *agents, guint index)
{
    return json_array_get_object_element(
        json_object_get_array_member(json_node_get_object(agents), "agents"),
        index);
}

/*
 * An agent on one of its fallbacks carries the library's badge and the
 * daemon's sentence; one on its own backend carries neither.
 */
static void
test_agent_row_on_fallback(void)
{
    g_autoptr(JsonNode) away = parse(
        "{\"id\":\"scout\",\"state\":\"running\","
        "\"failover\":{\"state\":\"fallback\","
        "\"active\":{\"provider\":\"codex-cli\",\"model\":\"gpt-5.5\"}},"
        "\"failover_message\":\"scout is running on fallback\"}");
    g_autoptr(JsonNode) home = parse("{\"id\":\"scout\",\"state\":\"running\"}");
    g_autoptr(GHashTable) unread = new_unread();
    g_autoptr(GHashTable) typing = new_typing();
    g_autoptr(ClawtLrgAgentRow) on = clawt_lrg_agent_row_new(
        json_node_get_object(away), unread, typing, TRUE);
    g_autoptr(ClawtLrgAgentRow) off = clawt_lrg_agent_row_new(
        json_node_get_object(home), unread, typing, TRUE);

    /* short: the model is on its page, the row needs the width */
    g_assert_cmpstr(on->fallback, ==, "On fallback");
    g_assert_cmpstr(on->fallback_message, ==, "scout is running on fallback");
    g_assert_null(off->fallback);
    g_assert_null(off->fallback_message);
}

static void
test_agent_row(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(GHashTable) unread = new_unread();
    g_autoptr(GHashTable) typing = new_typing();
    g_autoptr(ClawtLrgAgentRow) chief = NULL;
    g_autoptr(ClawtLrgAgentRow) ann = NULL;
    g_autoptr(ClawtLrgAgentRow) bob = NULL;
    g_autoptr(ClawtLrgAgentRow) bob_quiet = NULL;

    g_hash_table_insert(unread, g_strdup("dm:ann:user"), GUINT_TO_POINTER(3));

    chief = clawt_lrg_agent_row_new(agent_at(agents, 0), unread, typing, TRUE);
    ann = clawt_lrg_agent_row_new(agent_at(agents, 1), unread, typing, TRUE);
    bob = clawt_lrg_agent_row_new(agent_at(agents, 2), unread, typing, TRUE);

    g_assert_cmpint(chief->standing, ==, CLAWT_TEAM_BADGE_CHIEF);
    g_assert_cmpstr(chief->team, ==, "");
    g_assert_true(chief->live);
    g_assert_false(chief->busy);
    g_assert_cmpuint(chief->unread, ==, 0);

    /* busy, for a peer, with mail waiting: all of it in the subtitle */
    g_assert_cmpint(ann->standing, ==, CLAWT_TEAM_BADGE_LEAD);
    g_assert_true(ann->host);
    g_assert_true(ann->can_interrupt);
    g_assert_true(ann->busy);
    g_assert_cmpuint(ann->unread, ==, 3);
    g_assert_nonnull(strstr(ann->subtitle, "chief"));
    g_assert_nonnull(strstr(ann->subtitle, "2 waiting"));
    g_assert_cmpstr(ann->state_tone, ==, "good");

    g_assert_cmpstr(bob->subtitle, ==, "does red work");
    g_assert_cmpstr(bob->state_tone, ==, "neutral");
    g_assert_false(bob->live);
    g_assert_true(bob->machine);
    g_assert_true(bob->stop_removes);
    g_assert_cmpstr(bob->computer, ==, "container");

    /* descriptions off: gone from the subtitle, kept on the row */
    bob_quiet = clawt_lrg_agent_row_new(agent_at(agents, 2), unread, typing,
                                        FALSE);
    g_assert_null(bob_quiet->subtitle);
    g_assert_cmpstr(bob_quiet->description, ==, "does red work");

    /* the typing frames outrank the listing: ann's turn ended */
    clawt_lrg_typing_note(typing, "ann", "dm:ann:chief", TRUE);
    clawt_lrg_typing_note(typing, "ann", "dm:ann:chief", FALSE);
    {
        g_autoptr(ClawtLrgAgentRow) idle =
            clawt_lrg_agent_row_new(agent_at(agents, 1), unread, typing, TRUE);

        g_assert_false(idle->busy);
        g_assert_cmpstr(idle->subtitle, ==, "2 waiting to be read");
    }
}

static void
test_agent_verbs(void)
{
    g_assert_false(clawt_lrg_agent_verb_allowed("running", "start"));
    g_assert_true(clawt_lrg_agent_verb_allowed("running", "stop"));
    g_assert_true(clawt_lrg_agent_verb_allowed("stopped", "start"));
    g_assert_false(clawt_lrg_agent_verb_allowed("stopped", "stop"));
    g_assert_false(clawt_lrg_agent_verb_allowed("shadow", "start"));
    g_assert_false(clawt_lrg_agent_verb_allowed("shadow", "restart"));
    g_assert_true(clawt_lrg_agent_verb_allowed("shadow", "delete"));
    g_assert_false(clawt_lrg_agent_verb_allowed("running", "explode"));

    g_assert_cmpstr(clawt_lrg_agent_state_tone("error"), ==, "bad");
    g_assert_cmpstr(clawt_lrg_agent_state_tone("starting"), ==, "warn");
    g_assert_cmpstr(clawt_lrg_agent_state_tone(NULL), ==, "neutral");
}

/* ── The sidebar's order ──────────────────────────────────────────── */

static gchar *
describe(GPtrArray *entries)
{
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; i < entries->len; i++) {
        ClawtLrgSidebarEntry *entry = g_ptr_array_index(entries, i);
        const gchar *kind = "?";

        switch (entry->kind) {
        case CLAWT_LRG_ENTRY_TEAM:  kind = "T"; break;
        case CLAWT_LRG_ENTRY_AGENT: kind = "A"; break;
        case CLAWT_LRG_ENTRY_ROOM:  kind = "R"; break;
        case CLAWT_LRG_ENTRY_EMPTY: kind = "E"; break;
        }

        g_string_append_printf(out, "%s%s:%s", out->len > 0 ? " " : "",
                               kind, entry->id);
    }

    return g_string_free(out, FALSE);
}

static void
test_sidebar_order(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(JsonNode) teams = parse(TEAMS_JSON);
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GPtrArray) entries = NULL;
    g_autofree gchar *order = NULL;
    ClawtLrgSidebarEntry *red;

    entries = clawt_lrg_sidebar_entries(agents, teams, rooms, NULL);
    order = describe(entries);

    /*
     * Teamless first with its room, red with its agents and then its
     * room, and the empty blue heading at the end so there is somewhere
     * to drop an agent onto it.
     */
    g_assert_cmpstr(order, ==,
                    "T: A:chief R:lobby T:red A:ann A:bob R:war T:blue");

    red = g_ptr_array_index(entries, 3);
    g_assert_cmpstr(red->title, ==, "Red team");
    g_assert_cmpstr(red->description, ==, "the reds");
    g_assert_cmpuint(red->total, ==, 2);
    g_assert_cmpuint(red->running, ==, 1);
    g_assert_cmpuint(red->busy, ==, 1);
    g_assert_cmpstr(red->subtitle, ==, "1 working \302\267 1/2");

    /* the teamless heading says so in words */
    g_assert_cmpstr(((ClawtLrgSidebarEntry *)
                     g_ptr_array_index(entries, 0))->title, ==, "No team");

    /* a room under the group it was put in */
    g_assert_cmpstr(clawt_lrg_entry_team_of(entries, "r:war"), ==, "red");
    g_assert_cmpstr(clawt_lrg_entry_team_of(entries, "a:chief"), ==, "");
    g_assert_null(clawt_lrg_entry_team_of(entries, "a:nobody"));
}

static void
test_sidebar_collapsed(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(JsonNode) teams = parse(TEAMS_JSON);
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GHashTable) collapsed = g_hash_table_new(g_str_hash,
                                                       g_str_equal);
    g_autoptr(GPtrArray) entries = NULL;
    g_autofree gchar *order = NULL;
    ClawtLrgSidebarEntry *red;

    g_hash_table_add(collapsed, (gpointer)"red");
    entries = clawt_lrg_sidebar_entries(agents, teams, rooms, collapsed);
    order = describe(entries);

    /*
     * Red keeps its heading and its tally, loses its agents -- and its
     * room is hidden, not moved to the bottom under some other heading.
     */
    g_assert_cmpstr(order, ==, "T: A:chief R:lobby T:red T:blue");

    red = g_ptr_array_index(entries, 3);
    g_assert_true(red->collapsed);
    g_assert_cmpuint(red->total, ==, 2);

    /* folded rows are "cannot tell", never "teamless" */
    g_assert_null(clawt_lrg_entry_team_of(entries, "a:ann"));
}

static void
test_sidebar_empty_fleet(void)
{
    g_autoptr(JsonNode) agents = parse("{\"agents\":[]}");
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GPtrArray) entries = NULL;
    g_autofree gchar *order = NULL;

    entries = clawt_lrg_sidebar_entries(agents, NULL, rooms, NULL);
    order = describe(entries);

    /* the placeholder, and the teamless declared room not lost */
    g_assert_cmpstr(order, ==, "E: R:lobby");

    g_clear_pointer(&entries, g_ptr_array_unref);
    entries = clawt_lrg_sidebar_entries(NULL, NULL, NULL, NULL);
    g_assert_cmpuint(entries->len, ==, 1);
}

static void
test_room_labels(void)
{
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    JsonArray *array = json_object_get_array_member(
        json_node_get_object(rooms), "rooms");
    g_autofree gchar *war = clawt_lrg_room_label(
        json_array_get_object_element(array, 2));
    g_autofree gchar *lobby = clawt_lrg_room_label(
        json_array_get_object_element(array, 3));
    g_autofree gchar *war_sub = clawt_lrg_room_subtitle(
        json_array_get_object_element(array, 2));
    g_autofree gchar *lobby_sub = clawt_lrg_room_subtitle(
        json_array_get_object_element(array, 3));

    g_assert_cmpstr(war, ==, "War room");

    /* unnamed: its members, without the operator */
    g_assert_cmpstr(lobby, ==, "chief, ann");

    g_assert_cmpstr(war_sub, ==, "3 members \302\267 answers when named");
    g_assert_cmpstr(lobby_sub, ==, "3 members \302\267 everyone answers");
}

static void
test_reorder(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(JsonNode) teams = parse(TEAMS_JSON);
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GPtrArray) entries = NULL;
    g_autofree gchar *above = NULL;
    g_autofree gchar *below = NULL;
    g_autofree gchar *last = NULL;

    entries = clawt_lrg_sidebar_entries(agents, teams, rooms, NULL);

    /* the whole order, headings left out, the dragged row moved */
    above = clawt_lrg_reorder_entries(entries, "a:bob", "a:chief", FALSE);
    g_assert_cmpstr(above, ==, "a:bob,a:chief,r:lobby,a:ann,r:war");

    below = clawt_lrg_reorder_entries(entries, "a:chief", "a:ann", TRUE);
    g_assert_cmpstr(below, ==, "r:lobby,a:ann,a:chief,a:bob,r:war");

    /* the lower half of the last row is the only way to be last */
    last = clawt_lrg_reorder_entries(entries, "r:lobby", "r:war", TRUE);
    g_assert_cmpstr(last, ==, "a:chief,a:ann,a:bob,r:war,r:lobby");

    /* onto itself, or onto nothing, means nothing */
    g_assert_null(clawt_lrg_reorder_entries(entries, "a:ann", "a:ann", TRUE));
    g_assert_null(clawt_lrg_reorder_entries(entries, "a:ann", "a:ghost",
                                            TRUE));
}

static void
test_team_choices(void)
{
    g_autoptr(JsonNode) teams = parse(TEAMS_JSON);
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);

    clawt_lrg_team_choices(teams, "typo", ids, labels);

    g_assert_cmpuint(ids->len, ==, 4);
    g_assert_cmpstr(g_ptr_array_index(ids, 0), ==, "");
    g_assert_cmpstr(g_ptr_array_index(labels, 0), ==, "No team");
    g_assert_cmpstr(g_ptr_array_index(labels, 1), ==, "Red team");
    g_assert_cmpstr(g_ptr_array_index(ids, 3), ==, "typo");

    g_assert_cmpstr(clawt_lrg_team_name(teams, "blue"), ==, "Blue team");
    g_assert_cmpstr(clawt_lrg_team_name(teams, "typo"), ==, "typo");
}

static void
test_lifecycle_texts(void)
{
    g_autoptr(JsonNode) removed = parse("{\"computer\":\"removed\"}");
    g_autoptr(JsonNode) kept = parse("{\"computer\":\"busy\"}");
    g_autoptr(JsonNode) plain = parse("{}");
    g_autoptr(JsonNode) power = parse("{\"state\":\"stopped\","
                                      "\"removes\":true}");
    g_autofree gchar *a = clawt_lrg_removed_text("bob", removed);
    g_autofree gchar *b = clawt_lrg_removed_text("bob", kept);
    g_autofree gchar *c = clawt_lrg_removed_text("bob", plain);
    g_autofree gchar *d = clawt_lrg_power_text("bob", power);
    g_autofree gchar *e = clawt_lrg_moved_text("bob", "red", "Red team");
    g_autofree gchar *f = clawt_lrg_moved_text("bob", "", NULL);

    g_assert_cmpstr(a, ==, "bob and its computer are gone.");
    g_assert_cmpstr(b, ==, "bob is gone; its computer was not removed: busy");
    g_assert_cmpstr(c, ==, "bob is gone.");
    g_assert_nonnull(strstr(d, "does not survive a stop"));
    g_assert_cmpstr(e, ==, "bob moved to Red team.");
    g_assert_cmpstr(f, ==, "bob taken off its team.");
}

/* ── Slash commands ───────────────────────────────────────────────── */

static void
test_slash_commands(void)
{
    static const gchar *const expected[] = {
        "/agents", "/attach", "/clear", "/compose", "/copy", "/edit",
        "/export", "/files", "/flow", "/help", "/interrupt", "/memory",
        "/new", "/recall", "/reset", "/restart", "/retry", "/start", "/stop",
        "/tasks", NULL
    };
    guint n = 0;
    guint i;
    g_autofree gchar *help = clawt_lrg_slash_help();

    clawt_lrg_slash_commands(&n);
    g_assert_cmpuint(n, ==, g_strv_length((gchar **)expected));

    /* every GTK command, and the help names each one */
    for (i = 0; expected[i] != NULL; i++) {
        g_assert_nonnull(clawt_lrg_slash_find(expected[i]));
        g_assert_nonnull(strstr(help, expected[i]));
    }

    g_assert_null(clawt_lrg_slash_find("/nope"));
    g_assert_nonnull(strstr(help, "/memory <query>"));
    g_assert_nonnull(strstr(help, "Ctrl+G"));
}

static void
test_slash_split(void)
{
    gchar *name = NULL;
    gchar *rest = NULL;

    g_assert_false(clawt_lrg_slash_split("hello", &name, &rest));
    g_assert_null(name);

    g_assert_true(clawt_lrg_slash_split("/memory  cats and dogs ", &name,
                                        &rest));
    g_assert_cmpstr(name, ==, "/memory");
    g_assert_cmpstr(rest, ==, "cats and dogs");
    g_free(name);
    g_free(rest);

    /* only spaces after the word is no argument at all */
    g_assert_true(clawt_lrg_slash_split("/files   ", &name, &rest));
    g_assert_cmpstr(name, ==, "/files");
    g_assert_null(rest);
    g_free(name);

    g_assert_true(clawt_lrg_slash_completing("/me"));
    g_assert_false(clawt_lrg_slash_completing("/memory x"));
    g_assert_false(clawt_lrg_slash_completing("me"));
    g_assert_false(clawt_lrg_slash_completing(NULL));

    g_assert_true(clawt_lrg_slash_needs_agent("/reset"));
    g_assert_true(clawt_lrg_slash_needs_agent("/export"));
    g_assert_false(clawt_lrg_slash_needs_agent("/recall"));
    g_assert_false(clawt_lrg_slash_needs_agent("/help"));
    g_assert_nonnull(strstr(clawt_lrg_slash_needs_agent_text(TRUE), "room"));
}

static void
test_command_texts(void)
{
    g_autoptr(JsonNode) agents = parse(AGENTS_JSON);
    g_autoptr(JsonNode) files = parse(
        "{\"files\":[{\"name\":\"SOUL.org\",\"title\":\"who\","
        "\"path\":\"/ws/SOUL.org\"}]}");
    g_autoptr(JsonNode) memories = parse(
        "{\"memories\":[{\"id\":\"m1\",\"category\":\"fact\","
        "\"content\":\"the sky\"}]}");
    g_autoptr(JsonNode) none = parse("{\"memories\":[]}");
    g_autoptr(JsonNode) reset = parse("{\"sessions_cleared\":1,"
                                      "\"restarted\":true}");
    g_autoptr(JsonNode) killed = parse("{\"killed\":2}");
    g_autoptr(JsonNode) idle = parse("{\"killed\":0}");
    g_autoptr(JsonNode) history = parse(
        "{\"messages\":[{\"sender\":\"user\",\"body\":\"first\"},"
        "{\"sender\":\"ann\",\"body\":\"reply\"},"
        "{\"sender\":\"user\",\"body\":\"second\"}]}");
    g_autofree gchar *who = clawt_lrg_chat_agents_text(agents);
    g_autofree gchar *list = clawt_lrg_chat_files_text(files);
    g_autofree gchar *mem = clawt_lrg_chat_memory_text(memories);
    g_autofree gchar *empty = clawt_lrg_chat_memory_text(none);
    g_autofree gchar *again = clawt_lrg_chat_reset_text(reset);
    g_autofree gchar *stop = clawt_lrg_chat_interrupt_text("ann", killed);
    g_autofree gchar *nothing = clawt_lrg_chat_interrupt_text("ann", idle);

    g_assert_nonnull(strstr(who, "bob"));
    g_assert_nonnull(strstr(who, "stopped"));
    g_assert_nonnull(strstr(list, "SOUL.org"));
    g_assert_nonnull(strstr(list, "/edit <name>"));
    g_assert_cmpstr(clawt_lrg_chat_file_path(files, "SOUL.org"), ==,
                    "/ws/SOUL.org");
    g_assert_null(clawt_lrg_chat_file_path(files, "../etc/passwd"));
    g_assert_cmpstr(mem, ==, "m1 [fact]\n  the sky\n");
    g_assert_cmpstr(empty, ==, "Nothing remembered matches that.");

    /* `restarted` is a JSON boolean, and is read as one */
    g_assert_nonnull(strstr(again, "1 stored session cleared and the agent "
                                   "restarted"));
    g_assert_nonnull(strstr(stop, "2 process(es)"));
    g_assert_nonnull(strstr(nothing, "between turns"));
    g_assert_cmpstr(clawt_lrg_chat_last_user_message(history), ==, "second");
    g_assert_null(clawt_lrg_chat_last_user_message(none));
}

static void
test_send_note(void)
{
    g_autoptr(JsonNode) steered = parse("{\"steered\":true,"
                                        "\"target_state\":\"running\"}");
    g_autoptr(JsonNode) stopped = parse("{\"target_state\":\"stopped\"}");
    g_autoptr(JsonNode) running = parse("{\"target_state\":\"running\"}");
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;
    gboolean waiting = TRUE;

    a = clawt_lrg_chat_send_note("ann", steered, &waiting);
    g_assert_nonnull(strstr(a, "mid-turn"));
    g_assert_false(waiting);

    b = clawt_lrg_chat_send_note("bob", stopped, &waiting);
    g_assert_nonnull(strstr(b, "bob is stopped"));
    g_assert_false(waiting);

    g_assert_null(clawt_lrg_chat_send_note("ann", running, &waiting));
    g_assert_true(waiting);
}

static void
test_export_and_chips(void)
{
    g_autoptr(JsonNode) history = parse(
        "{\"messages\":[{\"sender\":\"user\",\"body\":\"hi\",\"ts\":100},"
        "{\"sender\":\"ann\",\"body\":\"hello\",\"ts\":160}]}");
    g_autoptr(GPtrArray) messages = clawt_lrg_chat_export_messages(history,
                                                                   "war");
    g_autofree gchar *task = clawt_lrg_task_chip_text("0123456789abcdef");
    g_autofree gchar *hop = clawt_lrg_hop_text(3);

    g_assert_cmpint(clawt_lrg_export_format("copy-org"), ==,
                    CLAWT_EXPORT_ORG);
    g_assert_cmpint(clawt_lrg_export_format("save-text"), ==,
                    CLAWT_EXPORT_PLAIN);
    g_assert_cmpint(clawt_lrg_export_format("edit-markdown"), ==,
                    CLAWT_EXPORT_MARKDOWN);
    g_assert_cmpint(clawt_lrg_export_format(NULL), ==,
                    CLAWT_EXPORT_MARKDOWN);

    g_assert_cmpuint(messages->len, ==, 2);
    g_assert_cmpint(clawt_message_get_timestamp(
                        g_ptr_array_index(messages, 1)), ==, 160);

    g_assert_cmpstr(task, ==, "task 01234567");
    g_assert_null(clawt_lrg_task_chip_text(NULL));
    g_assert_cmpstr(hop, ==, "hop 3");
    g_assert_null(clawt_lrg_hop_text(1));
}

/* ── Runs, steps and attachments ──────────────────────────────────── */

static void
test_runs(void)
{
    ClawtLrgRun run = { NULL, NULL };
    g_autoptr(GDateTime) monday = g_date_time_new_local(2026, 11, 2, 10, 0, 0);
    g_autoptr(GDateTime) later = g_date_time_new_local(2026, 11, 2, 11, 0, 0);
    g_autoptr(GDateTime) tuesday = g_date_time_new_local(2026, 11, 3, 9, 0, 0);
    gboolean new_day = FALSE;

    g_assert_true(clawt_lrg_run_advance(&run, "ann", monday, &new_day));
    g_assert_true(new_day);

    /* the same sender the same day continues the run, however late */
    g_assert_false(clawt_lrg_run_advance(&run, "ann", later, &new_day));
    g_assert_false(new_day);

    g_assert_true(clawt_lrg_run_advance(&run, "user", later, &new_day));
    g_assert_false(new_day);

    /* a new day starts a run even from the same sender */
    g_assert_true(clawt_lrg_run_advance(&run, "user", tuesday, &new_day));
    g_assert_true(new_day);

    clawt_lrg_run_reset(&run);
    g_assert_null(run.sender);
    g_assert_true(clawt_lrg_run_advance(&run, "user", tuesday, NULL));
    clawt_lrg_run_reset(&run);
}

static ClawtTurnStep *
step_at(gint64 seconds)
{
    g_autoptr(JsonNode) node = NULL;
    g_autofree gchar *json = g_strdup_printf(
        "{\"step_kind\":\"tool\",\"tool\":\"bash\",\"text\":\"ls\","
        "\"room_id\":\"war\",\"ts\":%" G_GINT64_FORMAT "}",
        seconds * G_USEC_PER_SEC);

    node = parse(json);
    return clawt_turn_step_new_from_object(json_node_get_object(node), "ann");
}

static void
test_steps_merge(void)
{
    g_autoptr(GPtrArray) steps = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_turn_step_free);
    ClawtTurnStep *first = step_at(100);

    if (first == NULL) {
        g_test_skip("turn step fixture not understood by this libclawt");
        return;
    }

    g_ptr_array_add(steps, first);
    g_ptr_array_add(steps, step_at(150));
    g_ptr_array_add(steps, step_at(300));

    /* message stamps are seconds, step stamps microseconds */
    g_assert_cmpuint(clawt_lrg_steps_before(steps, 0, 50, NULL), ==, 0);
    g_assert_cmpuint(clawt_lrg_steps_before(steps, 0, 200, NULL), ==, 2);
    g_assert_cmpuint(clawt_lrg_steps_before(steps, 2, 200, NULL), ==, 2);
    g_assert_cmpuint(clawt_lrg_steps_before(steps, 2, 400, NULL), ==, 3);
    g_assert_cmpuint(clawt_lrg_steps_before(NULL, 0, 400, NULL), ==, 0);
}

/*
 * An MCP app in the conversation is a link out to the web client's view
 * of it -- lrg has no HTML engine -- and the link comes from the step's
 * own members, read off the wire the way room.steps sends them.
 */
static void
test_step_app_link(void)
{
    g_autoptr(JsonNode) node = parse(
        "{\"step_kind\":\"status\",\"room_id\":\"war\","
        "\"text\":\"App: Flowchart from excalidraw\","
        "\"tool\":\"create_view\",\"app_id\":\"a0123456789abcdef\","
        "\"app_url\":\"http://127.0.0.1:8790/a/ann/app/a0123456789abcdef\","
        "\"app_title\":\"Flowchart\",\"ts\":1000000}");
    g_autoptr(JsonNode) plain_node = parse(
        "{\"step_kind\":\"status\",\"room_id\":\"war\","
        "\"text\":\"Compacting\",\"ts\":1000000}");
    g_autoptr(ClawtTurnStep) step = clawt_turn_step_new_from_object(
        json_node_get_object(node), "ann");
    g_autoptr(ClawtTurnStep) plain = clawt_turn_step_new_from_object(
        json_node_get_object(plain_node), "ann");
    g_autofree gchar *label = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *expected = NULL;

    g_assert_true(clawt_lrg_step_app_link(step, &label, &url));
    expected = clawt_mcp_app_link_label("Flowchart");
    g_assert_cmpstr(label, ==, expected);
    g_assert_cmpstr(url, ==,
                    "http://127.0.0.1:8790/a/ann/app/a0123456789abcdef");

    /* A copy -- the live block keeps copies -- still links. */
    {
        g_autoptr(ClawtTurnStep) copy = clawt_turn_step_copy(step);

        g_assert_true(clawt_lrg_step_app_link(copy, NULL, NULL));
    }

    g_assert_false(clawt_lrg_step_app_link(plain, NULL, NULL));
}

/*
 * A trimmed-result notice is drawn as a warning; other status chatter
 * and a successful call stay muted, and a failure stays bad.
 */
static void
test_step_trim_notice(void)
{
    g_autofree gchar *notice = clawt_mcp_result_notice(
        "github", "list_issues", 2000000, 65536, "r0123456789abcdef");
    g_autoptr(ClawtTurnStep) trimmed = clawt_turn_step_new(
        CLAWT_STEP_STATUS, "ann", "war", notice, NULL, NULL, FALSE);
    g_autoptr(ClawtTurnStep) status = clawt_turn_step_new(
        CLAWT_STEP_STATUS, "ann", "war", "Compacting", NULL, NULL, FALSE);
    g_autoptr(ClawtTurnStep) call = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "ann", "war", NULL, "bash", "ls", FALSE);
    g_autoptr(ClawtTurnStep) failed = clawt_turn_step_new(
        CLAWT_STEP_TOOL, "ann", "war", NULL, "bash", "ls", TRUE);

    g_assert_true(clawt_mcp_result_is_notice(notice));
    g_assert_cmpstr(clawt_lrg_step_tone(trimmed), ==, "warn");
    g_assert_null(clawt_lrg_step_tone(status));
    g_assert_null(clawt_lrg_step_tone(call));
    g_assert_cmpstr(clawt_lrg_step_tone(failed), ==, "bad");
}

static void
test_attachment_refs(void)
{
    const gchar *body =
        "look at these\n\n"
        CLAWT_ATTACHMENT_MARKER ". Open them with your own read tool.\n"
        "- shot.png\n"
        "  /home/z/.clawtilla/exchange/shot.png\n"
        "  (inside your container: /exchange/shot.png)\n"
        "- notes.pdf\n"
        "  clawt:abc123\n"
        "And then I quoted /etc/passwd at the margin.\n";
    g_autoptr(GPtrArray) refs = clawt_lrg_attachment_refs(body);
    g_autoptr(GPtrArray) none = clawt_lrg_attachment_refs("just /a/path");
    ClawtLrgAttachmentRef *ref;

    g_assert_cmpuint(refs->len, ==, 2);

    ref = g_ptr_array_index(refs, 0);
    g_assert_false(ref->daemon_copy);
    g_assert_cmpstr(ref->value, ==, "/home/z/.clawtilla/exchange/shot.png");

    ref = g_ptr_array_index(refs, 1);
    g_assert_true(ref->daemon_copy);
    g_assert_cmpstr(ref->value, ==, "abc123");

    g_assert_cmpuint(none->len, ==, 0);

    g_assert_true(clawt_lrg_looks_like_image("/x/SHOT.PNG"));
    g_assert_false(clawt_lrg_looks_like_image("/x/notes.pdf"));
    g_assert_false(clawt_lrg_looks_like_image(NULL));
}

static void
test_body_with_attachments(void)
{
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_sent_file_free);
    ClawtLrgSentFile *ok = g_new0(ClawtLrgSentFile, 1);
    ClawtLrgSentFile *failed = g_new0(ClawtLrgSentFile, 1);
    g_autofree gchar *plain = clawt_lrg_body_with_attachments("hi", NULL);
    g_autofree gchar *body = NULL;
    g_autoptr(GPtrArray) refs = NULL;

    ok->name = g_strdup("shot.png");
    ok->host_path = g_strdup("/host/shot.png");
    ok->path = g_strdup("/exchange/shot.png");
    ok->saved = TRUE;
    failed->name = g_strdup("huge.mkv");
    g_ptr_array_add(files, ok);
    g_ptr_array_add(files, failed);

    g_assert_cmpstr(plain, ==, "hi");

    body = clawt_lrg_body_with_attachments("look", files);
    g_assert_true(g_str_has_prefix(body, "look\n\n" CLAWT_ATTACHMENT_MARKER));
    g_assert_nonnull(strstr(body, "  /host/shot.png\n"));
    g_assert_nonnull(strstr(body, "(inside your container: "
                                  "/exchange/shot.png)"));
    g_assert_nonnull(strstr(body, "- huge.mkv (could not be saved)"));

    /* what one client writes, the preview reader finds again */
    refs = clawt_lrg_attachment_refs(body);
    g_assert_cmpuint(refs->len, ==, 1);
    g_assert_cmpstr(((ClawtLrgAttachmentRef *)
                     g_ptr_array_index(refs, 0))->value, ==,
                    "/host/shot.png");

    g_assert_false(clawt_lrg_attachment_too_big(32u * 1024u * 1024u));
    g_assert_true(clawt_lrg_attachment_too_big(32u * 1024u * 1024u + 1u));
}

/*
 * An operator's file is named twice -- by host path for the agent, and
 * by `clawt:<id>` for the clients -- and drawn once, from the daemon's
 * copy: the host path opens only on the daemon's machine, so a remote
 * client previewing from it showed nothing.  An older message with only
 * a path still previews from the path.
 */
static void
test_attachment_prefers_the_daemon_copy(void)
{
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_sent_file_free);
    ClawtLrgSentFile *file = g_new0(ClawtLrgSentFile, 1);
    g_autofree gchar *body = NULL;
    g_autoptr(GPtrArray) refs = NULL;
    g_autoptr(GPtrArray) old = NULL;
    ClawtLrgAttachmentRef *ref;

    file->name = g_strdup("pasted-1.png");
    file->host_path = g_strdup("/host/pasted-1.png");
    file->path = g_strdup("/host/pasted-1.png");
    file->id = g_strdup("feed01");
    file->saved = TRUE;
    g_ptr_array_add(files, file);

    body = clawt_lrg_body_with_attachments("see this", files);

    /* the agent is still told the path it opens */
    g_assert_nonnull(strstr(body, "  /host/pasted-1.png\n"));
    g_assert_nonnull(strstr(body, "  clawt:feed01\n"));

    refs = clawt_lrg_attachment_refs(body);
    g_assert_cmpuint(refs->len, ==, 1);
    ref = g_ptr_array_index(refs, 0);
    g_assert_true(ref->daemon_copy);
    g_assert_cmpstr(ref->value, ==, "feed01");

    old = clawt_lrg_attachment_refs(
        CLAWT_ATTACHMENT_MARKER ".\n- a.png\n  /host/a.png\n"
        "- b.png\n  clawt:b1\n  /host/b.png\n");
    g_assert_cmpuint(old->len, ==, 2);
    g_assert_cmpstr(((ClawtLrgAttachmentRef *)
                     g_ptr_array_index(old, 0))->value, ==, "/host/a.png");
    g_assert_cmpstr(((ClawtLrgAttachmentRef *)
                     g_ptr_array_index(old, 1))->value, ==, "b1");
}

/*
 * A daemon copy's chip read "01m7s9ghwmkxmdzpxa4t-attachment_page.pdf",
 * because the cache keeps it under its id.  Breaks if the id's random
 * prefix comes back in a chip or a "Save a copy" name -- or if a host
 * path's own dashes are taken for one and a name loses its first part.
 */
static void
test_attachment_display_name(void)
{
    g_autofree gchar *cached = NULL;
    g_autofree gchar *host = NULL;
    g_autofree gchar *nocache = NULL;
    g_autofree gchar *refused = NULL;

    cached = clawt_lrg_attachment_display_name(
        "/home/u/.cache/clawtilla/attachments/01m7s9ghwm-attachment_page.pdf",
        "/home/u/.cache/clawtilla/attachments");
    g_assert_cmpstr(cached, ==, "attachment_page.pdf");

    /* an old message's host path: the basename, dashes and all */
    host = clawt_lrg_attachment_display_name(
        "/srv/exchange/alice/my-report-final.pdf",
        "/home/u/.cache/clawtilla/attachments");
    g_assert_cmpstr(host, ==, "my-report-final.pdf");

    /* no cache directory known: nothing is taken for an id */
    nocache = clawt_lrg_attachment_display_name("/tmp/a1-b.png", NULL);
    g_assert_cmpstr(nocache, ==, "a1-b.png");

    /* in the cache but not an id the daemon would make: kept whole */
    refused = clawt_lrg_attachment_display_name(
        "/c/.hidden-name", "/c");
    g_assert_cmpstr(refused, ==, ".hidden-name");
}

/*
 * The Files panel's tiles from attachment.list: newest first as the
 * daemon gives them, "you" for the operator, a gone file said so and
 * not offered a size, an old message's host path as the target, and an
 * item with nothing to open skipped.
 *
 * What breaks without it: the libregnum Files panel draws exactly these
 * rows, so a wrong target opens nothing and a wrong `exists` offers a
 * tile that cannot be opened.
 */
static void
test_gallery_rows(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"room\":\"dm:user:scout\",\"items\":["
        "{\"id\":\"1-paper.pdf\",\"name\":\"paper.pdf\",\"kind\":\"pdf\","
        " \"bytes\":2048,\"exists\":true,\"message\":\"m2\","
        " \"sender\":\"scout\",\"ts\":0},"
        "{\"path\":\"/h/old.png\",\"name\":\"old.png\",\"kind\":\"image\","
        " \"bytes\":0,\"exists\":false,\"message\":\"m1\","
        " \"sender\":\"user\",\"ts\":0},"
        "{\"name\":\"nothing\",\"kind\":\"other\",\"exists\":false}]}");
    g_autoptr(GPtrArray) rows = clawt_lrg_gallery_rows(reply);
    g_autoptr(GPtrArray) empty = clawt_lrg_gallery_rows(NULL);
    ClawtLrgGalleryRow *row;

    g_assert_cmpuint(rows->len, ==, 2);

    row = g_ptr_array_index(rows, 0);
    g_assert_true(row->daemon_copy);
    g_assert_cmpstr(row->target, ==, "1-paper.pdf");
    g_assert_cmpstr(row->kind, ==, "pdf");
    g_assert_true(row->exists);
    g_assert_true(g_str_has_prefix(row->subtitle, "scout"));
    g_assert_nonnull(strstr(row->subtitle, "kB"));

    row = g_ptr_array_index(rows, 1);
    g_assert_false(row->daemon_copy);
    g_assert_cmpstr(row->target, ==, "/h/old.png");
    g_assert_false(row->exists);
    g_assert_true(g_str_has_prefix(row->subtitle, "you"));
    g_assert_nonnull(strstr(row->subtitle, "gone"));

    g_assert_cmpuint(empty->len, ==, 0);
}

/* ── Mentions and conversations ───────────────────────────────────── */

static void
test_mentions(void)
{
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GPtrArray) roster = clawt_lrg_room_roster(rooms, "war");
    g_autoptr(GPtrArray) none = clawt_lrg_room_roster(rooms, "nowhere");
    g_autoptr(GPtrArray) bare = NULL;
    g_autoptr(GPtrArray) a = NULL;
    g_autoptr(GPtrArray) b = NULL;
    g_autoptr(GPtrArray) named = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) with_all = NULL;
    gboolean offer_all = FALSE;

    g_assert_cmpuint(roster->len, ==, 3);
    g_assert_cmpuint(none->len, ==, 0);

    /* a bare `@` offers everybody and @all */
    bare = clawt_lrg_mention_rows("", roster, &offer_all);
    g_assert_cmpuint(bare->len, ==, 3);
    g_assert_true(offer_all);

    /* "a" starts "ann" and "all" */
    a = clawt_lrg_mention_rows("a", roster, &offer_all);
    g_assert_cmpuint(a->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(a, 0), ==, "ann");
    g_assert_true(offer_all);

    /* "b" does not start "all" */
    b = clawt_lrg_mention_rows("b", roster, &offer_all);
    g_assert_cmpstr(g_ptr_array_index(b, 0), ==, "bob");
    g_assert_false(offer_all);

    /* a member literally called "all" is never shadowed by @all */
    g_ptr_array_add(named, g_strdup("all"));
    g_ptr_array_add(named, g_strdup("ann"));
    with_all = clawt_lrg_mention_rows("a", named, &offer_all);
    g_assert_false(offer_all);
}

static void
test_conversations(void)
{
    g_autoptr(JsonNode) rooms = parse(ROOMS_JSON);
    g_autoptr(GPtrArray) ann = clawt_lrg_conversation_peers(rooms, "ann");
    g_autoptr(GPtrArray) chief = clawt_lrg_conversation_peers(rooms, "chief");
    g_autoptr(GPtrArray) bob = clawt_lrg_conversation_peers(rooms, "bob");
    g_autofree gchar *own = clawt_lrg_conversation_label(NULL);
    g_autofree gchar *peer = clawt_lrg_conversation_label("chief");

    /* the operator's own and the group rooms are not peer chats */
    g_assert_cmpuint(ann->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(ann, 0), ==, "chief");
    g_assert_cmpuint(chief->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(chief, 0), ==, "ann");
    g_assert_cmpuint(bob->len, ==, 0);

    g_assert_cmpstr(own, ==, "Chat");
    g_assert_cmpstr(peer, ==, "with chief");
}

static void
test_history_payload(void)
{
    g_autoptr(JsonNode) group = clawt_lrg_history_payload("war", NULL, NULL,
                                                          NULL);
    g_autoptr(JsonNode) own = clawt_lrg_history_payload(NULL, NULL, "ann",
                                                        "5000");
    g_autoptr(JsonNode) peer = clawt_lrg_history_payload(NULL, "chief", "ann",
                                                         NULL);
    JsonObject *o;

    g_assert_null(clawt_lrg_history_payload(NULL, NULL, NULL, NULL));

    o = json_node_get_object(group);
    g_assert_cmpstr(clawt_ipc_payload_string(o, "room"), ==, "war");
    g_assert_cmpstr(clawt_ipc_payload_string(o, "as"), ==, "user");
    g_assert_false(json_object_has_member(o, "limit"));

    o = json_node_get_object(own);
    g_assert_cmpstr(clawt_ipc_payload_string(o, "room"), ==, "ann");
    g_assert_cmpstr(clawt_ipc_payload_string(o, "limit"), ==, "5000");

    /* a peer chat is read as the agent, from the peer's name */
    o = json_node_get_object(peer);
    g_assert_cmpstr(clawt_ipc_payload_string(o, "room"), ==, "chief");
    g_assert_cmpstr(clawt_ipc_payload_string(o, "as"), ==, "ann");
}

/* Only a peer conversation folds anything, and it is the agent's. */
static void
test_fold_viewer(void)
{
    g_assert_cmpstr(clawt_lrg_fold_viewer(NULL, "chief", "ann"), ==, "ann");
    g_assert_null(clawt_lrg_fold_viewer(NULL, NULL, "ann"));
    g_assert_null(clawt_lrg_fold_viewer("war", NULL, NULL));
    g_assert_null(clawt_lrg_fold_viewer("war", "chief", "ann"));
}

static void
test_exchange_payload(void)
{
    g_autoptr(JsonNode) payload = clawt_lrg_exchange_payload("ann", "chief");
    JsonObject *o = json_node_get_object(payload);

    g_assert_null(clawt_lrg_exchange_payload(NULL, "chief"));
    g_assert_null(clawt_lrg_exchange_payload("ann", NULL));

    /* the direct room, read as the agent, from further back */
    g_assert_cmpstr(clawt_ipc_payload_string(o, "room"), ==, "chief");
    g_assert_cmpstr(clawt_ipc_payload_string(o, "as"), ==, "ann");
    g_assert_cmpstr(clawt_ipc_payload_string(o, "limit"), ==, "500");
}

/* --open, against the two replies this client keeps. */
static void
test_open_resolve(void)
{
    g_autoptr(JsonNode) agents = parse(
        "{\"agents\":[{\"id\":\"ann\",\"dm_room\":\"dm:ann:user\"}]}");
    g_autoptr(JsonNode) rooms = parse(
        "{\"rooms\":[{\"id\":\"war\",\"members\":[\"ann\",\"bob\"]},"
        "{\"id\":\"dm:ann:bob\",\"members\":[\"ann\",\"bob\"]}]}");
    g_autoptr(GError) error = NULL;
    g_autofree gchar *agent = NULL;
    g_autofree gchar *peer = NULL;
    g_autofree gchar *room = NULL;

    g_assert_true(clawt_lrg_open_resolve("ann", agents, rooms, &agent, &peer,
                                         &room, &error));
    g_assert_cmpstr(agent, ==, "ann");
    g_clear_pointer(&agent, g_free);

    g_assert_true(clawt_lrg_open_resolve("war", agents, rooms, &agent, &peer,
                                         &room, &error));
    g_assert_cmpstr(room, ==, "war");
    g_clear_pointer(&room, g_free);

    g_assert_true(clawt_lrg_open_resolve("dm:ann:bob", agents, rooms, &agent,
                                         &peer, &room, &error));
    g_assert_cmpstr(agent, ==, "ann");
    g_assert_cmpstr(peer, ==, "bob");
    g_clear_pointer(&agent, g_free);
    g_clear_pointer(&peer, g_free);

    /* nothing called that, and no listing yet: an error, not a crash */
    g_assert_false(clawt_lrg_open_resolve("ghost", agents, rooms, &agent,
                                          &peer, &room, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_false(clawt_lrg_open_resolve("ann", NULL, NULL, &agent, &peer,
                                          &room, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
}

/* ── Decisions inline ─────────────────────────────────────────────── */

/*
 * Two rooms' worth of a `decision.list`-by-room reply: two asked here,
 * one already settled, in the order asked.
 */
#define ROOM_DECISIONS_JSON \
    "{\"decisions\": [" \
    " {\"id\": \"d1\", \"agent\": \"chief\", \"question\": \"Ship?\"," \
    "  \"options\": [\"now\", \"later\"], \"default\": \"later\"," \
    "  \"created_at\": 100, \"state\": 0, \"room\": \"talk-1\"}," \
    " {\"id\": \"d2\", \"agent\": \"chief\", \"question\": \"Free text?\"," \
    "  \"options\": [], \"default\": \"skip\"," \
    "  \"answer\": \"skip\", \"created_at\": 200, \"state\": 2," \
    "  \"room\": \"talk-1\"}]}"

static ClawtEvent *
decision_event(const gchar *kind, const gchar *id, const gchar *room)
{
    ClawtEvent *event = clawt_event_new(kind, "chief");

    clawt_event_set_detail(event, "decision", id);
    clawt_event_set_detail(event, "room", room);

    return event;
}

/*
 * The room's decisions are held in the order asked and placed between
 * messages by the shared rule: strictly before the message.
 */
static void
test_inline_decisions_are_placed(void)
{
    g_autoptr(JsonNode) reply = parse(ROOM_DECISIONS_JSON);
    g_autoptr(GPtrArray) held = clawt_lrg_inline_decisions(reply);
    g_autoptr(GPtrArray) none = clawt_lrg_inline_decisions(NULL);

    g_assert_cmpuint(held->len, ==, 2);
    g_assert_cmpuint(none->len, ==, 0);

    g_assert_cmpuint(clawt_lrg_decisions_before(held, 0, 100), ==, 0);
    g_assert_cmpuint(clawt_lrg_decisions_before(held, 0, 150), ==, 1);
    g_assert_cmpuint(clawt_lrg_decisions_before(held, 1, 201), ==, 2);

    /* buttons only while open, and only with options to press */
    g_assert_true(clawt_lrg_inline_decision_pressable(
        g_ptr_array_index(held, 0)));
    g_assert_false(clawt_lrg_inline_decision_pressable(
        g_ptr_array_index(held, 1)));
}

/*
 * Settled in the inbox -- or anywhere -- the card in the chat redraws
 * with how it ended, from the event alone.  Two rooms, so an event
 * about the other one cannot be mistaken for this one's.
 */
static void
test_inline_decision_follows_the_inbox(void)
{
    g_autoptr(JsonNode) reply = parse(ROOM_DECISIONS_JSON);
    g_autoptr(GPtrArray) held = clawt_lrg_inline_decisions(reply);
    g_autoptr(ClawtEvent) elsewhere =
        decision_event("decision.settled", "d1", "talk-2");
    g_autoptr(ClawtEvent) settled =
        decision_event("decision.settled", "d1", "talk-1");
    g_autoptr(ClawtEvent) asked =
        decision_event("decision.asked", "d3", "talk-1");
    g_autoptr(ClawtEvent) gone =
        decision_event("decision.deleted", "d2", "talk-1");
    g_autofree gchar *open_footer = NULL;
    g_autofree gchar *footer = NULL;

    open_footer = clawt_lrg_inline_decision_footer(g_ptr_array_index(held, 0),
                                                   NULL);
    g_assert_nonnull(strstr(open_footer, "If nobody answers"));

    g_assert_cmpint(clawt_lrg_inline_decision_event(held, "talk-1", elsewhere),
                    ==, CLAWT_LRG_INLINE_NONE);

    clawt_event_set_detail(settled, "how", "answered");
    clawt_event_set_detail(settled, "answer", "now");
    clawt_event_set_detail(settled, "via", CLAWT_DECISION_VIA_INBOX);

    g_assert_cmpint(clawt_lrg_inline_decision_event(held, "talk-1", settled),
                    ==, CLAWT_LRG_INLINE_REDRAW);
    g_assert_false(clawt_lrg_inline_decision_pressable(
        clawt_lrg_inline_decision_find(held, "d1")));

    footer = clawt_lrg_inline_decision_footer(
        clawt_lrg_inline_decision_find(held, "d1"), NULL);
    g_assert_nonnull(strstr(footer, "You answered"));
    g_assert_nonnull(strstr(footer, "in the inbox"));

    /* a replay of the same settle is not news */
    g_assert_cmpint(clawt_lrg_inline_decision_event(held, "talk-1", settled),
                    ==, CLAWT_LRG_INLINE_NONE);

    /* asked here and not held: read the room's list */
    g_assert_cmpint(clawt_lrg_inline_decision_event(held, "talk-1", asked),
                    ==, CLAWT_LRG_INLINE_FETCH);

    /* deleted: dropped from what is held, and the card goes */
    g_assert_cmpint(clawt_lrg_inline_decision_event(held, "talk-1", gone),
                    ==, CLAWT_LRG_INLINE_REMOVE);
    g_assert_null(clawt_lrg_inline_decision_find(held, "d2"));
    g_assert_cmpuint(held->len, ==, 1);

    /* nothing on screen: nothing to change */
    g_assert_cmpint(clawt_lrg_inline_decision_event(held, NULL, asked),
                    ==, CLAWT_LRG_INLINE_NONE);
}

/*
 * A muted agent's pill is drawn (dimmed) but counted nowhere: the Chat
 * tab's number leaves its conversation out, a group room and an unmuted
 * agent still count, and the number still hides on the Chat page.  The
 * muted set is taken from each notice.list and reports whether it moved,
 * which is when the sidebar redraws.  Regressing it is a mute that
 * leaves the tab lit, or a sidebar redrawn on every message.
 */
static void
test_a_muted_pill_is_not_in_the_total(void)
{
    g_autoptr(GHashTable) unread = g_hash_table_new_full(g_str_hash,
                                                         g_str_equal,
                                                         g_free, NULL);
    g_autoptr(GHashTable) muted = g_hash_table_new_full(g_str_hash,
                                                        g_str_equal,
                                                        g_free, NULL);
    g_autoptr(GHashTable) from = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal, g_free,
                                                       NULL);

    g_hash_table_insert(unread, g_strdup("dm:scout:user"),
                        GUINT_TO_POINTER(2));
    g_hash_table_insert(unread, g_strdup("dm:lead:user"),
                        GUINT_TO_POINTER(5));
    g_hash_table_insert(unread, g_strdup("standup"), GUINT_TO_POINTER(1));

    g_assert_cmpuint(clawt_lrg_unread_badge(unread, muted, FALSE), ==, 8);

    g_hash_table_add(from, g_strdup("lead"));
    g_assert_true(clawt_lrg_muted_take(muted, from));
    g_assert_false(clawt_lrg_muted_take(muted, from));
    g_assert_cmpuint(clawt_lrg_unread_badge(unread, muted, FALSE), ==, 3);
    g_assert_cmpuint(clawt_lrg_unread_badge(unread, muted, TRUE), ==, 0);

    /* the row itself still has its count, for the dimmed pill */
    g_assert_cmpuint(clawt_lrg_unread_count(unread, "dm:lead:user"), ==, 5);

    /* unmuted by the next list: counted again */
    g_assert_true(clawt_lrg_muted_take(muted, NULL));
    g_assert_cmpuint(clawt_lrg_unread_badge(unread, muted, FALSE), ==, 8);

    /* a narrowed drawer shows one agent's rows; an open one shows all */
    g_assert_true(clawt_lrg_notice_in_scope("lead", NULL));
    g_assert_true(clawt_lrg_notice_in_scope("lead", "lead"));
    g_assert_false(clawt_lrg_notice_in_scope("scout", "lead"));
    g_assert_false(clawt_lrg_notice_in_scope(NULL, "lead"));
}

gint
main(gint argc, gchar **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg/chat/unread/rows", test_row_rooms);
    g_test_add_func("/lrg/chat/unread/two-rooms", test_unread_two_rooms);
    g_test_add_func("/lrg/chat/unread/prune", test_unread_prune);
    g_test_add_func("/lrg/chat/unread/seed", test_unread_seed);
    g_test_add_func("/lrg/chat/unread/apply-read", test_unread_apply_read);
    g_test_add_func("/lrg/chat/typing/level", test_typing_is_a_level_per_room);
    g_test_add_func("/lrg/chat/typing/forget", test_typing_forget);
    g_test_add_func("/lrg/chat/typing/activity", test_typing_activity);
    g_test_add_func("/lrg/chat/sidebar/agent-row", test_agent_row);
    g_test_add_func("/lrg/chat/sidebar/agent-row-on-fallback",
                    test_agent_row_on_fallback);
    g_test_add_func("/lrg/chat/sidebar/verbs", test_agent_verbs);
    g_test_add_func("/lrg/chat/sidebar/order", test_sidebar_order);
    g_test_add_func("/lrg/chat/sidebar/collapsed", test_sidebar_collapsed);
    g_test_add_func("/lrg/chat/sidebar/empty", test_sidebar_empty_fleet);
    g_test_add_func("/lrg/chat/sidebar/room-labels", test_room_labels);
    g_test_add_func("/lrg/chat/sidebar/reorder", test_reorder);
    g_test_add_func("/lrg/chat/sidebar/team-choices", test_team_choices);
    g_test_add_func("/lrg/chat/sidebar/lifecycle-texts", test_lifecycle_texts);
    g_test_add_func("/lrg/chat/slash/commands", test_slash_commands);
    g_test_add_func("/lrg/chat/slash/split", test_slash_split);
    g_test_add_func("/lrg/chat/slash/texts", test_command_texts);
    g_test_add_func("/lrg/chat/send-note", test_send_note);
    g_test_add_func("/lrg/chat/export-and-chips", test_export_and_chips);
    g_test_add_func("/lrg/chat/runs", test_runs);
    g_test_add_func("/lrg/chat/steps-merge", test_steps_merge);
    g_test_add_func("/lrg/chat/steps/app-link", test_step_app_link);
    g_test_add_func("/lrg/chat/steps/trim-notice", test_step_trim_notice);
    g_test_add_func("/lrg/chat/attachments/refs", test_attachment_refs);
    g_test_add_func("/lrg/chat/attachments/body", test_body_with_attachments);
    g_test_add_func("/lrg/chat/attachments/gallery-rows", test_gallery_rows);
    g_test_add_func("/lrg/chat/attachments/display-name",
                    test_attachment_display_name);
    g_test_add_func("/lrg/chat/attachments/prefers-daemon-copy",
                    test_attachment_prefers_the_daemon_copy);
    g_test_add_func("/lrg/chat/mentions", test_mentions);
    g_test_add_func("/lrg/chat/conversations", test_conversations);
    g_test_add_func("/lrg/chat/history-payload", test_history_payload);
    g_test_add_func("/lrg/chat/fold-viewer", test_fold_viewer);
    g_test_add_func("/lrg/chat/exchange-payload", test_exchange_payload);
    g_test_add_func("/lrg/chat/open-resolve", test_open_resolve);

    g_test_add_func("/lrg/chat/inline-decisions/placed",
                    test_inline_decisions_are_placed);
    g_test_add_func("/lrg/chat/inline-decisions/follow-the-inbox",
                    test_inline_decision_follows_the_inbox);

    g_test_add_func("/lrg-models/chat/muted-pill-not-in-total",
                    test_a_muted_pill_is_not_in_the_total);

    return g_test_run();
}
