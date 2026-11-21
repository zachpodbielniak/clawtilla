/*
 * test-face.c - The generated face: shapes, eyes, blinking, colour, SVG
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawt-face is pure geometry with no toolkit, and every client draws
 * from it, so a regression here is a regression in all three at once.
 * Each test says what a person would see go wrong if it failed.
 */

#include <clawtilla.h>

#include <locale.h>
#include <math.h>
#include <string.h>

#define OUTLINE_POINTS (240)

/* ── Shapes and names ─────────────────────────────────────────────── */

/*
 * Names are what agents.face stores.  Two shapes with one name would
 * make one of them impossible to choose; a lookup that does not round
 * trip would show a picker selection that is not what was saved.
 */
static void
test_names(void)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    guint count = clawt_face_shape_count();
    guint i;

    g_assert_cmpuint(count, >=, 12);

    for (i = 0; i < count; i++) {
        const gchar *name = clawt_face_shape_name((gint)i);
        g_autofree gchar *shouted = g_ascii_strup(name, -1);
        g_autofree gchar *padded = g_strdup_printf("  %s ", name);

        g_assert_nonnull(name);
        g_assert_nonnull(clawt_face_shape_label((gint)i));
        g_assert_false(g_hash_table_contains(seen, name));
        g_hash_table_add(seen, (gpointer)name);

        g_assert_cmpint(clawt_face_shape_lookup(name), ==, (gint)i);
        /* typed into YAML by hand: case and spaces are not a new name */
        g_assert_cmpint(clawt_face_shape_lookup(shouted), ==, (gint)i);
        g_assert_cmpint(clawt_face_shape_lookup(padded), ==, (gint)i);
        g_assert_true(clawt_face_value_is_valid(name));
    }

    g_assert_null(clawt_face_shape_name((gint)count));
    g_assert_null(clawt_face_shape_name(-1));
    g_assert_cmpint(clawt_face_shape_lookup("initials"), ==,
                    CLAWT_FACE_INITIALS);
    g_assert_cmpint(clawt_face_shape_lookup("Initials"), ==,
                    CLAWT_FACE_INITIALS);
    g_assert_cmpint(clawt_face_shape_lookup("hexagon"), ==, -2);
    g_assert_cmpint(clawt_face_shape_lookup(""), ==, -2);
    g_assert_cmpint(clawt_face_shape_lookup(NULL), ==, -2);
}

/*
 * agent.set refuses what this refuses, with clawt_face_valid_values()
 * as the hint.  Empty must stay valid: it is how "pick one for me" is
 * written back after a shape was chosen.
 */
static void
test_valid_values(void)
{
    g_autofree gchar *list = clawt_face_valid_values();
    guint i;

    g_assert_true(clawt_face_value_is_valid(NULL));
    g_assert_true(clawt_face_value_is_valid(""));
    g_assert_true(clawt_face_value_is_valid("  "));
    g_assert_true(clawt_face_value_is_valid("initials"));
    g_assert_false(clawt_face_value_is_valid("hexagon"));
    g_assert_false(clawt_face_value_is_valid("cloud;rm"));

    for (i = 0; i < clawt_face_shape_count(); i++)
        g_assert_nonnull(strstr(list, clawt_face_shape_name((gint)i)));
    g_assert_nonnull(strstr(list, "initials"));
}

/* ── Resolving an agent's face ────────────────────────────────────── */

/*
 * A configured shape wins; initials stays initials; a typo falls back
 * to the automatic face rather than to nothing.
 */
static void
test_resolve_rules(void)
{
    gint automatic = clawt_face_resolve("hornbill", NULL);

    g_assert_cmpint(automatic, >=, 0);
    g_assert_cmpint(automatic, <, (gint)clawt_face_shape_count());

    g_assert_cmpint(clawt_face_resolve("hornbill", "cloud"), ==,
                    clawt_face_shape_lookup("cloud"));
    g_assert_cmpint(clawt_face_resolve("hornbill", "initials"), ==,
                    CLAWT_FACE_INITIALS);
    g_assert_cmpint(clawt_face_resolve("hornbill", "hexagon"), ==,
                    automatic);
    g_assert_cmpint(clawt_face_resolve("hornbill", ""), ==, automatic);

    g_assert_cmpstr(clawt_face_resolve_name("hornbill", "initials"), ==,
                    "initials");
    g_assert_cmpstr(clawt_face_resolve_name("hornbill", "CLOUD"), ==,
                    "cloud");
    g_assert_cmpstr(clawt_face_resolve_name("hornbill", NULL), ==,
                    clawt_face_shape_name(automatic));

    /* no id at all still gets a face, not a crash */
    g_assert_cmpint(clawt_face_resolve(NULL, NULL), >=, 0);
}

/*
 * The automatic face must be the same in every process (GTK, the web
 * server and the daemon resolve it separately) and must actually vary:
 * a hash stuck on a few shapes would give a fleet of identical blobs.
 */
static void
test_resolve_spread(void)
{
    guint hits[64] = { 0 };
    guint used = 0;
    guint i;

    g_assert_cmpuint(clawt_face_shape_count(), <=, G_N_ELEMENTS(hits));
    g_assert_cmpuint(clawt_face_seed("hornbill"), ==,
                     clawt_face_seed("hornbill"));
    g_assert_cmpuint(clawt_face_seed("hornbill"), !=,
                     clawt_face_seed("hornbil"));

    for (i = 0; i < 500; i++) {
        g_autofree gchar *id = g_strdup_printf("agent-%u", i);

        hits[clawt_face_resolve(id, NULL)]++;
    }

    for (i = 0; i < clawt_face_shape_count(); i++) {
        if (hits[i] > 0)
            used++;
        /* no shape takes more than a quarter of a fleet */
        g_assert_cmpuint(hits[i], <, 125);
    }
    g_assert_cmpuint(used, >=, 12);
}

/*
 * The shape and the colour are picked from the same id; if they came
 * from the same hash bits, every agent of one colour would also be the
 * same shape.  Over many ids, one tone must show several shapes.
 */
static void
test_shape_independent_of_tone(void)
{
    guint shapes_for_tone0[64] = { 0 };
    guint distinct = 0;
    guint i;

    for (i = 0; i < 400; i++) {
        g_autofree gchar *id = g_strdup_printf("worker-%u", i);

        if (clawt_avatar_tone(id, NULL) == 0)
            shapes_for_tone0[clawt_face_resolve(id, NULL)]++;
    }
    for (i = 0; i < clawt_face_shape_count(); i++) {
        if (shapes_for_tone0[i] > 0)
            distinct++;
    }
    g_assert_cmpuint(distinct, >=, 6);
}

/* ── Outlines ─────────────────────────────────────────────────────── */

/*
 * Finite, inside the box, never through the centre.  A NaN draws
 * nothing in GTK and a broken path in SVG; a radius of 0 or less would
 * fold the shape over itself and lrg's triangle fan would draw a bow
 * tie.  Outside the box would be clipped by every client.
 */
static void
test_outline_bounds(void)
{
    gdouble xy[OUTLINE_POINTS * 2];
    guint shape;
    guint i;

    for (shape = 0; shape < clawt_face_shape_count(); shape++) {
        gdouble most = 0.0;

        clawt_face_outline((gint)shape, OUTLINE_POINTS, xy);

        for (i = 0; i < OUTLINE_POINTS; i++) {
            gdouble x = xy[i * 2];
            gdouble y = xy[i * 2 + 1];
            gdouble r = sqrt(x * x + y * y);

            g_assert_true(isfinite(x) && isfinite(y));
            g_assert_cmpfloat(fabs(x), <=, 1.0);
            g_assert_cmpfloat(fabs(y), <=, 1.0);
            g_assert_cmpfloat(r, >, 0.35);
            most = MAX(most, r);
        }

        /* scaled so the furthest edge nearly fills the box */
        g_assert_cmpfloat(most, >, 0.85);
        g_assert_cmpfloat(most, <=, 0.93);
    }
}

/*
 * Neighbouring points close together: the outline is one continuous
 * loop with no jumps, and the last point leads back to the first.
 */
static void
test_outline_closed(void)
{
    gdouble xy[OUTLINE_POINTS * 2];
    guint shape;
    guint i;

    for (shape = 0; shape < clawt_face_shape_count(); shape++) {
        clawt_face_outline((gint)shape, OUTLINE_POINTS, xy);

        for (i = 0; i < OUTLINE_POINTS; i++) {
            guint j = (i + 1) % OUTLINE_POINTS;
            gdouble dx = xy[j * 2] - xy[i * 2];
            gdouble dy = xy[j * 2 + 1] - xy[i * 2 + 1];

            g_assert_cmpfloat(sqrt(dx * dx + dy * dy), <, 0.08);
        }
    }

    /* an out-of-range shape is drawn as shape 0, not garbage */
    {
        gdouble a[16];
        gdouble b[16];

        clawt_face_outline(0, 8, a);
        clawt_face_outline(999, 8, b);
        g_assert_cmpmem(a, sizeof(a), b, sizeof(b));
    }
}

/* ── Eyes ─────────────────────────────────────────────────────────── */

/* Inside the shape: closer to the centre than its edge in that direction. */
static gboolean
inside(gint shape, gdouble x, gdouble y, gdouble margin)
{
    gdouble r = sqrt(x * x + y * y);
    gdouble theta = atan2(y, x);

    return r + margin <= clawt_face_radius_at(shape, theta);
}

static gboolean
eye_inside(gint shape, const ClawtFaceEye *eye)
{
    gdouble hw = eye->width / 2.0;
    gdouble hh = eye->height / 2.0;

    return inside(shape, eye->x - hw, eye->y - hh, 0.02) &&
           inside(shape, eye->x + hw, eye->y - hh, 0.02) &&
           inside(shape, eye->x - hw, eye->y + hh, 0.02) &&
           inside(shape, eye->x + hw, eye->y + hh, 0.02);
}

/*
 * The eyes stay on the face however far they look -- on every shape,
 * in all eight directions.  An eye hanging off the edge of a star is
 * the bug this catches, and it is easy to cause by tuning one shape.
 */
static void
test_eyes_stay_inside(void)
{
    static const gdouble dirs[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
        { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 }
    };
    guint shape;
    guint d;

    for (shape = 0; shape < clawt_face_shape_count(); shape++) {
        ClawtFaceEyes eyes;

        clawt_face_eyes((gint)shape, NULL, &eyes);
        if (!eye_inside((gint)shape, &eyes.left) ||
            !eye_inside((gint)shape, &eyes.right))
            g_error("%s: eyes off the face at rest",
                    clawt_face_shape_name((gint)shape));

        for (d = 0; d < 8; d++) {
            ClawtFacePose pose = { dirs[d][0] * 5.0, dirs[d][1] * 5.0, 0.0 };

            clawt_face_eyes((gint)shape, &pose, &eyes);
            if (!eye_inside((gint)shape, &eyes.left) ||
                !eye_inside((gint)shape, &eyes.right))
                g_error("%s: eyes off the face looking (%g, %g)",
                        clawt_face_shape_name((gint)shape),
                        dirs[d][0], dirs[d][1]);

            /* and never into each other */
            g_assert_cmpfloat(eyes.right.x - eyes.left.x, >,
                              eyes.left.width + 0.05);
        }
    }
}

/*
 * The look is clamped to a disc: a diagonal moves the eyes no further
 * than a straight look, and a huge or NaN look is not a huge offset.
 */
static void
test_look_clamped(void)
{
    ClawtFaceEyes rest;
    ClawtFaceEyes right;
    ClawtFaceEyes far;
    ClawtFaceEyes diagonal;
    ClawtFaceEyes nan_eyes;
    ClawtFacePose pose_right = { 1.0, 0.0, 0.0 };
    ClawtFacePose pose_far = { 100.0, 0.0, 0.0 };
    ClawtFacePose pose_diag = { 1.0, 1.0, 0.0 };
    ClawtFacePose pose_nan = { NAN, INFINITY, NAN };
    gdouble dx;
    gdouble dy;

    clawt_face_eyes(0, NULL, &rest);
    clawt_face_eyes(0, &pose_right, &right);
    clawt_face_eyes(0, &pose_far, &far);
    clawt_face_eyes(0, &pose_diag, &diagonal);
    clawt_face_eyes(0, &pose_nan, &nan_eyes);

    g_assert_cmpfloat_with_epsilon(right.left.x - rest.left.x,
                                   CLAWT_FACE_LOOK_REACH, 1e-9);
    g_assert_cmpfloat_with_epsilon(far.left.x, right.left.x, 1e-9);

    dx = diagonal.left.x - rest.left.x;
    dy = (diagonal.left.y - rest.left.y) / 0.7;
    g_assert_cmpfloat(sqrt(dx * dx + dy * dy), <=,
                      CLAWT_FACE_LOOK_REACH + 1e-9);

    g_assert_cmpfloat_with_epsilon(nan_eyes.left.x, rest.left.x, 1e-9);
    g_assert_cmpfloat_with_epsilon(nan_eyes.left.height, rest.left.height,
                                   1e-9);
}

/*
 * A blink squashes the eyes to a thin line and no further, keeps them
 * centred where they were, and keeps the corner radius legal -- more
 * than half the side would make GTK refuse the rounded rect.
 */
static void
test_blink_shape(void)
{
    ClawtFaceEyes open_eyes;
    ClawtFaceEyes shut;
    ClawtFaceEyes half;
    ClawtFacePose pose_shut = { 0.0, 0.0, 1.0 };
    ClawtFacePose pose_half = { 0.0, 0.0, 0.5 };
    ClawtFacePose pose_over = { 0.0, 0.0, 7.0 };
    ClawtFaceEyes over;

    clawt_face_eyes(2, NULL, &open_eyes);
    clawt_face_eyes(2, &pose_shut, &shut);
    clawt_face_eyes(2, &pose_half, &half);
    clawt_face_eyes(2, &pose_over, &over);

    g_assert_cmpfloat(shut.left.height, <, open_eyes.left.height / 4.0);
    g_assert_cmpfloat(shut.left.height, >, 0.0);
    g_assert_cmpfloat(half.left.height, <, open_eyes.left.height);
    g_assert_cmpfloat(half.left.height, >, shut.left.height);
    g_assert_cmpfloat_with_epsilon(over.left.height, shut.left.height, 1e-9);
    g_assert_cmpfloat_with_epsilon(shut.left.y, open_eyes.left.y, 1e-9);
    g_assert_cmpfloat(shut.left.radius, <=, shut.left.height / 2.0 + 1e-9);
    g_assert_cmpfloat(open_eyes.left.radius, <=,
                      open_eyes.left.width / 2.0 + 1e-9);
}

/* ── Pointer to look ──────────────────────────────────────────────── */

/*
 * Toward the pointer, growing with distance, capped at 1; nothing at
 * the centre or for a zero-sized face (which would divide by zero).
 */
static void
test_look_at(void)
{
    gdouble lx;
    gdouble ly;

    clawt_face_look_at(10, 10, 5, 10, 10, &lx, &ly);
    g_assert_cmpfloat(lx, ==, 0.0);
    g_assert_cmpfloat(ly, ==, 0.0);

    clawt_face_look_at(10, 10, 0, 50, 10, &lx, &ly);
    g_assert_cmpfloat(lx, ==, 0.0);

    /* one radius to the right: a third of the way */
    clawt_face_look_at(10, 10, 5, 15, 10, &lx, &ly);
    g_assert_cmpfloat_with_epsilon(lx, 1.0 / 3.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(ly, 0.0, 1e-9);

    /* far up-left: full length, pointing up-left */
    clawt_face_look_at(10, 10, 5, -1000, -1000, &lx, &ly);
    g_assert_cmpfloat_with_epsilon(sqrt(lx * lx + ly * ly), 1.0, 1e-9);
    g_assert_cmpfloat(lx, <, 0.0);
    g_assert_cmpfloat(ly, <, 0.0);

    /* below: looks down (y grows downward on screen) */
    clawt_face_look_at(0, 0, 5, 0, 100, &lx, &ly);
    g_assert_cmpfloat_with_epsilon(ly, 1.0, 1e-9);
}

/*
 * Easing approaches and then lands exactly, so a client can stop its
 * frame clock by comparing for equality.
 */
static void
test_ease(void)
{
    gdouble v = 0.0;
    guint frames = 0;

    g_assert_cmpfloat(clawt_face_ease(0.3, 1.0, 0.0), ==, 0.3);
    g_assert_cmpfloat(clawt_face_ease(0.3, 1.0, -5.0), ==, 0.3);

    while (v != 1.0 && frames < 1000) {
        gdouble next = clawt_face_ease(v, 1.0, 16.0);

        g_assert_cmpfloat(next, >, v);
        v = next;
        frames++;
    }
    g_assert_cmpfloat(v, ==, 1.0);
    /* settles in well under a second at 60 fps */
    g_assert_cmpuint(frames, <, 40);
}

/* ── Blinking ─────────────────────────────────────────────────────── */

/*
 * Walk @seed's schedule over @span_ms and collect where each separate
 * blink starts (a double blink counts once).  Returns the fraction of
 * the time the eyes were more than half shut.
 */
static gdouble
walk_blinks(guint32 seed, gint64 from, gint64 span_ms, GArray *starts)
{
    gint64 t;
    gint64 shut = 0;
    gint64 last_open = from - 1000;
    gboolean blinking = FALSE;

    for (t = from; t < from + span_ms; t++) {
        gdouble amount = clawt_face_blink_at(seed, t);

        g_assert_cmpfloat(amount, >=, 0.0);
        g_assert_cmpfloat(amount, <=, 1.0);

        if (amount > 0.5)
            shut++;

        if (amount > 0.0 && !blinking) {
            /* a new blink, unless it is the second half of a double */
            if (t - last_open > 400)
                g_array_append_val(starts, t);
            blinking = TRUE;
        } else if (amount == 0.0 && blinking) {
            blinking = FALSE;
            last_open = t;
        }
    }

    return (gdouble)shut / span_ms;
}

/*
 * Blinks come every 2.5-6 s, the eyes are open nearly all the time, and
 * the same seed gives the same schedule.  Too frequent reads as a
 * twitch; too rare and nobody notices a face is alive.
 */
static void
test_blink_schedule(void)
{
    g_autoptr(GArray) starts = g_array_new(FALSE, FALSE, sizeof(gint64));
    guint32 seeds[] = { 1, 7, 0xdeadbeef, clawt_face_seed("hornbill") };
    guint s;
    guint i;

    for (s = 0; s < G_N_ELEMENTS(seeds); s++) {
        gdouble shut;

        g_array_set_size(starts, 0);
        shut = walk_blinks(seeds[s], 0, 60000, starts);

        g_assert_cmpfloat(shut, <, 0.06);
        g_assert_cmpuint(starts->len, >=, 9);   /* 60 s / 6 s */
        g_assert_cmpuint(starts->len, <=, 25);  /* 60 s / 2.5 s */

        for (i = 1; i < starts->len; i++) {
            gint64 gap = g_array_index(starts, gint64, i) -
                         g_array_index(starts, gint64, i - 1);

            g_assert_cmpint(gap, >=, CLAWT_FACE_BLINK_GAP_MIN_MS - 300);
            g_assert_cmpint(gap, <=, CLAWT_FACE_BLINK_GAP_MAX_MS);
        }

        /* deterministic: a pure function of seed and time */
        for (i = 0; i < 200; i++)
            g_assert_cmpfloat(clawt_face_blink_at(seeds[s], i * 37),
                              ==, clawt_face_blink_at(seeds[s], i * 37));
    }
}

/*
 * Negative times work like any other: a client's clock origin is its
 * own business, and floor division gone wrong would skip a blink or
 * blink twice around zero.
 */
static void
test_blink_negative_time(void)
{
    g_autoptr(GArray) starts = g_array_new(FALSE, FALSE, sizeof(gint64));
    guint i;

    walk_blinks(42, -30000, 60000, starts);
    g_assert_cmpuint(starts->len, >=, 9);
    for (i = 1; i < starts->len; i++)
        g_assert_cmpint(g_array_index(starts, gint64, i) -
                        g_array_index(starts, gint64, i - 1), >=,
                        CLAWT_FACE_BLINK_GAP_MIN_MS - 300);
}

/*
 * Two agents must not blink in step: a sidebar that blinks all at once
 * looks like a rendering glitch rather than a crowd.
 */
static void
test_blink_not_in_step(void)
{
    guint32 a = clawt_face_seed("alpha");
    guint32 b = clawt_face_seed("beta");
    gint64 t;
    guint together = 0;
    guint blinks_a = 0;

    for (t = 0; t < 120000; t += 10) {
        gboolean shut_a = clawt_face_blink_at(a, t) > 0.5;
        gboolean shut_b = clawt_face_blink_at(b, t) > 0.5;

        if (shut_a)
            blinks_a++;
        if (shut_a && shut_b)
            together++;
    }
    g_assert_cmpuint(blinks_a, >, 0);
    g_assert_cmpuint(together, <, blinks_a / 3);
}

/*
 * next_blink is where a client arms its one timer.  It must land on a
 * moment the eyes are actually closing, never in the past, and never
 * skip over a blink -- a skipped one means a face that stops blinking.
 */
static void
test_next_blink(void)
{
    guint32 seed = clawt_face_seed("hornbill");
    gint64 now = 12345;
    guint n;

    for (n = 0; n < 30; n++) {
        gint64 next = clawt_face_next_blink(seed, now);
        gint64 t;

        g_assert_cmpint(next, >=, now);
        g_assert_cmpint(next - now, <=, CLAWT_FACE_BLINK_GAP_MAX_MS + 800);
        g_assert_cmpfloat(clawt_face_blink_at(seed, next + 40), >, 0.0);

        /* nothing between now and then */
        for (t = now; t < next; t++) {
            if (clawt_face_blink_at(seed, t) > 0.0 &&
                clawt_face_blink_at(seed, t - 1) == 0.0)
                g_error("a blink started at %" G_GINT64_FORMAT
                        " before next_blink said %" G_GINT64_FORMAT, t, next);
        }

        now = next + CLAWT_FACE_BLINK_MS;
    }
}

/* ── Colour ───────────────────────────────────────────────────────── */

/*
 * Dark eyes on light fills, light eyes on dark ones; anything that is
 * not a colour gets the dark default rather than nothing.
 */
static void
test_eye_color(void)
{
    guint i;

    g_assert_cmpstr(clawt_face_eye_color("#ffffff"), ==, "#1b1b22");
    g_assert_cmpstr(clawt_face_eye_color("#f2a516"), ==, "#1b1b22");
    g_assert_cmpstr(clawt_face_eye_color("#000000"), ==, "#f6f5f4");
    g_assert_cmpstr(clawt_face_eye_color("#a51d2d"), ==, "#f6f5f4");
    g_assert_cmpstr(clawt_face_eye_color("#222"), ==, "#f6f5f4");
    g_assert_cmpstr(clawt_face_eye_color("nonsense"), ==, "#1b1b22");
    g_assert_cmpstr(clawt_face_eye_color(NULL), ==, "#1b1b22");

    /* every swatch is a colour, and they are all different */
    for (i = 0; i < CLAWT_FACE_PALETTE_SIZE; i++) {
        guint j;

        g_assert_nonnull(clawt_color_ink(clawt_face_palette_color(i)));
        for (j = 0; j < i; j++)
            g_assert_cmpstr(clawt_face_palette_color(i), !=,
                            clawt_face_palette_color(j));
    }
    g_assert_null(clawt_face_palette_color(CLAWT_FACE_PALETTE_SIZE));
}

/* ── SVG ──────────────────────────────────────────────────────────── */

typedef struct {
    guint  paths;
    guint  rects;
    guint  groups;
    gchar *fill;
    gchar *style;
} SvgSeen;

static void
svg_start(GMarkupParseContext *context, const gchar *element,
          const gchar **names, const gchar **values, gpointer user_data,
          GError **error)
{
    SvgSeen *seen = user_data;
    guint i;

    if (g_strcmp0(element, "path") == 0)
        seen->paths++;
    if (g_strcmp0(element, "rect") == 0)
        seen->rects++;
    if (g_strcmp0(element, "g") == 0)
        seen->groups++;

    for (i = 0; names[i] != NULL; i++) {
        if (g_strcmp0(element, "path") == 0 &&
            g_strcmp0(names[i], "fill") == 0)
            seen->fill = g_strdup(values[i]);
        if (g_strcmp0(element, "svg") == 0 &&
            g_strcmp0(names[i], "style") == 0)
            seen->style = g_strdup(values[i]);
    }
}

static void
parse_svg(const gchar *svg, SvgSeen *seen)
{
    static const GMarkupParser parser = { svg_start, NULL, NULL, NULL, NULL };
    g_autoptr(GMarkupParseContext) context =
        g_markup_parse_context_new(&parser, 0, seen, NULL);
    g_autoptr(GError) error = NULL;

    g_markup_parse_context_parse(context, svg, -1, &error);
    g_assert_no_error(error);
    g_markup_parse_context_end_parse(context, &error);
    g_assert_no_error(error);
}

/*
 * Well-formed for every shape, one body and two eyes, the fill as
 * given, and no blink variables unless animated -- the CLI's export is
 * for files, where a dangling CSS variable is just noise.
 */
static void
test_svg(void)
{
    guint shape;

    for (shape = 0; shape < clawt_face_shape_count(); shape++) {
        g_autofree gchar *svg =
            clawt_face_svg((gint)shape, "#22b573", 48, NULL, 1, FALSE);
        SvgSeen seen = { 0 };

        g_assert_nonnull(svg);
        g_assert_true(g_str_has_prefix(svg, "<svg "));
        parse_svg(svg, &seen);
        g_assert_cmpuint(seen.paths, ==, 1);
        g_assert_cmpuint(seen.rects, ==, 2);
        g_assert_cmpuint(seen.groups, ==, 1);
        g_assert_cmpstr(seen.fill, ==, "#22b573");
        g_assert_null(seen.style);
        g_assert_nonnull(strstr(svg, "width=\"48\""));
        g_assert_nonnull(strstr(svg, "class=\"face-eyes\""));
        g_free(seen.fill);
    }

    {
        guint32 seed = clawt_face_seed("hornbill");
        g_autofree gchar *svg = clawt_face_svg(0, "#fff", 32, NULL, seed,
                                               TRUE);
        g_autofree gchar *gap = g_strdup_printf(
            "--blink-gap:%ums", clawt_face_blink_period(seed));
        SvgSeen seen = { 0 };

        parse_svg(svg, &seen);
        g_assert_nonnull(seen.style);
        g_assert_nonnull(strstr(seen.style, gap));
        g_assert_nonnull(strstr(seen.style, "--blink-delay:-"));
        g_free(seen.style);
        g_free(seen.fill);
    }
}

/*
 * The fill is written into markup.  Anything that is not a plain hex
 * colour is refused, so a colour typed into YAML can never close the
 * attribute and inject an element into the web client's page.
 */
static void
test_svg_refuses_bad_fill(void)
{
    g_assert_null(clawt_face_svg(0, "red\"/><script>alert(1)</script>", 32,
                                 NULL, 0, FALSE));
    g_assert_null(clawt_face_svg(0, "red", 32, NULL, 0, FALSE));
    g_assert_null(clawt_face_svg(0, "#12345", 32, NULL, 0, FALSE));
    g_assert_null(clawt_face_svg(0, NULL, 32, NULL, 0, FALSE));
}

/*
 * Numbers in the path use '.', whatever the locale: under de_DE a
 * printf'd 0.5 is "0,5", and the browser stops drawing at the comma.
 */
static void
test_svg_locale(void)
{
    g_autofree gchar *before = clawt_face_svg(3, "#2f7de1", 32, NULL, 0,
                                              FALSE);
    g_autofree gchar *after = NULL;
    g_autofree gchar *saved = g_strdup(setlocale(LC_NUMERIC, NULL));

    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") == NULL &&
        setlocale(LC_NUMERIC, "fr_FR.UTF-8") == NULL) {
        g_test_skip("no locale with a decimal comma is installed");
        return;
    }

    after = clawt_face_svg(3, "#2f7de1", 32, NULL, 0, FALSE);
    setlocale(LC_NUMERIC, saved);
    g_assert_cmpstr(before, ==, after);
}

/* A pose is drawn: looking right moves the rects' x in the markup. */
static void
test_svg_pose(void)
{
    ClawtFacePose right = { 1.0, 0.0, 0.0 };
    g_autofree gchar *rest = clawt_face_svg(0, "#fff", 32, NULL, 0, FALSE);
    g_autofree gchar *look = clawt_face_svg(0, "#fff", 32, &right, 0, FALSE);
    const gchar *a = strstr(rest, "<rect");
    const gchar *b = strstr(look, "<rect");

    g_assert_nonnull(a);
    g_assert_nonnull(b);
    g_assert_cmpstr(a, !=, b);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/face/names", test_names);
    g_test_add_func("/face/valid-values", test_valid_values);
    g_test_add_func("/face/resolve/rules", test_resolve_rules);
    g_test_add_func("/face/resolve/spread", test_resolve_spread);
    g_test_add_func("/face/resolve/independent-of-tone",
                    test_shape_independent_of_tone);
    g_test_add_func("/face/outline/bounds", test_outline_bounds);
    g_test_add_func("/face/outline/closed", test_outline_closed);
    g_test_add_func("/face/eyes/stay-inside", test_eyes_stay_inside);
    g_test_add_func("/face/eyes/look-clamped", test_look_clamped);
    g_test_add_func("/face/eyes/blink-shape", test_blink_shape);
    g_test_add_func("/face/look-at", test_look_at);
    g_test_add_func("/face/ease", test_ease);
    g_test_add_func("/face/blink/schedule", test_blink_schedule);
    g_test_add_func("/face/blink/negative-time", test_blink_negative_time);
    g_test_add_func("/face/blink/not-in-step", test_blink_not_in_step);
    g_test_add_func("/face/blink/next", test_next_blink);
    g_test_add_func("/face/eye-color", test_eye_color);
    g_test_add_func("/face/svg/shape", test_svg);
    g_test_add_func("/face/svg/refuses-bad-fill", test_svg_refuses_bad_fill);
    g_test_add_func("/face/svg/locale", test_svg_locale);
    g_test_add_func("/face/svg/pose", test_svg_pose);

    return g_test_run();
}
