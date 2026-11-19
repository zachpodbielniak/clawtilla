/*
 * test-read-store.c - Where the operator has read up to, held by the daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two halves.  The store on its own: a mark survives a reopen, never
 * moves backwards, and knows whether this open made the file.  And the
 * daemon around it, driven through clawt_daemon_handle_request() in a
 * private state directory: the first open seeds every room so an upgrade
 * lights nothing, `room.read` defaults to the newest message, and
 * `all:true` covers every room.  Nothing here reaches the real fleet.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

/* ── The store ────────────────────────────────────────────────────── */

/*
 * A mark written, the store closed and opened again, the mark read back
 * -- and the second open knows it did not make the file.  Regressing the
 * first half is unread counts that come back after every restart; the
 * second is the daemon re-seeding (marking everything read) on every
 * start.
 */
static void
test_a_mark_survives_a_reopen(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-reads-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "state", "reads.db", NULL);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    gint64 ts = 0;

    {
        g_autoptr(ClawtReadStore) store = clawt_read_store_new(path, &error);

        g_assert_no_error(error);
        g_assert_true(clawt_read_store_was_created(store));
        g_assert_false(clawt_read_store_get(store, "dm:alpha:user", &id,
                                            &ts));
        g_assert_null(id);
        g_assert_cmpint(ts, ==, 0);

        g_assert_true(clawt_read_store_set(store, "dm:alpha:user", "m7", 70,
                                           &error));
        g_assert_no_error(error);
    }

    {
        g_autoptr(ClawtReadStore) store = clawt_read_store_new(path, &error);

        g_assert_no_error(error);
        g_assert_false(clawt_read_store_was_created(store));
        g_assert_true(clawt_read_store_get(store, "dm:alpha:user", &id,
                                           &ts));
        g_assert_cmpstr(id, ==, "m7");
        g_assert_cmpint(ts, ==, 70);
        g_assert_cmpuint(clawt_read_store_count(store), ==, 1);
    }

    clawt_test_remove_tree(dir);
}

/*
 * Two rooms, each with its own mark, and a write replacing the mark it
 * names and no other.  Regressing it -- a mark kept as one shared value
 * -- reads one conversation and clears them all.
 */
static void
test_marks_are_per_room(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-reads-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "reads.db", NULL);
    g_autoptr(ClawtReadStore) store = clawt_read_store_new(path, NULL);
    g_autofree gchar *id = NULL;
    gint64 ts = 0;

    g_assert_nonnull(store);
    g_assert_true(clawt_read_store_set(store, "a", "m5", 50, NULL));
    g_assert_true(clawt_read_store_set(store, "b", "m2", 20, NULL));
    g_assert_true(clawt_read_store_set(store, "a", "m6", 60, NULL));

    clawt_read_store_get(store, "a", &id, &ts);
    g_assert_cmpstr(id, ==, "m6");
    g_assert_cmpint(ts, ==, 60);
    g_clear_pointer(&id, g_free);

    clawt_read_store_get(store, "b", &id, &ts);
    g_assert_cmpstr(id, ==, "m2");
    g_assert_cmpint(ts, ==, 20);
    g_assert_cmpuint(clawt_read_store_count(store), ==, 2);

    g_assert_false(clawt_read_store_set(store, NULL, "m1", 1, NULL));

    g_clear_object(&store);
    clawt_test_remove_tree(dir);
}

/* ── The daemon around it ─────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} Fixture;

static void
fixture_start(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);
}

/*
 * Two agents and a group room, everything under a private directory --
 * the socket, the state, the workspaces -- so nothing here touches a
 * fleet somebody is running.
 */
static void
fixture_setup(Fixture *fixture)
{
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-reads-d-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "skills:\n  dir: \"%s/skills\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n  - id: alpha\n  - id: zed\n"
        "rooms:\n"
        "  - id: standup\n"
        "    members: [alpha, zed]\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->dir);

    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1, NULL));

    fixture_start(fixture);
}

static void
fixture_stop(Fixture *fixture)
{
    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    g_clear_pointer(&fixture->context, g_main_context_unref);
}

static void
fixture_teardown(Fixture *fixture)
{
    fixture_stop(fixture);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
    g_free(fixture->config_path);
}

static JsonNode *
request(Fixture *fixture, const gchar *kind, const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "r1");

    if (payload_json != NULL) {
        g_autoptr(JsonParser) parser = json_parser_new();

        g_assert_true(json_parser_load_from_data(parser, payload_json, -1,
                                                 NULL));
        clawt_ipc_frame_set_payload(
            frame, json_node_copy(json_parser_get_root(parser)));
    }

    return clawt_daemon_handle_request(fixture->daemon, frame);
}

/*
 * A message in a room, through the transcript, as a turn ending would
 * leave it.  A direct room is reached through the manager's own naming,
 * `dm:a:b` taken apart into its two members.
 */
static const gchar *
say(Fixture *fixture, const gchar *room_id, const gchar *sender)
{
    ClawtRoom *room = clawt_room_manager_get(fixture->daemon->rooms,
                                             room_id);
    g_autoptr(ClawtMessage) message = NULL;
    const gchar *id = NULL;

    if (room == NULL && g_str_has_prefix(room_id, "dm:")) {
        g_auto(GStrv) parts = g_strsplit(room_id, ":", 3);

        room = clawt_room_manager_get_direct(fixture->daemon->rooms,
                                             parts[1], parts[2]);
    }

    g_assert_nonnull(room);

    message = clawt_message_new(clawt_room_get_id(room), sender, "hello");
    g_assert_true(clawt_room_append(room, message, NULL));
    clawt_room_get_latest(room, &id, NULL);

    return id;
}

/* The `unread` room.list reports for @room_id, or -1 when it is absent. */
static gint64
listed_unread(Fixture *fixture, const gchar *room_id)
{
    g_autoptr(JsonNode) reply = request(fixture, "room.list", NULL);
    JsonArray *rooms = json_object_get_array_member(
        clawt_ipc_frame_get_payload(reply), "rooms");
    guint i;

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);

        if (g_strcmp0(json_object_get_string_member(room, "id"),
                      room_id) == 0)
            return json_object_has_member(room, "unread")
                ? json_object_get_int_member(room, "unread") : -1;
    }

    return -1;
}

/*
 * An upgrade: conversations already on disk, and no reads.db yet.  The
 * first daemon to open the store marks every room read at its newest
 * message, so nothing lights up; a room created afterwards has no mark
 * and counts everything from somebody else.  Regressing the seed greets
 * the operator with every message the fleet ever sent as unread.
 */
static void
test_the_first_open_seeds_every_room(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *reads = NULL;

    fixture_setup(&fixture);

    say(&fixture, "dm:user:zed", "zed");
    say(&fixture, "dm:user:zed", "zed");
    say(&fixture, "standup", "alpha");
    fixture_stop(&fixture);

    /* what a build before read marks would have left behind */
    reads = g_build_filename(fixture.dir, "state", "reads.db", NULL);
    g_assert_cmpint(g_unlink(reads), ==, 0);

    fixture_start(&fixture);
    g_assert_true(clawt_read_store_was_created(fixture.daemon->reads));
    g_assert_cmpint(listed_unread(&fixture, "dm:user:zed"), ==, 0);
    g_assert_cmpint(listed_unread(&fixture, "standup"), ==, 0);

    /* a room with no mark: everything not the operator's */
    say(&fixture, "dm:alpha:user", "alpha");
    say(&fixture, "dm:alpha:user", "user");
    say(&fixture, "dm:alpha:user", "alpha");
    g_assert_cmpint(listed_unread(&fixture, "dm:alpha:user"), ==, 2);

    fixture_teardown(&fixture);
}

/*
 * `room.read` with no `upto` marks the newest message, and a named
 * `upto` must be a message the room holds.  Regressing the default
 * leaves the last reply unread after "Mark as read"; regressing the
 * check lets a client set a mark that swallows messages.
 */
static void
test_room_read_defaults_to_the_newest(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *first = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autofree gchar *upto = NULL;

    fixture_setup(&fixture);

    first = g_strdup(say(&fixture, "standup", "alpha"));
    say(&fixture, "standup", "zed");
    say(&fixture, "standup", "alpha");
    g_assert_cmpint(listed_unread(&fixture, "standup"), ==, 3);

    upto = g_strdup_printf("{\"room\":\"standup\",\"upto\":\"%s\"}", first);
    reply = request(&fixture, "room.read", upto);
    g_assert_false(clawt_ipc_frame_is_error(reply));
    g_assert_cmpint(json_object_get_int_member(
                        clawt_ipc_frame_get_payload(reply), "unread"),
                    ==, 2);
    g_clear_pointer(&reply, json_node_unref);

    reply = request(&fixture, "room.read", "{\"room\":\"standup\"}");
    g_assert_false(clawt_ipc_frame_is_error(reply));
    g_assert_cmpint(listed_unread(&fixture, "standup"), ==, 0);

    refused = request(&fixture, "room.read",
                      "{\"room\":\"standup\",\"upto\":\"no-such\"}");
    g_assert_true(clawt_ipc_frame_is_error(refused));

    fixture_teardown(&fixture);
}

/*
 * Two clients read the same room and their requests land in either
 * order: the one that read less, arriving second, must not undo the one
 * that read more.  Three messages made in the same instant, so ids that
 * sort only to the millisecond cannot be what decides it.  Regressing
 * it lights a conversation up again because a second window caught up
 * late.
 */
static void
test_room_read_never_moves_back(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *first = NULL;
    g_autofree gchar *upto = NULL;
    g_autoptr(JsonNode) newest = NULL;
    g_autoptr(JsonNode) late = NULL;

    fixture_setup(&fixture);

    first = g_strdup(say(&fixture, "standup", "alpha"));
    say(&fixture, "standup", "zed");
    say(&fixture, "standup", "alpha");

    newest = request(&fixture, "room.read", "{\"room\":\"standup\"}");
    g_assert_false(clawt_ipc_frame_is_error(newest));

    upto = g_strdup_printf("{\"room\":\"standup\",\"upto\":\"%s\"}",
                           first);
    late = request(&fixture, "room.read", upto);
    g_assert_false(clawt_ipc_frame_is_error(late));
    g_assert_cmpint(json_object_get_int_member(
                        clawt_ipc_frame_get_payload(late), "unread"),
                    ==, 0);
    g_assert_cmpint(listed_unread(&fixture, "standup"), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * `all:true` reads every room with anything unread, and says how many.
 * Two rooms, so marking only the first would show.  Regressing it is a
 * "Mark all as read" that leaves badges behind.
 */
static void
test_room_read_all_covers_every_room(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;

    fixture_setup(&fixture);

    say(&fixture, "standup", "alpha");
    say(&fixture, "dm:user:zed", "zed");
    g_assert_cmpint(listed_unread(&fixture, "standup"), ==, 1);
    g_assert_cmpint(listed_unread(&fixture, "dm:user:zed"), ==, 1);

    reply = request(&fixture, "room.read", "{\"all\":true}");
    g_assert_false(clawt_ipc_frame_is_error(reply));
    g_assert_cmpint(json_object_get_int_member(
                        clawt_ipc_frame_get_payload(reply), "marked"),
                    ==, 2);
    g_assert_cmpint(listed_unread(&fixture, "standup"), ==, 0);
    g_assert_cmpint(listed_unread(&fixture, "dm:user:zed"), ==, 0);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/read-store/reopen", test_a_mark_survives_a_reopen);
    g_test_add_func("/read-store/per-room", test_marks_are_per_room);
    g_test_add_func("/read-store/daemon/seeds-on-first-open",
                    test_the_first_open_seeds_every_room);
    g_test_add_func("/read-store/daemon/defaults-to-newest",
                    test_room_read_defaults_to_the_newest);
    g_test_add_func("/read-store/daemon/never-backwards",
                    test_room_read_never_moves_back);
    g_test_add_func("/read-store/daemon/all",
                    test_room_read_all_covers_every_room);

    return g_test_run();
}
