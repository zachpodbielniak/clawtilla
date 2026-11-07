/*
 * test-activity.c - Every turn running across the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * activity.list is a fold over state that already exists -- each agent's
 * typing set and each room's step history -- so the tests drive those
 * and read the reply. Two agents, and one of them in two rooms, because
 * a per-agent scalar is the bug this shape has had: a row per turn, the
 * age from the rising edge and not a keepalive, only that turn's own
 * steps, and Steer offered on the operator's room alone.
 *
 * The daemon half includes core/clawt-daemon-private.h, as test-policy
 * does.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

/* ── The library's rules ────────────────────────────────────────── */

static void
test_elapsed_labels(void)
{
    const gint64 base = (gint64)1700000000 * G_USEC_PER_SEC;
    g_autofree gchar *none = clawt_activity_elapsed_label(0, base);
    g_autofree gchar *future = clawt_activity_elapsed_label(base + G_USEC_PER_SEC, base);
    g_autofree gchar *seconds = clawt_activity_elapsed_label(base, base + (gint64)45 * G_USEC_PER_SEC);
    g_autofree gchar *minutes = clawt_activity_elapsed_label(base, base + (gint64)192 * G_USEC_PER_SEC);
    g_autofree gchar *hours = clawt_activity_elapsed_label(base, base + (gint64)3840 * G_USEC_PER_SEC);

    g_assert_cmpstr(none, ==, "just now");
    g_assert_cmpstr(future, ==, "just now");
    g_assert_cmpstr(seconds, ==, "45s");
    g_assert_cmpstr(minutes, ==, "3m 12s");
    g_assert_cmpstr(hours, ==, "1h 04m");
}

/* Stop says it is the whole agent, and how much that is. */
static void
test_stop_prompt(void)
{
    g_autofree gchar *one = clawt_activity_stop_prompt("kudu", 1);
    g_autofree gchar *three = clawt_activity_stop_prompt("kudu", 3);

    g_assert_nonnull(strstr(one, "Stop kudu?"));
    g_assert_nonnull(strstr(one, "not only this turn"));
    g_assert_nonnull(strstr(three, "3 turns running"));
    g_assert_nonnull(strstr(three, "all of them"));
}

/* ── The agent's running set ────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-activity-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "config.yaml", NULL);
    yaml = g_strdup_printf("daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
                           "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n"
                           "agents:\n"
                           "  - id: kudu\n    runtime:\n      autostart: false\n"
                           "  - id: oryx\n    runtime:\n      autostart: false\n",
                           fixture->dir, fixture->dir, fixture->dir, fixture->dir);
    g_assert_true(g_file_set_contents(path, yaml, -1, &error));
    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(path, fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

static JsonNode *
request(DaemonFixture *fixture, const gchar *kind)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

    return clawt_daemon_handle_request(fixture->daemon, frame);
}

static void
note_step(DaemonFixture *fixture, const gchar *agent_id, const gchar *room_id,
          const gchar *command)
{
    g_autoptr(ClawtTurnStep) step = clawt_turn_step_new(
        CLAWT_STEP_TOOL, agent_id, room_id, NULL, "Bash", command, FALSE);

    clawt_daemon_note_step(fixture->daemon, step);
}

/* The turn whose agent and room are these, or NULL. */
static JsonObject *
find_turn(JsonArray *turns, const gchar *agent_id, const gchar *room_id)
{
    guint i;

    for (i = 0; i < json_array_get_length(turns); i++) {
        JsonObject *turn = json_array_get_object_element(turns, i);

        if (g_strcmp0(json_object_get_string_member(turn, "agent"), agent_id) == 0 &&
            g_strcmp0(json_object_get_string_member(turn, "room"), room_id) == 0)
            return turn;
    }

    return NULL;
}

/* Oldest first, the age from the rising edge, and gone on the falling one. */
static void
test_running_rooms_keep_their_start(void)
{
    DaemonFixture fixture = { 0 };
    ClawtAgent *kudu;
    g_autoptr(GPtrArray) rooms = NULL;
    gint64 first;

    daemon_setup(&fixture);
    kudu = clawt_agent_manager_get(fixture.daemon->agents, "kudu");

    clawt_agent_note_typing(kudu, "room-b", TRUE);
    g_usleep(2000);
    clawt_agent_note_typing(kudu, "room-a", TRUE);
    first = clawt_agent_get_turn_started_in(kudu, "room-b");
    g_assert_cmpint(first, >, 0);

    /* A keepalive is the same level again: it does not restart the clock. */
    g_usleep(2000);
    clawt_agent_note_typing(kudu, "room-b", TRUE);
    g_assert_cmpint(clawt_agent_get_turn_started_in(kudu, "room-b"), ==, first);

    rooms = clawt_agent_dup_running_rooms(kudu);
    g_assert_cmpuint(rooms->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(rooms, 0), ==, "room-b");
    g_assert_cmpstr(g_ptr_array_index(rooms, 1), ==, "room-a");

    clawt_agent_note_typing(kudu, "room-b", FALSE);
    g_assert_cmpint(clawt_agent_get_turn_started_in(kudu, "room-b"), ==, 0);
    clawt_agent_clear_typing(kudu);
    g_assert_cmpint(clawt_agent_get_turn_started_in(kudu, "room-a"), ==, 0);

    daemon_teardown(&fixture);
}

/* A row per turn, with only that turn's steps, and Steer on the operator's room. */
static void
test_list_is_per_turn(void)
{
    DaemonFixture fixture = { 0 };
    ClawtAgent *kudu;
    ClawtAgent *oryx;
    ClawtRoom *direct;
    const gchar *direct_id;
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *turns;
    JsonObject *turn;
    guint i;

    daemon_setup(&fixture);
    kudu = clawt_agent_manager_get(fixture.daemon->agents, "kudu");
    oryx = clawt_agent_manager_get(fixture.daemon->agents, "oryx");
    direct = clawt_room_manager_get_direct(fixture.daemon->rooms, "user", "kudu");
    direct_id = clawt_room_get_id(direct);

    /* Before the turn: history, not this turn. */
    note_step(&fixture, "kudu", direct_id, "echo before");
    g_usleep(2000);

    clawt_agent_note_typing(kudu, direct_id, TRUE);
    clawt_agent_note_typing(kudu, "peer-room", TRUE);
    clawt_agent_note_typing(oryx, "peer-room", TRUE);

    for (i = 0; i < CLAWT_ACTIVITY_STEPS + 2; i++) {
        g_autofree gchar *command = g_strdup_printf("echo %u", i);

        note_step(&fixture, "kudu", direct_id, command);
    }
    note_step(&fixture, "oryx", "peer-room", "ls oryx");
    note_step(&fixture, "kudu", "peer-room", "ls kudu");

    reply = request(&fixture, "activity.list");
    turns = json_object_get_array_member(clawt_ipc_frame_get_payload(reply), "turns");
    g_assert_cmpuint(json_array_get_length(turns), ==, 3);

    /* kudu with the operator: steerable, capped, and nothing from before. */
    turn = find_turn(turns, "kudu", direct_id);
    g_assert_nonnull(turn);
    g_assert_true(json_object_get_boolean_member(turn, "steerable"));
    g_assert_cmpint(json_object_get_int_member(turn, "running_turns"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(turn, "step_count"), ==, CLAWT_ACTIVITY_STEPS + 2);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(turn, "steps")),
                     ==, CLAWT_ACTIVITY_STEPS);
    {
        g_autofree gchar *text = json_to_string(json_object_get_member(turn, "steps"), FALSE);

        g_assert_null(strstr(text, "echo before"));
        g_assert_null(strstr(text, "ls kudu"));
        g_assert_nonnull(strstr(text, "echo 7"));
    }

    /* Two agents in one room are two turns, each with its own steps. */
    turn = find_turn(turns, "kudu", "peer-room");
    g_assert_nonnull(turn);
    g_assert_false(json_object_get_boolean_member(turn, "steerable"));
    g_assert_cmpint(json_object_get_int_member(turn, "step_count"), ==, 1);

    turn = find_turn(turns, "oryx", "peer-room");
    g_assert_nonnull(turn);
    g_assert_cmpint(json_object_get_int_member(turn, "running_turns"), ==, 1);
    {
        g_autofree gchar *text = json_to_string(json_object_get_member(turn, "steps"), FALSE);

        g_assert_nonnull(strstr(text, "ls oryx"));
        g_assert_null(strstr(text, "ls kudu"));
    }

    /* A turn that ends is gone from the next listing. */
    clawt_agent_note_typing(oryx, "peer-room", FALSE);
    g_clear_pointer(&reply, json_node_unref);
    reply = request(&fixture, "activity.list");
    turns = json_object_get_array_member(clawt_ipc_frame_get_payload(reply), "turns");
    g_assert_cmpuint(json_array_get_length(turns), ==, 2);
    g_assert_null(find_turn(turns, "oryx", "peer-room"));

    daemon_teardown(&fixture);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/activity/elapsed-labels", test_elapsed_labels);
    g_test_add_func("/activity/stop-prompt", test_stop_prompt);
    g_test_add_func("/activity/running-rooms-keep-their-start",
                    test_running_rooms_keep_their_start);
    g_test_add_func("/activity/list-is-per-turn", test_list_is_per_turn);

    return g_test_run();
}
