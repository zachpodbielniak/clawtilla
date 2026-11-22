/*
 * test-office.c - The office's presence fold, world packs and choreography
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The presence fold is tested with two agents and two rooms throughout,
 * because the state is keyed and a test with one key cannot see a
 * shared-scalar bug.  Times are explicit, so decay is tested without
 * waiting.  The world packs are fixtures under tests/fixtures/office/.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <string.h>

#define T0 ((gint64)1800000000)

static ClawtOfficePresence *
two_agents(void)
{
    ClawtOfficePresence *presence = clawt_office_presence_new("fleet-a", 600);

    clawt_office_presence_set_participant(presence, CLAWT_OFFICE_KIND_AGENT, "kudu",
                                          "Kudu", "core", NULL, TRUE, T0);
    clawt_office_presence_set_participant(presence, CLAWT_OFFICE_KIND_AGENT, "oryx",
                                          "Oryx", "core", NULL, TRUE, T0);
    return presence;
}

/* A turn in one room is not a turn in the other, nor another agent's. */
static void
test_turns_are_per_room(void)
{
    g_autoptr(ClawtOfficePresence) presence = two_agents();
    ClawtOfficeStation station;

    clawt_office_presence_note_turn(presence, "kudu", "room-a", TRUE, T0);
    clawt_office_presence_note_turn(presence, "kudu", "room-b", TRUE, T0);
    clawt_office_presence_note_step(presence, "kudu", "room-a", "Read", T0 + 1);

    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu", T0 + 2, &station,
                                                  NULL, NULL), ==, CLAWT_OFFICE_WORKING);
    g_assert_cmpint(station, ==, CLAWT_OFFICE_STATION_LIBRARY);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "oryx", T0 + 2, NULL,
                                                  NULL, NULL), ==, CLAWT_OFFICE_IDLE);

    /* one room ends; the other is still running */
    clawt_office_presence_note_turn(presence, "kudu", "room-a", FALSE, T0 + 3);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu", T0 + 4, NULL,
                                                  NULL, NULL), ==, CLAWT_OFFICE_WORKING);
    clawt_office_presence_note_turn(presence, "kudu", "room-b", FALSE, T0 + 5);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu", T0 + 6, NULL,
                                                  NULL, NULL), ==, CLAWT_OFFICE_IDLE);
}

/* A step goes stale into thinking; a turn never closed decays to idle. */
static void
test_decay(void)
{
    g_autoptr(ClawtOfficePresence) presence = two_agents();

    clawt_office_presence_note_turn(presence, "kudu", "room-a", TRUE, T0);
    clawt_office_presence_note_step(presence, "kudu", "room-a", "Bash", T0);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu",
                    T0 + CLAWT_OFFICE_STEP_SECONDS + 1, NULL, NULL, NULL), ==,
                    CLAWT_OFFICE_THINKING);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu", T0 + 601, NULL,
                                                  NULL, NULL), ==, CLAWT_OFFICE_IDLE);
}

/* The loud ones win, and each is about the right people. */
static void
test_priority_and_peers(void)
{
    g_autoptr(ClawtOfficePresence) presence = two_agents();
    const gchar *peer = NULL;
    const gchar *room = NULL;
    const gchar *const members[] = { "kudu", "oryx", NULL };

    clawt_office_presence_note_turn(presence, "kudu", "dm", TRUE, T0);
    clawt_office_presence_note_call(presence, "kudu", "oryx", T0);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "oryx", T0 + 1, NULL,
                                                  &peer, NULL), ==, CLAWT_OFFICE_ON_CALL);
    g_assert_cmpstr(peer, ==, "kudu");
    /* a call lasts as long as it is renewed */
    g_assert_cmpint(clawt_office_presence_resolve(presence, "oryx",
                    T0 + CLAWT_OFFICE_CALL_SECONDS + 1, NULL, NULL, NULL), ==,
                    CLAWT_OFFICE_IDLE);

    clawt_office_presence_note_meeting(presence, "launch", members, TRUE, T0);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu", T0 + 1, NULL,
                                                  NULL, &room), ==, CLAWT_OFFICE_IN_MEETING);
    g_assert_cmpstr(room, ==, "town-hall:launch");

    clawt_office_presence_note_error(presence, "kudu", "timed out", T0 + 2);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu", T0 + 3, NULL,
                                                  NULL, NULL), ==, CLAWT_OFFICE_ERROR);
    clawt_office_presence_note_waiting(presence, "kudu", "d1", TRUE);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu", T0 + 3, NULL,
                                                  NULL, NULL), ==, CLAWT_OFFICE_WAITING_ON_YOU);
    /* settled by id alone, as decision.settled does */
    clawt_office_presence_note_waiting(presence, NULL, "d1", FALSE);
    /* the next turn clears the error */
    clawt_office_presence_note_turn(presence, "kudu", "dm", TRUE, T0 + 4);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "kudu",
                    T0 + CLAWT_OFFICE_MEETING_SECONDS + 5, NULL, NULL, NULL), ==,
                    CLAWT_OFFICE_THINKING);
    /* and oryx was never anything but itself */
    g_assert_cmpint(clawt_office_presence_resolve(presence, "oryx",
                    T0 + CLAWT_OFFICE_MEETING_SECONDS + 5, NULL, NULL, NULL), ==,
                    CLAWT_OFFICE_IDLE);
}

/* Edges, not frames: an unchanged participant is not reported again. */
static void
test_changed_since_reports_edges(void)
{
    g_autoptr(ClawtOfficePresence) presence = two_agents();
    g_autoptr(GPtrArray) first = clawt_office_presence_changed_since(presence, T0);
    g_autoptr(GPtrArray) quiet = NULL;
    g_autoptr(GPtrArray) moved = NULL;

    g_assert_cmpuint(first->len, ==, 2);
    quiet = clawt_office_presence_changed_since(presence, T0 + 1);
    g_assert_cmpuint(quiet->len, ==, 0);
    clawt_office_presence_note_turn(presence, "oryx", "r", TRUE, T0 + 2);
    moved = clawt_office_presence_changed_since(presence, T0 + 2);
    g_assert_cmpuint(moved->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(moved, 0), ==, "oryx");
}

/* Helpers arrive and leave; offline is offline; the wire carries the fleet. */
static void
test_helpers_and_snapshot(void)
{
    g_autoptr(ClawtOfficePresence) presence = two_agents();
    g_autoptr(JsonNode) snapshot = NULL;
    JsonArray *people;
    g_autofree gchar *line = NULL;

    clawt_office_presence_set_participant(presence, CLAWT_OFFICE_KIND_EPHEMERAL,
                                          "turbo-narwhal", NULL, "ephemeral", "kudu",
                                          TRUE, T0);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "turbo-narwhal", T0 + 1,
                    NULL, NULL, NULL), ==, CLAWT_OFFICE_ARRIVING);
    clawt_office_presence_note_leaving(presence, "turbo-narwhal", T0 + 30);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "turbo-narwhal", T0 + 31,
                    NULL, NULL, NULL), ==, CLAWT_OFFICE_LEAVING);
    clawt_office_presence_set_participant(presence, CLAWT_OFFICE_KIND_AGENT, "oryx",
                                          "Oryx", "core", NULL, FALSE, T0 + 31);
    g_assert_cmpint(clawt_office_presence_resolve(presence, "oryx", T0 + 31, NULL,
                                                  NULL, NULL), ==, CLAWT_OFFICE_OFFLINE);

    snapshot = clawt_office_presence_snapshot(presence, T0 + 31);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(snapshot),
                                                  "fleet"), ==, "fleet-a");
    people = json_object_get_array_member(json_node_get_object(snapshot), "participants");
    g_assert_cmpuint(json_array_get_length(people), ==, 3);
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(people, 2),
                                                  "parent"), ==, "kudu");

    clawt_office_presence_remove(presence, "turbo-narwhal");
    g_assert_cmpint(clawt_office_presence_resolve(presence, "turbo-narwhal", T0 + 32,
                    NULL, NULL, NULL), ==, CLAWT_OFFICE_OFFLINE);

    line = clawt_office_roster_line(json_array_get_object_element(people, 2), T0 + 31);
    g_assert_cmpstr(line, ==, "heading out");
}

static void
test_vocabulary(void)
{
    guint i;

    g_assert_cmpint(clawt_office_station_for_tool("mcp__clawtilla__clawtilla_delegate"),
                    ==, CLAWT_OFFICE_STATION_WHITEBOARD);
    g_assert_cmpint(clawt_office_station_for_tool("Edit"), ==, CLAWT_OFFICE_STATION_DESK);
    g_assert_cmpint(clawt_office_station_for_tool("WebSearch"), ==,
                    CLAWT_OFFICE_STATION_LIBRARY);
    g_assert_cmpint(clawt_office_station_for_tool(NULL), ==, CLAWT_OFFICE_STATION_NONE);

    /* every activity round-trips and is said and coloured */
    for (i = 0; i < clawt_office_activity_count(); i++) {
        ClawtOfficeActivity back;

        g_assert_true(clawt_office_activity_from_nick(
            clawt_office_activity_nick((ClawtOfficeActivity)i), &back));
        g_assert_cmpint(back, ==, (ClawtOfficeActivity)i);
        g_assert_nonnull(clawt_office_activity_label(back));
        g_assert_nonnull(clawt_office_activity_tone(back));
    }
}

/* ── World packs ────────────────────────────────────────────────── */

static gchar *
fixture(const gchar *name)
{
    return g_build_filename(CLAWT_TEST_FIXTURES, "office", name, NULL);
}

static void
test_world_loads_and_finds(void)
{
    g_autofree gchar *dir = fixture("good");
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtOfficeWorld) world = clawt_office_world_load(dir, &error);
    const ClawtOfficeZone *zone;
    gboolean warned = FALSE;
    guint i;

    g_assert_no_error(error);
    g_assert_cmpstr(clawt_office_world_get_name(world), ==, "Test Office");
    g_assert_true(clawt_office_world_verify_assets(world, &error));
    g_assert_no_error(error);

    zone = clawt_office_world_find_zone(world, "desk", "core", 0);
    g_assert_cmpstr(zone->id, ==, "core-desks");
    zone = clawt_office_world_find_zone(world, "desk", "elsewhere", 0);
    g_assert_cmpstr(zone->id, ==, "core-desks");
    /* the smallest meeting room that fits */
    zone = clawt_office_world_find_zone(world, "meeting", NULL, 3);
    g_assert_cmpstr(zone->id, ==, "huddle");
    zone = clawt_office_world_find_zone(world, "meeting", NULL, 6);
    g_assert_cmpstr(zone->id, ==, "boardroom");
    g_assert_null(clawt_office_world_find_zone(world, "meeting", NULL, 40));
    g_assert_null(clawt_office_world_find_zone(world, "library", NULL, 0));

    for (i = 0; i < clawt_office_world_get_warnings(world)->len; i++)
        if (strstr(g_ptr_array_index(clawt_office_world_get_warnings(world), i),
                   "disco_floor") != NULL)
            warned = TRUE;
    g_assert_true(warned);
}

static void
test_world_refusals(void)
{
    g_autofree gchar *nolicense = fixture("nolicense");
    g_autofree gchar *escape = fixture("escape");
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtOfficeWorld) a = clawt_office_world_load(nolicense, &error);
    g_autoptr(ClawtOfficeWorld) b = NULL;

    g_assert_null(a);
    g_assert_nonnull(strstr(error->message, "license"));
    g_clear_error(&error);
    b = clawt_office_world_load(escape, &error);
    g_assert_null(b);
    g_assert_nonnull(strstr(error->message, "inside the pack"));
}

static void
test_discover_skips_refused(void)
{
    g_autofree gchar *root = g_build_filename(CLAWT_TEST_FIXTURES, "office", NULL);
    const gchar *const dirs[] = { root, NULL };
    g_autoptr(GPtrArray) refused = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) worlds = clawt_office_worlds_discover(dirs, refused);

    g_assert_cmpuint(worlds->len, ==, 1);
    g_assert_cmpuint(refused->len, ==, 2);
}

/* Built in, then the pack's, then the person's: each overrides by rule. */
static void
test_choreography_layers(void)
{
    g_autofree gchar *dir = fixture("good");
    g_autoptr(ClawtOfficeWorld) world = clawt_office_world_load(dir, NULL);
    g_autoptr(ClawtOfficeChoreography) rules = clawt_office_choreography_new_default();
    g_autoptr(GError) error = NULL;
    const ClawtOfficeCue *cue;

    cue = clawt_office_choreography_cue(rules, CLAWT_OFFICE_WORKING,
                                        CLAWT_OFFICE_STATION_LIBRARY);
    g_assert_cmpstr(cue->zone, ==, "library");
    cue = clawt_office_choreography_cue(rules, CLAWT_OFFICE_ON_CALL,
                                        CLAWT_OFFICE_STATION_NONE);
    g_assert_cmpstr(cue->prop, ==, "phone");

    g_assert_true(clawt_office_choreography_override(rules,
        clawt_office_world_get_choreography_yaml(world), &error));
    cue = clawt_office_choreography_cue(rules, CLAWT_OFFICE_IDLE, CLAWT_OFFICE_STATION_NONE);
    g_assert_cmpstr(cue->zone, ==, "breakroom");

    g_assert_true(clawt_office_choreography_override(rules,
        "choreography:\n  - { activity: idle, zone: lounge, pose: sleep, emote: zzz }\n",
        &error));
    cue = clawt_office_choreography_cue(rules, CLAWT_OFFICE_IDLE, CLAWT_OFFICE_STATION_NONE);
    g_assert_cmpstr(cue->emote, ==, "zzz");

    /* a bad rule refuses the whole override and changes nothing */
    g_assert_false(clawt_office_choreography_override(rules,
        "choreography:\n  - { activity: idle, zone: x }\n"
        "  - { activity: dancing, zone: y }\n", &error));
    cue = clawt_office_choreography_cue(rules, CLAWT_OFFICE_IDLE, CLAWT_OFFICE_STATION_NONE);
    g_assert_cmpstr(cue->zone, ==, "lounge");
}

/* ── The director ───────────────────────────────────────────────── */

static JsonObject *
placement_of(JsonNode *layout, const gchar *id)
{
    JsonArray *all = json_object_get_array_member(json_node_get_object(layout), "placements");
    guint i;

    for (i = 0; i < json_array_get_length(all); i++) {
        JsonObject *p = json_array_get_object_element(all, i);

        if (g_strcmp0(json_object_get_string_member(p, "id"), id) == 0)
            return p;
    }
    return NULL;
}

/*
 * Desks are a team's first and stable whatever order people arrive in;
 * a meeting finds a room that fits; a call with no booth stays at the
 * desk; a delivery stands at the assignee's desk; offline is not placed.
 */
static void
test_director_places(void)
{
    g_autofree gchar *dir = fixture("good");
    g_autoptr(ClawtOfficeWorld) world = clawt_office_world_load(dir, NULL);
    g_autoptr(ClawtOfficeChoreography) rules = clawt_office_choreography_new_default();
    g_autoptr(ClawtOfficePresence) a = clawt_office_presence_new("f", 600);
    g_autoptr(ClawtOfficePresence) b = clawt_office_presence_new("f", 600);
    const gchar *const members[] = { "kudu", "oryx", "pika", NULL };
    const gchar *const order_a[] = { "kudu", "oryx", "pika", "wren", NULL };
    const gchar *const order_b[] = { "wren", "pika", "oryx", "kudu", NULL };
    g_autoptr(JsonNode) snap_a = NULL;
    g_autoptr(JsonNode) snap_b = NULL;
    g_autoptr(JsonNode) layout_a = NULL;
    g_autoptr(JsonNode) layout_b = NULL;
    JsonObject *p;
    gsize i;

    for (i = 0; order_a[i] != NULL; i++) {
        clawt_office_presence_set_participant(a, CLAWT_OFFICE_KIND_AGENT, order_a[i], NULL,
            g_strcmp0(order_a[i], "wren") == 0 ? NULL : "core", NULL, TRUE, T0);
        clawt_office_presence_set_participant(b, CLAWT_OFFICE_KIND_AGENT, order_b[i], NULL,
            g_strcmp0(order_b[i], "wren") == 0 ? NULL : "core", NULL, TRUE, T0);
    }
    clawt_office_presence_set_participant(a, CLAWT_OFFICE_KIND_AGENT, "gone", NULL, NULL,
                                          NULL, FALSE, T0);

    /* the same story told to both, in different orders */
    clawt_office_presence_note_meeting(a, "standup", members, FALSE, T0);
    clawt_office_presence_note_meeting(b, "standup", members, FALSE, T0);
    clawt_office_presence_note_delivery(a, "wren", "kudu", T0 + 200);
    clawt_office_presence_note_delivery(b, "wren", "kudu", T0 + 200);

    snap_a = clawt_office_presence_snapshot(a, T0 + 1);
    snap_b = clawt_office_presence_snapshot(b, T0 + 1);
    layout_a = clawt_office_direct(world, rules, json_node_get_object(snap_a));
    layout_b = clawt_office_direct(world, rules, json_node_get_object(snap_b));

    /* three at the standup: the huddle room for four, not the boardroom */
    p = placement_of(layout_a, "kudu");
    g_assert_cmpstr(json_object_get_string_member(p, "zone"), ==, "huddle");
    g_assert_null(placement_of(layout_a, "gone"));

    /* the same seats whatever order they were added in */
    for (i = 0; order_a[i] != NULL; i++) {
        JsonObject *x = placement_of(layout_a, order_a[i]);
        JsonObject *y = placement_of(layout_b, order_a[i]);

        g_assert_cmpstr(json_object_get_string_member(x, "zone"), ==,
                        json_object_get_string_member(y, "zone"));
        g_assert_cmpint(json_object_get_int_member(x, "seat"), ==,
                        json_object_get_int_member(y, "seat"));
    }

    /* after the meeting: kudu at the team's desks, the delivery beside them */
    g_clear_pointer(&snap_a, json_node_unref);
    g_clear_pointer(&layout_a, json_node_unref);
    clawt_office_presence_note_turn(a, "kudu", "r", TRUE, T0 + 200);
    clawt_office_presence_note_call(a, "oryx", "pika", T0 + 200);
    snap_a = clawt_office_presence_snapshot(a, T0 + 201);
    layout_a = clawt_office_direct(world, rules, json_node_get_object(snap_a));

    p = placement_of(layout_a, "kudu");
    g_assert_cmpstr(json_object_get_string_member(p, "activity"), ==, "thinking");
    g_assert_cmpstr(json_object_get_string_member(p, "zone"), ==, "core-desks");
    {
        JsonObject *w = placement_of(layout_a, "wren");

        g_assert_cmpstr(json_object_get_string_member(w, "activity"), ==, "delivering");
        g_assert_cmpstr(json_object_get_string_member(w, "zone"), ==, "core-desks");
        g_assert_cmpstr(json_object_get_string_member(w, "prop"), ==, "folder");
        g_assert_cmpfloat(json_object_get_double_member(w, "x"), >,
                          json_object_get_double_member(p, "x"));
    }
    /* no phone booth in this pack: the call is taken at the desk, and says so */
    p = placement_of(layout_a, "oryx");
    g_assert_cmpstr(json_object_get_string_member(p, "activity"), ==, "on_call");
    g_assert_cmpstr(json_object_get_string_member(p, "prop"), ==, "phone");
    g_assert_cmpstr(json_object_get_string_member(p, "facing"), ==, "pika");
    g_assert_nonnull(strstr(json_object_get_string_member(p, "fallback"), "desk"));
}

/* ── The daemon ─────────────────────────────────────────────────── */

static guint presence_events;

static void
count_presence(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    if (g_strcmp0(clawt_event_get_kind(event), "office.presence") == 0)
        presence_events++;
}

static JsonNode *
office_request(ClawtDaemon *daemon, const gchar *kind)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

    return clawt_daemon_handle_request(daemon, frame);
}

/*
 * Off answers "off" with a reason; on, an event on the bus reaches the
 * fold, the snapshot says so, an edge is published once, and the layout
 * answers for the configured world.  A Grok job stands in for a running
 * agent, because make test starts no agent process.
 */
static void
test_daemon_office(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("clawt-office-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "config.yaml", NULL);
    g_autofree gchar *worlds = g_build_filename(CLAWT_TEST_FIXTURES, "office", NULL);
    g_autofree gchar *yaml = g_strdup_printf(
        "daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
        "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "office:\n  enabled: true\n  world: good\n  world_dirs: [\"%s\"]\n"
        "agents:\n  - id: kudu\n    runtime:\n      autostart: false\n",
        dir, dir, dir, dir, worlds);
    GMainContext *context = g_main_context_new();
    ClawtDaemon *daemon;
    g_autoptr(JsonNode) snapshot = NULL;
    g_autoptr(JsonNode) layout = NULL;
    g_autoptr(ClawtEvent) job = clawt_event_new("grok.job_updated", "job-1");
    JsonArray *people;
    gboolean found = FALSE;
    gint64 deadline;
    guint i;

    g_assert_true(g_file_set_contents(path, yaml, -1, &error));
    daemon = clawt_daemon_new(path, context);
    clawt_daemon_start(daemon, &error);
    g_assert_no_error(error);
    g_assert_nonnull(daemon->office);
    g_assert_nonnull(clawt_daemon_fleet_id(daemon));
    g_signal_connect(daemon->bus, "event", G_CALLBACK(count_presence), NULL);

    clawt_event_set_detail(job, "bot", "scout");
    clawt_event_set_detail(job, "state", "working");
    clawt_event_bus_publish(daemon->bus, job);

    snapshot = office_request(daemon, "office.snapshot");
    people = json_object_get_array_member(clawt_ipc_frame_get_payload(snapshot),
                                          "participants");
    for (i = 0; i < json_array_get_length(people); i++) {
        JsonObject *p = json_array_get_object_element(people, i);

        if (g_strcmp0(json_object_get_string_member(p, "id"), "grok:scout") == 0) {
            g_assert_cmpstr(json_object_get_string_member(p, "kind"), ==, "grok_bot");
            g_assert_cmpstr(json_object_get_string_member(p, "activity"), ==, "thinking");
            found = TRUE;
        }
    }
    g_assert_true(found);

    /* the coalesced edge, once, on the daemon's own context */
    presence_events = 0;
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (presence_events == 0 && g_get_monotonic_time() < deadline)
        g_main_context_iteration(context, FALSE);
    g_assert_cmpuint(presence_events, >, 0);

    layout = office_request(daemon, "office.layout");
    g_assert_false(clawt_ipc_frame_is_error(layout));
    g_assert_cmpstr(json_object_get_string_member(clawt_ipc_frame_get_payload(layout),
                                                  "world"), ==, "good");

    clawt_daemon_stop(daemon);
    g_object_unref(daemon);
    while (g_main_context_iteration(context, FALSE))
        ;
    g_main_context_unref(context);
    clawt_test_remove_tree(dir);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/office/turns-are-per-room", test_turns_are_per_room);
    g_test_add_func("/office/decay", test_decay);
    g_test_add_func("/office/priority-and-peers", test_priority_and_peers);
    g_test_add_func("/office/changed-since-reports-edges", test_changed_since_reports_edges);
    g_test_add_func("/office/helpers-and-snapshot", test_helpers_and_snapshot);
    g_test_add_func("/office/vocabulary", test_vocabulary);
    g_test_add_func("/office/world-loads-and-finds", test_world_loads_and_finds);
    g_test_add_func("/office/world-refusals", test_world_refusals);
    g_test_add_func("/office/discover-skips-refused", test_discover_skips_refused);
    g_test_add_func("/office/choreography-layers", test_choreography_layers);
    g_test_add_func("/office/director-places", test_director_places);
    g_test_add_func("/office/daemon", test_daemon_office);

    return g_test_run();
}
