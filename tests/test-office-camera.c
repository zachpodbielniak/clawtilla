/*
 * test-office-camera.c - The office camera, without a window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Clamps, a pan that stays inside the footprint, a wheel that keeps the
 * floor point under the cursor, and the same pose after one second at
 * 20 fps and at 60.  The orbit camera these commits replace is copied
 * at the bottom and shown to fail the pan and the pitch clamp: it had
 * no target argument and no pitch.
 */

#include "clawtilla.h"

#include <math.h>

static gfloat
wrap_delta(gfloat a, gfloat b)
{
    gfloat d = a - b;

    while (d > (gfloat)G_PI)
        d -= 2.0f * (gfloat)G_PI;
    while (d < -(gfloat)G_PI)
        d += 2.0f * (gfloat)G_PI;
    return d;
}

static void
test_pitch_fit_matches_old_eye(void)
{
    g_autoptr(ClawtOfficeCamera) cam = clawt_office_camera_new();
    gfloat pitch = clawt_office_camera_pitch_fit();
    gfloat reach = 30.0f;
    gfloat slant = reach * hypotf(1.0f, CLAWT_OFFICE_VIEW_3D_LIFT);
    gfloat eye_x = 0.0f;
    gfloat eye_y = 0.0f;
    gfloat eye_z = 0.0f;
    gfloat ground;

    clawt_office_camera_snap_pose(cam, 10.0f, 8.0f, CLAWT_OFFICE_CAMERA_YAW_HOME,
                                  pitch, slant);
    clawt_office_camera_eye(cam, &eye_x, &eye_y, &eye_z);
    ground = hypotf(eye_x - 10.0f, eye_z - 8.0f);
    g_assert_cmpfloat_with_epsilon(eye_y, reach * CLAWT_OFFICE_VIEW_3D_LIFT, 0.02f);
    g_assert_cmpfloat_with_epsilon(ground, reach, 0.02f);
    g_assert_cmpfloat(pitch, >, CLAWT_OFFICE_CAMERA_PITCH_MIN);
    g_assert_cmpfloat(pitch, <, CLAWT_OFFICE_CAMERA_PITCH_MAX);
}

static void
test_clamps(void)
{
    g_autoptr(ClawtOfficeCamera) cam = clawt_office_camera_new();
    gfloat x = 0.0f;
    gfloat z = 0.0f;

    clawt_office_camera_set_bounds(cam, 0.0f, 20.0f, 0.0f, 16.0f);
    clawt_office_camera_set_desired_pitch(cam, 0.0f);
    g_assert_cmpfloat(clawt_office_camera_get_desired_pitch(cam), >=,
                      CLAWT_OFFICE_CAMERA_PITCH_MIN);
    clawt_office_camera_set_desired_pitch(cam, 3.0f);
    g_assert_cmpfloat(clawt_office_camera_get_desired_pitch(cam), <=,
                      CLAWT_OFFICE_CAMERA_PITCH_MAX);

    clawt_office_camera_snap_pose(cam, 10.0f, 8.0f, 0.4f,
                                  clawt_office_camera_pitch_fit(), 12.0f);
    clawt_office_camera_look(cam, 0.0f, 10000.0f);
    g_assert_cmpfloat(clawt_office_camera_get_desired_pitch(cam), <=,
                      CLAWT_OFFICE_CAMERA_PITCH_MAX);
    clawt_office_camera_look(cam, 0.0f, -10000.0f);
    g_assert_cmpfloat(clawt_office_camera_get_desired_pitch(cam), >=,
                      CLAWT_OFFICE_CAMERA_PITCH_MIN);

    clawt_office_camera_set_desired_target(cam, 1000.0f, -1000.0f);
    clawt_office_camera_get_desired_target(cam, &x, &z);
    g_assert_cmpfloat(x, <=, 20.0f + CLAWT_OFFICE_CAMERA_TARGET_MARGIN);
    g_assert_cmpfloat(x, >=, 0.0f - CLAWT_OFFICE_CAMERA_TARGET_MARGIN);
    g_assert_cmpfloat(z, <=, 16.0f + CLAWT_OFFICE_CAMERA_TARGET_MARGIN);
    g_assert_cmpfloat(z, >=, 0.0f - CLAWT_OFFICE_CAMERA_TARGET_MARGIN);

    clawt_office_camera_set_desired_distance(cam, 0.01f);
    g_assert_cmpfloat(clawt_office_camera_get_desired_distance(cam), >=,
                      CLAWT_OFFICE_CAMERA_DIST_MIN);
    clawt_office_camera_set_desired_distance(cam, 10000.0f);
    g_assert_cmpfloat(clawt_office_camera_get_desired_distance(cam), <=,
                      CLAWT_OFFICE_CAMERA_DIST_MAX);
}

static void
test_pan_stays_in_bounds(void)
{
    g_autoptr(ClawtOfficeCamera) cam = clawt_office_camera_new();
    gfloat x = 0.0f;
    gfloat z = 0.0f;
    gfloat moved;

    clawt_office_camera_set_bounds(cam, 0.0f, 20.0f, 0.0f, 16.0f);
    clawt_office_camera_snap_pose(cam, 10.0f, 8.0f, 0.3f,
                                  clawt_office_camera_pitch_fit(), 18.0f);
    clawt_office_camera_pan(cam, 40.0f, -25.0f, 720.0f, CLAWT_OFFICE_VIEW_3D_FOVY);
    clawt_office_camera_get_desired_target(cam, &x, &z);
    moved = hypotf(x - 10.0f, z - 8.0f);
    g_assert_cmpfloat(moved, >, 0.5f);
    g_assert_cmpfloat(x, <=, 20.0f + CLAWT_OFFICE_CAMERA_TARGET_MARGIN);
    g_assert_cmpfloat(z, <=, 16.0f + CLAWT_OFFICE_CAMERA_TARGET_MARGIN);
    g_assert_cmpfloat(x, >=, -CLAWT_OFFICE_CAMERA_TARGET_MARGIN);
    g_assert_cmpfloat(z, >=, -CLAWT_OFFICE_CAMERA_TARGET_MARGIN);

    clawt_office_camera_pan(cam, 100000.0f, 100000.0f, 720.0f, CLAWT_OFFICE_VIEW_3D_FOVY);
    clawt_office_camera_get_desired_target(cam, &x, &z);
    g_assert_cmpfloat(x, <=, 20.0f + CLAWT_OFFICE_CAMERA_TARGET_MARGIN + 0.01f);
    g_assert_cmpfloat(z, <=, 16.0f + CLAWT_OFFICE_CAMERA_TARGET_MARGIN + 0.01f);
    g_assert_cmpfloat(x, >=, -CLAWT_OFFICE_CAMERA_TARGET_MARGIN - 0.01f);
    g_assert_cmpfloat(z, >=, -CLAWT_OFFICE_CAMERA_TARGET_MARGIN - 0.01f);
}

static void
pose_of(ClawtOfficeCamera *cam, gfloat *x, gfloat *z, gfloat *yaw, gfloat *pitch, gfloat *dist)
{
    clawt_office_camera_target(cam, x, z);
    *yaw = clawt_office_camera_get_yaw(cam);
    *pitch = clawt_office_camera_get_pitch(cam);
    *dist = clawt_office_camera_get_distance(cam);
}

static void
drive(ClawtOfficeCamera *cam, gboolean ramp, gfloat dt, gint steps)
{
    gint i;

    for (i = 0; i < steps; i++) {
        if (ramp) {
            clawt_office_camera_move(cam, 1.0f, 0.5f, dt, FALSE);
            clawt_office_camera_turn(cam, 1.0f, dt, FALSE);
        }
        clawt_office_camera_step(cam, dt);
    }
}

static void
test_dt_scaling(void)
{
    g_autoptr(ClawtOfficeCamera) slow = clawt_office_camera_new();
    g_autoptr(ClawtOfficeCamera) fast = clawt_office_camera_new();
    gfloat pitch = clawt_office_camera_pitch_fit();
    gfloat sx, sz, syaw, spitch, sdist;
    gfloat fx, fz, fyaw, fpitch, fdist;
    gboolean ramp;

    for (ramp = FALSE; ramp <= TRUE; ramp++) {
        clawt_office_camera_snap_pose(slow, 0.0f, 0.0f, 0.4f, pitch, 12.0f);
        clawt_office_camera_snap_pose(fast, 0.0f, 0.0f, 0.4f, pitch, 12.0f);
        if (!ramp) {
            clawt_office_camera_set_desired_target(slow, 20.0f, -8.0f);
            clawt_office_camera_set_desired_target(fast, 20.0f, -8.0f);
            clawt_office_camera_set_desired_yaw(slow, 0.4f + 1.5f);
            clawt_office_camera_set_desired_yaw(fast, 0.4f + 1.5f);
            clawt_office_camera_set_desired_pitch(slow, 1.2f);
            clawt_office_camera_set_desired_pitch(fast, 1.2f);
            clawt_office_camera_set_desired_distance(slow, 40.0f);
            clawt_office_camera_set_desired_distance(fast, 40.0f);
        }
        drive(slow, ramp, 1.0f / 20.0f, 20);
        drive(fast, ramp, 1.0f / 60.0f, 60);
        pose_of(slow, &sx, &sz, &syaw, &spitch, &sdist);
        pose_of(fast, &fx, &fz, &fyaw, &fpitch, &fdist);
        /*
         * A held target settles the same way at both rates.  Walking
         * while turning follows the facing that is on screen, which
         * lags the spring by a different slice of a frame at 20fps
         * than at 60, so that path is allowed a third of a tile.
         */
        g_assert_cmpfloat_with_epsilon(sx, fx, ramp ? 0.30f : 0.05f);
        g_assert_cmpfloat_with_epsilon(sz, fz, ramp ? 0.30f : 0.05f);
        g_assert_cmpfloat_with_epsilon(wrap_delta(syaw, fyaw), 0.0f, ramp ? 0.05f : 0.02f);
        g_assert_cmpfloat_with_epsilon(spitch, fpitch, ramp ? 0.05f : 0.02f);
        g_assert_cmpfloat_with_epsilon(sdist, fdist, ramp ? 0.30f : 0.05f);
    }
}

static void
test_slow_frame_does_not_overshoot(void)
{
    g_autoptr(ClawtOfficeCamera) cam = clawt_office_camera_new();
    gfloat x = 0.0f;
    gfloat z = 0.0f;

    clawt_office_camera_snap_pose(cam, 0.0f, 0.0f, 0.2f,
                                  clawt_office_camera_pitch_fit(), 10.0f);
    clawt_office_camera_set_desired_target(cam, 10.0f, 0.0f);
    clawt_office_camera_step(cam, 0.5f);
    clawt_office_camera_target(cam, &x, &z);
    g_assert_cmpfloat(x, <=, 10.0f + 0.001f);
    g_assert_cmpfloat(x, >, 1.0f);
}

static void
test_shift_is_faster(void)
{
    g_autoptr(ClawtOfficeCamera) plain = clawt_office_camera_new();
    g_autoptr(ClawtOfficeCamera) fast = clawt_office_camera_new();
    gfloat px, pz, fx, fz;

    clawt_office_camera_snap_pose(plain, 0.0f, 0.0f, 0.0f,
                                  clawt_office_camera_pitch_fit(), 10.0f);
    clawt_office_camera_snap_pose(fast, 0.0f, 0.0f, 0.0f,
                                  clawt_office_camera_pitch_fit(), 10.0f);
    clawt_office_camera_move(plain, 1.0f, 0.0f, 0.05f, FALSE);
    clawt_office_camera_move(fast, 1.0f, 0.0f, 0.05f, TRUE);
    clawt_office_camera_get_desired_target(plain, &px, &pz);
    clawt_office_camera_get_desired_target(fast, &fx, &fz);
    g_assert_cmpfloat_with_epsilon(fx, px * CLAWT_OFFICE_CAMERA_FAST_SCALE, 0.001f);
    g_assert_cmpfloat_with_epsilon(fz, pz * CLAWT_OFFICE_CAMERA_FAST_SCALE, 0.001f);
}

static void
test_floor_and_dolly(void)
{
    g_autoptr(ClawtOfficeCamera) cam = clawt_office_camera_new();
    gfloat pitch = clawt_office_camera_pitch_fit();
    gfloat eye[3];
    gfloat target[3];
    gfloat point[3];
    gfloat sx = 0.0f;
    gfloat sy = 0.0f;
    gfloat hit_x = 0.0f;
    gfloat hit_z = 0.0f;
    gfloat before;
    gfloat after;
    gfloat tx = 0.0f;
    gfloat tz = 0.0f;

    clawt_office_camera_snap_pose(cam, 12.0f, 9.0f, 0.9f, pitch, 24.0f);
    clawt_office_camera_eye(cam, &eye[0], &eye[1], &eye[2]);
    target[0] = 12.0f;
    target[1] = 0.0f;
    target[2] = 9.0f;
    point[0] = 16.0f;
    point[1] = 0.0f;
    point[2] = 14.0f;
    g_assert_true(clawt_office_view_project_3d(eye, target, CLAWT_OFFICE_VIEW_3D_FOVY,
                                               1280.0f, 720.0f, point, &sx, &sy));
    g_assert_true(clawt_office_camera_floor_at(cam, sx, sy, 1280.0f, 720.0f,
                                               CLAWT_OFFICE_VIEW_3D_FOVY, &hit_x, &hit_z));
    g_assert_cmpfloat_with_epsilon(hit_x, point[0], 0.05f);
    g_assert_cmpfloat_with_epsilon(hit_z, point[2], 0.05f);

    before = hypotf(12.0f - hit_x, 9.0f - hit_z);
    clawt_office_camera_dolly(cam, 1.0f, TRUE, hit_x, hit_z);
    clawt_office_camera_get_desired_target(cam, &tx, &tz);
    after = hypotf(tx - hit_x, tz - hit_z);
    g_assert_cmpfloat(after, <, before);
    g_assert_cmpfloat(clawt_office_camera_get_desired_distance(cam), <, 24.0f);
}

static void
test_zoom_around(void)
{
    gfloat cx = 400.0f;
    gfloat cy = 300.0f;
    gfloat cursor_x = 900.0f;
    gfloat cursor_y = 200.0f;
    gfloat old_zoom = 1.0f;
    gfloat new_zoom;
    gfloat world_x;
    gfloat world_y;
    gfloat again_x;
    gfloat again_y;

    new_zoom = clawt_office_camera_zoom_factor(old_zoom, 1.0f);
    g_assert_cmpfloat(new_zoom, >, old_zoom);
    world_x = cx + (cursor_x - 640.0f) / old_zoom;
    world_y = cy + (cursor_y - 360.0f) / old_zoom;
    clawt_office_camera_zoom_around(&cx, &cy, old_zoom, new_zoom,
                                    cursor_x, cursor_y, 1280.0f, 720.0f);
    again_x = cx + (cursor_x - 640.0f) / new_zoom;
    again_y = cy + (cursor_y - 360.0f) / new_zoom;
    g_assert_cmpfloat_with_epsilon(again_x, world_x, 0.01f);
    g_assert_cmpfloat_with_epsilon(again_y, world_y, 0.01f);

    g_assert_cmpfloat(clawt_office_camera_zoom_factor(1.0f, 100.0f), <=,
                      CLAWT_OFFICE_CAMERA_ZOOM_MAX);
    g_assert_cmpfloat(clawt_office_camera_zoom_factor(1.0f, -100.0f), >=,
                      CLAWT_OFFICE_CAMERA_ZOOM_MIN);
}

static void
test_double_click(void)
{
    g_assert_true(clawt_office_camera_double_click(0.2f, 3.0f, 4.0f));
    g_assert_false(clawt_office_camera_double_click(1.0f, 0.0f, 0.0f));
    g_assert_false(clawt_office_camera_double_click(0.1f, 40.0f, 0.0f));
    g_assert_false(clawt_office_camera_double_click(-1.0f, 0.0f, 0.0f));
}

static void
test_bindings_and_gdk(void)
{
    ClawtOfficeInput input = { 0 };
    guint i;
    gboolean saw_w = FALSE;

    g_assert_cmpuint(clawt_office_key_from_gdk(0x0077u), ==, CLAWT_OFFICE_KEY_W);
    g_assert_cmpuint(clawt_office_key_from_gdk(0x0057u), ==, CLAWT_OFFICE_KEY_W);
    g_assert_cmpuint(clawt_office_key_from_gdk(0xff1bu), ==, CLAWT_OFFICE_KEY_ESCAPE);
    g_assert_cmpuint(clawt_office_key_from_gdk(0xff50u), ==, CLAWT_OFFICE_KEY_HOME);
    g_assert_cmpuint(clawt_office_key_from_gdk(0xff51u), ==, CLAWT_OFFICE_KEY_LEFT);
    g_assert_cmpuint(clawt_office_key_from_gdk(0xffe1u), ==, CLAWT_OFFICE_KEY_LEFT_SHIFT);
    /* Return, Tab and U stay with GTK. */
    g_assert_cmpuint(clawt_office_key_from_gdk(0xff0du), ==, 0);
    g_assert_cmpuint(clawt_office_key_from_gdk(0xff09u), ==, 0);
    g_assert_cmpuint(clawt_office_key_from_gdk(0x0075u), ==, 0);

    /* GDK control is bit 2.  The frame protocol's control bit is 2, not 4. */
    g_assert_cmpuint(clawt_office_modifiers_from_gdk(1u << 0), ==, 1u);
    g_assert_cmpuint(clawt_office_modifiers_from_gdk(1u << 2), ==, 2u);
    g_assert_cmpuint(clawt_office_modifiers_from_gdk((1u << 2) | (1u << 3)), ==, 6u);

    g_assert_cmpint(clawt_office_binding_action(CLAWT_OFFICE_KEY_W), ==,
                    CLAWT_OFFICE_BIND_FORWARD);
    g_assert_cmpint(clawt_office_binding_action(CLAWT_OFFICE_KEY_UP), ==,
                    CLAWT_OFFICE_BIND_FORWARD);
    g_assert_cmpint(clawt_office_binding_action(CLAWT_OFFICE_KEY_Q), ==,
                    CLAWT_OFFICE_BIND_TURN_LEFT);
    g_assert_cmpint(clawt_office_binding_action(CLAWT_OFFICE_KEY_E), ==,
                    CLAWT_OFFICE_BIND_TURN_RIGHT);
    g_assert_cmpint(clawt_office_binding_action(CLAWT_OFFICE_KEY_ESCAPE), ==,
                    CLAWT_OFFICE_BIND_NONE);

    for (i = 0; i < clawt_office_binding_count(); i++) {
        guint key = clawt_office_binding_key(i);

        if (key == CLAWT_OFFICE_KEY_W) {
            clawt_office_input_note_key(&input, key, TRUE, TRUE);
            saw_w = TRUE;
        } else if (key == CLAWT_OFFICE_KEY_F) {
            clawt_office_input_note_key(&input, key, TRUE, TRUE);
        } else if (key == CLAWT_OFFICE_KEY_LEFT_SHIFT) {
            clawt_office_input_note_key(&input, key, TRUE, TRUE);
        }
    }
    g_assert_true(saw_w);
    g_assert_true(input.move_forward);
    g_assert_true(input.reset_fit);
    g_assert_true(input.shift);
    input.reset_fit = FALSE;
    clawt_office_input_note_key(&input, CLAWT_OFFICE_KEY_F, FALSE, TRUE);
    g_assert_false(input.reset_fit);
}

/*
 * The orbit camera these commits replace, as clawt_office_scene_orbit()
 * and place_camera_3d() stood on fix/office-avatars before the (#43)
 * camera commits.  dx was added to the bearing.  A zoom step multiplied
 * the distance by 0.9 or 1.1 and clamped it to 0.35..2.  The target was
 * not an argument: place_camera_3d() wrote it back to the map centre
 * every frame, and the pitch was the fixed lift, so a requested pitch
 * under the floor or past straight down could not be refused.
 *
 * Checked out of nothing.  The formula is the older code, and the
 * assertions are the ones the new camera passes.
 */
static void
legacy_orbit(gfloat *orbit, gfloat *distance, gfloat dx, gfloat zoom)
{
    *orbit += dx;
    if (zoom != 0.0f)
        *distance = CLAMP(*distance * (zoom > 0.0f ? 0.9f : 1.1f), 0.35f, 2.0f);
}

static gboolean
pan_moves_target(gboolean legacy)
{
    if (legacy) {
        gfloat orbit = CLAWT_OFFICE_CAMERA_YAW_HOME;
        gfloat distance = 1.0f;
        gfloat centre = 22.0f;
        gfloat target = centre;

        legacy_orbit(&orbit, &distance, 0.4f, 1.0f);
        /* place_camera_3d() put the target back on the map centre */
        return target != centre;
    }

    {
        g_autoptr(ClawtOfficeCamera) cam = clawt_office_camera_new();
        gfloat x = 0.0f;
        gfloat z = 0.0f;

        clawt_office_camera_set_bounds(cam, 0.0f, 44.0f, 0.0f, 40.0f);
        clawt_office_camera_snap_pose(cam, 22.0f, 20.0f, CLAWT_OFFICE_CAMERA_YAW_HOME,
                                      clawt_office_camera_pitch_fit(), 20.0f);
        clawt_office_camera_pan(cam, 80.0f, 0.0f, 720.0f, CLAWT_OFFICE_VIEW_3D_FOVY);
        clawt_office_camera_get_desired_target(cam, &x, &z);
        return fabsf(x - 22.0f) > 0.5f || fabsf(z - 20.0f) > 0.5f;
    }
}

static gfloat
pitch_after(gboolean legacy, gfloat requested)
{
    if (legacy)
        return requested;

    {
        g_autoptr(ClawtOfficeCamera) cam = clawt_office_camera_new();

        clawt_office_camera_set_desired_pitch(cam, requested);
        return clawt_office_camera_get_desired_pitch(cam);
    }
}

static void
test_legacy_orbit_fails_pan_and_pitch(void)
{
    g_assert_false(pan_moves_target(TRUE));
    g_assert_true(pan_moves_target(FALSE));
    g_assert_cmpfloat(pitch_after(TRUE, 0.0f), <, CLAWT_OFFICE_CAMERA_PITCH_MIN);
    g_assert_cmpfloat(pitch_after(FALSE, 0.0f), >=, CLAWT_OFFICE_CAMERA_PITCH_MIN);
    g_assert_cmpfloat(pitch_after(TRUE, 3.0f), >, CLAWT_OFFICE_CAMERA_PITCH_MAX);
    g_assert_cmpfloat(pitch_after(FALSE, 3.0f), <=, CLAWT_OFFICE_CAMERA_PITCH_MAX);

    /* and the old zoom still cannot express a distance past its clamp */
    {
        gfloat orbit = 0.65f;
        gfloat distance = 1.0f;
        gint i;

        for (i = 0; i < 40; i++)
            legacy_orbit(&orbit, &distance, 0.0f, -1.0f);
        g_assert_cmpfloat(distance, <=, 2.0f);
        g_assert_cmpfloat(CLAWT_OFFICE_CAMERA_DIST_MAX, >, 2.0f);
    }
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/office-camera/pitch-fit-matches-old-eye", test_pitch_fit_matches_old_eye);
    g_test_add_func("/office-camera/clamps", test_clamps);
    g_test_add_func("/office-camera/pan-bounds", test_pan_stays_in_bounds);
    g_test_add_func("/office-camera/dt-scaling", test_dt_scaling);
    g_test_add_func("/office-camera/no-overshoot", test_slow_frame_does_not_overshoot);
    g_test_add_func("/office-camera/shift-faster", test_shift_is_faster);
    g_test_add_func("/office-camera/floor-and-dolly", test_floor_and_dolly);
    g_test_add_func("/office-camera/zoom-around", test_zoom_around);
    g_test_add_func("/office-camera/double-click", test_double_click);
    g_test_add_func("/office-camera/bindings", test_bindings_and_gdk);
    g_test_add_func("/office-camera/legacy-fails", test_legacy_orbit_fails_pan_and_pitch);
    return g_test_run();
}
