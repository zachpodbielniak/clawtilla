/*
 * test-lrgui-office.c - The drawn office's scene, without a window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Built only with libregnum (test-lrgui-*), and meaningful only in an
 * OFFICE=1 build, where the scene is compiled in; otherwise it says so
 * and passes.  No GL context: what is checked is what the scene decides
 * -- who is drawn, who walks in and out, simulate mode's seats -- not
 * what the GPU paints, which the screenshots in docs/office.org cover.
 */

#include "clawt-lrg.h"
#include "clawt-test-util.h"

#include <string.h>

#ifdef CLAWT_HAVE_OFFICE

#include "office-scene.h"

static JsonObject *
parse(const gchar *text)
{
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static const gchar *const world_json =
    "{\"id\":\"t\",\"zones\":["
    "{\"id\":\"floor\",\"tags\":[\"desk\"],\"anchors_2d\":[[100,100],[140,100],[180,100]]},"
    "{\"id\":\"booths\",\"tags\":[\"phone_booth\"],\"anchors_2d\":[[20,20],[60,20]]},"
    "{\"id\":\"board\",\"tags\":[\"meeting:6\"],\"anchors_2d\":[[300,300],[330,300],[360,300]]},"
    "{\"id\":\"lounge\",\"tags\":[\"lounge\"],\"anchors_2d\":[[500,500]]},"
    "{\"id\":\"door\",\"tags\":[\"entrance\"],\"anchors_2d\":[[10,600]]}]}";

static const gchar *const people_json =
    "\"participants\":["
    "{\"id\":\"chief\",\"name\":\"Chief\",\"kind\":\"agent\"},"
    "{\"id\":\"kudu\",\"name\":\"Kudu\",\"kind\":\"agent\"},"
    "{\"id\":\"oryx\",\"name\":\"Oryx\",\"kind\":\"agent\"},"
    "{\"id\":\"lynx\",\"name\":\"Lynx\",\"kind\":\"agent\"},"
    "{\"id\":\"wren\",\"name\":\"Wren\",\"kind\":\"agent\"}]";

/*
 * Simulate mode seats the real cast on the world's own anchors: the
 * first two on a call with each other in the booths, the next three in
 * the meeting room -- and invents nobody.
 */
static void
test_simulate_uses_the_world(void)
{
    g_autoptr(JsonObject) world = parse(world_json);
    g_autofree gchar *text = g_strdup_printf("{\"placements\":[],%s}", people_json);
    g_autoptr(JsonObject) layout = parse(text);
    g_autoptr(GRand) rng = g_rand_new_with_seed(7);
    g_autoptr(JsonObject) fake = clawt_office_simulate_layout(world, layout, rng);
    JsonArray *placements = json_object_get_array_member(fake, "placements");
    guint i;
    guint calls = 0;
    guint meeting = 0;

    g_assert_cmpuint(json_array_get_length(placements), ==, 5);
    for (i = 0; i < json_array_get_length(placements); i++) {
        JsonObject *p = json_array_get_object_element(placements, i);
        const gchar *activity = json_object_get_string_member(p, "activity");
        gdouble x = json_object_get_double_member(p, "x");

        if (g_strcmp0(activity, "on_call") == 0) {
            calls++;
            /* a booth, and the other one on the line */
            g_assert_true(x == 20.0 || x == 60.0);
            g_assert_nonnull(json_object_get_string_member(p, "facing"));
        }
        if (g_strcmp0(activity, "in_meeting") == 0) {
            meeting++;
            g_assert_cmpfloat(json_object_get_double_member(p, "y"), ==, 300.0);
        }
    }
    g_assert_cmpuint(calls, ==, 2);
    g_assert_cmpuint(meeting, ==, 3);
    /* the real participants travel with it, untouched */
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(fake,
                                                                        "participants")), ==, 5);
}

/*
 * Who is drawn is exactly who the layout places: a participant dropped
 * from it walks out and is gone once it reaches the door.
 */
static void
test_layout_in_and_out(void)
{
    ClawtOfficeScene *scene = clawt_office_scene_new();
    g_autofree gchar *both = g_strdup_printf(
        "{\"placements\":[{\"id\":\"chief\",\"activity\":\"working\",\"x\":100,\"y\":100},"
        "{\"id\":\"kudu\",\"activity\":\"idle\",\"x\":500,\"y\":500}],%s}", people_json);
    g_autofree gchar *one = g_strdup_printf(
        "{\"placements\":[{\"id\":\"chief\",\"activity\":\"working\",\"x\":100,\"y\":100}],%s}",
        people_json);
    g_autoptr(JsonObject) first = parse(both);
    g_autoptr(JsonObject) second = parse(one);
    guint frames;

    clawt_office_scene_set_layout(scene, first);
    g_assert_cmpuint(clawt_office_scene_n_participants(scene), ==, 2);

    clawt_office_scene_set_layout(scene, second);
    g_assert_cmpuint(clawt_office_scene_n_participants(scene), ==, 2);
    /* kudu walks out (to the default door, no world loaded), then fades */
    for (frames = 0; frames < 600 && clawt_office_scene_n_participants(scene) > 1; frames++)
        clawt_office_scene_update(scene, 1.0f / 30.0f);
    g_assert_cmpuint(clawt_office_scene_n_participants(scene), ==, 1);

    /* and nobody stands still forever animating: a settled room is idle */
    for (frames = 0; frames < 120; frames++)
        clawt_office_scene_update(scene, 1.0f / 30.0f);
    g_assert_false(clawt_office_scene_update(scene, 1.0f / 30.0f));
    clawt_office_scene_free(scene);
}

#endif /* CLAWT_HAVE_OFFICE */

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

#ifdef CLAWT_HAVE_OFFICE
    g_test_add_func("/lrgui/office/simulate-uses-the-world", test_simulate_uses_the_world);
    g_test_add_func("/lrgui/office/layout-in-and-out", test_layout_in_and_out);
#else
    g_test_message("built without OFFICE=1: the office scene is not compiled in");
#endif

    return g_test_run();
}
