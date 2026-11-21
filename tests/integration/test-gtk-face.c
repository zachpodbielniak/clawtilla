/*
 * test-gtk-face.c - The GTK face widget and its picker, on a real display
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An integration target (make test-gtk-face), not part of the hermetic
 * suite: it needs a display.  Run it under Xvfb or a nested session; it
 * needs no daemon.  The geometry itself is tested without a display in
 * tests/test-face.c -- this covers what GTK adds on top: the eyes easing
 * toward the pointer, the blink timer existing only while a face can be
 * seen, the client-wide motion switch, which widget clawt_gtk_build_avatar()
 * returns, and the picker's choice.
 */

#include "clawt-gtk.h"
#include "clawt-gtk-face.h"

#include <adwaita.h>
#include <clawtilla.h>

static void
spin(void)
{
    guint i;

    for (i = 0; i < 200 && g_main_context_pending(NULL); i++)
        g_main_context_iteration(NULL, FALSE);
}

/*
 * The pointer to the right turns the eyes right, easing rather than
 * jumping; leaving brings them back to rest.  A face that snapped would
 * flicker as the pointer crossed a row; one that never came back would
 * stare at where the pointer used to be.
 */
static void
test_eyes_follow_and_return(void)
{
    GtkWidget *face = g_object_ref_sink(clawt_gtk_face_new("scout", 1,
                                                           "#22b573", 32));
    gdouble look_x;
    gdouble target_x;
    gdouble target_y;
    guint frames = 0;

    clawt_gtk_faces_set_animate(TRUE);

    /* unallocated: the asked-for size stands in, centre at (16, 16) */
    clawt_gtk_face_point_at(CLAWT_GTK_FACE(face), 200.0, 16.0);
    clawt_gtk_face_get_pose(CLAWT_GTK_FACE(face), &look_x, NULL, &target_x,
                            &target_y);
    g_assert_cmpfloat(target_x, ==, 1.0);
    g_assert_cmpfloat_with_epsilon(target_y, 0.0, 1e-9);
    g_assert_cmpfloat(look_x, ==, 0.0);

    /* one frame moves part of the way, not all of it */
    clawt_gtk_face_step(CLAWT_GTK_FACE(face), 16.0);
    clawt_gtk_face_get_pose(CLAWT_GTK_FACE(face), &look_x, NULL, NULL, NULL);
    g_assert_cmpfloat(look_x, >, 0.0);
    g_assert_cmpfloat(look_x, <, 1.0);

    while (clawt_gtk_face_step(CLAWT_GTK_FACE(face), 16.0) && frames < 100)
        frames++;
    clawt_gtk_face_get_pose(CLAWT_GTK_FACE(face), &look_x, NULL, NULL, NULL);
    g_assert_cmpfloat(look_x, ==, 1.0);

    clawt_gtk_face_look_away(CLAWT_GTK_FACE(face));
    frames = 0;
    while (clawt_gtk_face_step(CLAWT_GTK_FACE(face), 16.0) && frames < 100)
        frames++;
    clawt_gtk_face_get_pose(CLAWT_GTK_FACE(face), &look_x, NULL, &target_x,
                            NULL);
    g_assert_cmpfloat(look_x, ==, 0.0);
    g_assert_cmpfloat(target_x, ==, 0.0);

    g_object_unref(face);
}

/*
 * A face costs a timer only while it is on screen and faces animate.
 * One left armed after unmap would wake a hidden window every few
 * seconds for nothing, for every agent in the list.
 */
static void
test_blink_armed_only_while_mapped(void)
{
    GtkWidget *window = gtk_window_new();
    GtkWidget *face = clawt_gtk_face_new("scout", 0, "#2f7de1", 32);

    clawt_gtk_faces_set_animate(TRUE);
    gtk_window_set_child(GTK_WINDOW(window), face);
    g_assert_false(clawt_gtk_face_is_blink_armed(CLAWT_GTK_FACE(face)));

    gtk_window_present(GTK_WINDOW(window));
    spin();
    g_assert_true(gtk_widget_get_mapped(face));
    g_assert_true(clawt_gtk_face_is_blink_armed(CLAWT_GTK_FACE(face)));

    /* the switch reaches faces already on screen, both ways */
    clawt_gtk_faces_set_animate(FALSE);
    g_assert_false(clawt_gtk_face_is_blink_armed(CLAWT_GTK_FACE(face)));
    clawt_gtk_faces_set_animate(TRUE);
    g_assert_true(clawt_gtk_face_is_blink_armed(CLAWT_GTK_FACE(face)));

    gtk_widget_set_visible(face, FALSE);
    spin();
    g_assert_false(clawt_gtk_face_is_blink_armed(CLAWT_GTK_FACE(face)));

    gtk_window_destroy(GTK_WINDOW(window));
    spin();
}

/*
 * With motion off, the pointer does nothing and a face already looking
 * somewhere is settled at rest -- "still" has to mean still at once,
 * not after the eyes finish their current move.
 */
static void
test_motion_off_settles(void)
{
    GtkWidget *window = gtk_window_new();
    GtkWidget *face = clawt_gtk_face_new("scout", 0, "#2f7de1", 32);
    gdouble target_x;

    clawt_gtk_faces_set_animate(TRUE);
    gtk_window_set_child(GTK_WINDOW(window), face);
    gtk_window_present(GTK_WINDOW(window));
    spin();

    clawt_gtk_face_point_at(CLAWT_GTK_FACE(face), 500.0, 16.0);
    clawt_gtk_face_get_pose(CLAWT_GTK_FACE(face), NULL, NULL, &target_x,
                            NULL);
    g_assert_cmpfloat(target_x, >, 0.0);

    clawt_gtk_faces_set_animate(FALSE);
    g_assert_false(clawt_gtk_faces_get_animate());
    clawt_gtk_face_get_pose(CLAWT_GTK_FACE(face), NULL, NULL, &target_x,
                            NULL);
    g_assert_cmpfloat(target_x, ==, 0.0);

    clawt_gtk_face_point_at(CLAWT_GTK_FACE(face), 500.0, 16.0);
    clawt_gtk_face_get_pose(CLAWT_GTK_FACE(face), NULL, NULL, &target_x,
                            NULL);
    g_assert_cmpfloat(target_x, ==, 0.0);

    clawt_gtk_faces_set_animate(TRUE);
    gtk_window_destroy(GTK_WINDOW(window));
    spin();
}

/*
 * The builder draws what the daemon resolved: a face for a shape, the
 * letter disc for initials -- and the letter disc for a shape this
 * build does not know, so a newer daemon's new shape never draws as
 * some other shape.  With nothing noted, the automatic face.
 */
static void
test_build_avatar_choice(void)
{
    GtkWidget *drawn;

    clawt_gtk_face_note("scout", "cloud");
    drawn = g_object_ref_sink(clawt_gtk_build_avatar(NULL, "Scout", "scout",
                                                     FALSE, NULL, 32));
    g_assert_true(CLAWT_GTK_IS_FACE(drawn));
    g_assert_cmpint(clawt_gtk_face_get_shape(CLAWT_GTK_FACE(drawn)), ==,
                    clawt_face_shape_lookup("cloud"));
    g_object_unref(drawn);

    clawt_gtk_face_note("scout", "initials");
    drawn = g_object_ref_sink(clawt_gtk_build_avatar(NULL, "Scout", "scout",
                                                     FALSE, NULL, 32));
    g_assert_true(ADW_IS_AVATAR(drawn));
    g_object_unref(drawn);

    clawt_gtk_face_note("scout", "hexagon");
    g_assert_cmpint(clawt_gtk_face_for("scout"), ==, CLAWT_FACE_INITIALS);

    clawt_gtk_face_note("scout", NULL);
    g_assert_cmpint(clawt_gtk_face_for("scout"), ==,
                    clawt_face_resolve("scout", NULL));
    drawn = g_object_ref_sink(clawt_gtk_build_avatar(NULL, "Scout", "scout",
                                                     FALSE, NULL, 32));
    g_assert_true(CLAWT_GTK_IS_FACE(drawn));
    g_object_unref(drawn);
}

static void
count_changed(ClawtGtkFacePicker *picker, gpointer user_data)
{
    (void)picker;
    (*(guint *)user_data)++;
}

/*
 * The picker answers in the spellings agents.face and agents.color
 * store: a lowercase shape name, initials, or "" for automatic; a hex
 * colour or "".  The preview shows the choice, and "automatic" shows the
 * agent's own automatic shape.  A junk colour is automatic, never
 * passed on for agent.set to refuse.
 */
static void
test_picker_choice(void)
{
    GtkWidget *picker = g_object_ref_sink(clawt_gtk_face_picker_new(
        "scout", NULL, NULL));
    ClawtGtkFacePicker *p = CLAWT_GTK_FACE_PICKER(picker);
    guint changed = 0;

    g_signal_connect(picker, "changed", G_CALLBACK(count_changed), &changed);

    g_assert_cmpstr(clawt_gtk_face_picker_get_face(p), ==, "");
    g_assert_cmpstr(clawt_gtk_face_picker_get_color(p), ==, "");
    g_assert_cmpint(
        clawt_gtk_face_get_shape(clawt_gtk_face_picker_get_preview(p)), ==,
        clawt_face_resolve("scout", NULL));

    clawt_gtk_face_picker_select(p, "Cloud", "#22b573");
    g_assert_cmpuint(changed, ==, 1);
    g_assert_cmpstr(clawt_gtk_face_picker_get_face(p), ==, "cloud");
    g_assert_cmpstr(clawt_gtk_face_picker_get_color(p), ==, "#22b573");
    g_assert_cmpint(
        clawt_gtk_face_get_shape(clawt_gtk_face_picker_get_preview(p)), ==,
        clawt_face_shape_lookup("cloud"));

    clawt_gtk_face_picker_select(p, "INITIALS", "not a colour");
    g_assert_cmpstr(clawt_gtk_face_picker_get_face(p), ==, "initials");
    g_assert_cmpstr(clawt_gtk_face_picker_get_color(p), ==, "");

    clawt_gtk_face_picker_select(p, "hexagon", NULL);
    g_assert_cmpstr(clawt_gtk_face_picker_get_face(p), ==, "");

    /* the automatic preview follows the id, as in the create dialog */
    clawt_gtk_face_picker_set_agent_id(p, "another-agent");
    g_assert_cmpint(
        clawt_gtk_face_get_shape(clawt_gtk_face_picker_get_preview(p)), ==,
        clawt_face_resolve("another-agent", NULL));

    /* shuffle: always a real shape and a palette colour, announced */
    {
        guint before = changed;
        guint n;

        for (n = 0; n < 20; n++) {
            clawt_gtk_face_picker_shuffle(p);
            g_assert_cmpint(clawt_face_shape_lookup(
                                clawt_gtk_face_picker_get_face(p)), >=, 0);
            g_assert_nonnull(clawt_color_ink(
                clawt_gtk_face_picker_get_color(p)));
        }
        g_assert_cmpuint(changed, ==, before + 20);
    }

    /* building with a saved choice does not announce a change */
    {
        guint quiet = 0;
        GtkWidget *saved = g_object_ref_sink(clawt_gtk_face_picker_new(
            "scout", "star", "#e5484d"));

        g_signal_connect(saved, "changed", G_CALLBACK(count_changed), &quiet);
        g_assert_cmpstr(clawt_gtk_face_picker_get_face(
                            CLAWT_GTK_FACE_PICKER(saved)), ==, "star");
        g_assert_cmpstr(clawt_gtk_face_picker_get_color(
                            CLAWT_GTK_FACE_PICKER(saved)), ==, "#e5484d");
        g_assert_cmpuint(quiet, ==, 0);
        g_object_unref(saved);
    }

    g_object_unref(picker);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!gtk_init_check()) {
        g_test_message("no display: run under Xvfb or a nested session");
        return 77;
    }
    adw_init();

    g_test_add_func("/gtk/face/eyes-follow-and-return",
                    test_eyes_follow_and_return);
    g_test_add_func("/gtk/face/blink-armed-only-while-mapped",
                    test_blink_armed_only_while_mapped);
    g_test_add_func("/gtk/face/motion-off-settles", test_motion_off_settles);
    g_test_add_func("/gtk/face/build-avatar-choice", test_build_avatar_choice);
    g_test_add_func("/gtk/face/picker-choice", test_picker_choice);

    return g_test_run();
}
