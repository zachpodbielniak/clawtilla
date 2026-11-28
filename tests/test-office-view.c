/*
 * test-office-view.c - The arithmetic of a drawn office in a view
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the GTK client and clawtilla-office agree on, checked without a
 * display or a GL context: how tall the view is on a page, the device
 * size the renderer is asked for at 1080p and 4K, the map's zoom and
 * centre, where name labels go, and the control line both ends speak --
 * including what happens when the renderer is gone and a line is still
 * written to its stdin.
 */

#include "clawtilla.h"

#include <math.h>
#include <signal.h>
#include <string.h>

/* ── Size ─────────────────────────────────────────────────────────── */

/*
 * Three quarters of the visible page, never under the minimum, and the
 * fallback before the page is known.  The old view was 420 high on any
 * window: a 1080p window's page is ~900, so the drawing is now ~675.
 */
static void
test_height_for_page(void)
{
    g_assert_cmpint(clawt_office_view_height_for_page(0), ==,
                    CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT);
    g_assert_cmpint(clawt_office_view_height_for_page(-5), ==,
                    CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT);
    g_assert_cmpint(clawt_office_view_height_for_page(900), ==, 675);
    g_assert_cmpint(clawt_office_view_height_for_page(2000), ==, 1500);
    /* a short window scrolls the page rather than squash the office */
    g_assert_cmpint(clawt_office_view_height_for_page(300), ==, CLAWT_OFFICE_VIEW_MIN_HEIGHT);
    g_assert_cmpint(clawt_office_view_height_for_page(426), ==, CLAWT_OFFICE_VIEW_MIN_HEIGHT);
    g_assert_cmpint(clawt_office_view_height_for_page(428), ==, 321);
}

/* Logical size times the device scale: one frame pixel per screen pixel. */
static void
test_device_size_scales(void)
{
    guint w = 0;
    guint h = 0;

    /* a 1080p monitor at scale 1 */
    g_assert_true(clawt_office_view_device_size(1600, 675, 1, &w, &h));
    g_assert_cmpuint(w, ==, 1600);
    g_assert_cmpuint(h, ==, 675);

    /* a 4K monitor at scale 2: the same logical view, twice the pixels */
    g_assert_true(clawt_office_view_device_size(1600, 675, 2, &w, &h));
    g_assert_cmpuint(w, ==, 3200);
    g_assert_cmpuint(h, ==, 1350);

    /* a scale under 1 is 1, never a smaller frame than the view */
    g_assert_true(clawt_office_view_device_size(800, 450, 0, &w, &h));
    g_assert_cmpuint(w, ==, 800);
    g_assert_cmpuint(h, ==, 450);
}

/* Past the renderer's limit both sides shrink by one factor; tiny sides grow. */
static void
test_device_size_clamps(void)
{
    guint w = 7;
    guint h = 7;

    g_assert_true(clawt_office_view_device_size(5120, 1440, 2, &w, &h));
    g_assert_cmpuint(w, ==, CLAWT_OFFICE_VIEW_MAX_DIMENSION);
    g_assert_cmpuint(h, ==, 2304);  /* 2880 * 8192 / 10240: the shape is kept */

    g_assert_true(clawt_office_view_device_size(10, 30, 1, &w, &h));
    g_assert_cmpuint(w, ==, CLAWT_OFFICE_VIEW_MIN_DIMENSION);
    g_assert_cmpuint(h, ==, CLAWT_OFFICE_VIEW_MIN_DIMENSION);

    /* nothing laid out yet: nothing to ask for, and the outs are left alone */
    w = 7;
    h = 9;
    g_assert_false(clawt_office_view_device_size(0, 400, 2, &w, &h));
    g_assert_false(clawt_office_view_device_size(400, -1, 2, &w, &h));
    g_assert_cmpuint(w, ==, 7);
    g_assert_cmpuint(h, ==, 9);
}

static void
test_ui_scale_clamped(void)
{
    g_assert_cmpint(clawt_office_view_clamp_ui_scale(0), ==, 1);
    g_assert_cmpint(clawt_office_view_clamp_ui_scale(-3), ==, 1);
    g_assert_cmpint(clawt_office_view_clamp_ui_scale(2), ==, 2);
    g_assert_cmpint(clawt_office_view_clamp_ui_scale(9), ==, CLAWT_OFFICE_VIEW_MAX_UI_SCALE);
}

/* ── Zoom ─────────────────────────────────────────────────────────── */

/*
 * modern-office is 1408 x 1280.  Fit is the fraction that fits, aspect
 * kept, including between 1 and 2.  Pixel-perfect is that fraction
 * floored once it is at least 1, which is what Fit used to do.  Below 1
 * both stay fractional.  1x and 2x are exactly 1 and 2 logical pixels.
 */
static void
test_fit_zoom_is_fractional(void)
{
    gfloat z;

    /* below one: the fraction, for Fit and for Pixel-perfect */
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_FIT, 1, 1600.0f, 675.0f,
                                   1408.0f, 1280.0f);
    g_assert_cmpfloat_with_epsilon(z, 675.0f / 1280.0f, 1e-6);
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_PIXEL, 1, 1600.0f, 675.0f,
                                   1408.0f, 1280.0f);
    g_assert_cmpfloat_with_epsilon(z, 675.0f / 1280.0f, 1e-6);

    /* a 4K view at scale 2 fits at about 1.05: Fit keeps it, pixel floors */
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_FIT, 2, 3200.0f, 1350.0f,
                                   1408.0f, 1280.0f);
    g_assert_cmpfloat_with_epsilon(z, 1350.0f / 1280.0f, 1e-5);
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_PIXEL, 2, 3200.0f, 1350.0f,
                                   1408.0f, 1280.0f);
    g_assert_cmpfloat(z, ==, 1.0f);

    /* the ticket's 1.7, and the 2294x2231 window that showed the bug */
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_FIT, 1, 1700.0f, 1700.0f,
                                   1000.0f, 1000.0f);
    g_assert_cmpfloat_with_epsilon(z, 1.7f, 1e-5);
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_PIXEL, 1, 1700.0f, 1700.0f,
                                   1000.0f, 1000.0f);
    g_assert_cmpfloat(z, ==, 1.0f);
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_FIT, 1, 2294.0f, 2231.0f,
                                   1408.0f, 1280.0f);
    g_assert_cmpfloat_with_epsilon(z, 2294.0f / 1408.0f, 1e-4);

    /* a wide view of a short map: Fit is 2.7, pixel-perfect is 2 */
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_FIT, 2, 3840.0f, 2000.0f,
                                   1408.0f, 640.0f);
    g_assert_cmpfloat_with_epsilon(z, 3840.0f / 1408.0f, 1e-4);
    z = clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_PIXEL, 2, 3840.0f, 2000.0f,
                                   1408.0f, 640.0f);
    g_assert_cmpfloat(z, ==, 2.0f);

    /* 1x and 2x, exactly, whatever the view */
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_1X, 1, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 1.0f);
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_1X, 2, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 2.0f);
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_2X, 1, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 2.0f);
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_2X, 2, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 4.0f);

    /* no map yet, or no view: 1, never a division by zero */
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_FIT, 1, 800.0f, 600.0f,
                                                 0.0f, 0.0f), ==, 1.0f);
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_FIT, 1, 0.0f, 0.0f,
                                                 100.0f, 100.0f), ==, 1.0f);
}

/* 1x and 2x are logical pixels per map pixel, whatever the view. */
static void
test_fixed_zoom_follows_ui_scale(void)
{
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_1X, 1, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 1.0f);
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_1X, 2, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 2.0f);
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_2X, 1, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 2.0f);
    g_assert_cmpfloat(clawt_office_view_map_zoom(CLAWT_OFFICE_ZOOM_2X, 2, 100.0f, 100.0f,
                                                 1408.0f, 1280.0f), ==, 4.0f);
}

/* A zoomed map never shows past its edge; a small one is centred. */
static void
test_clamp_centre(void)
{
    gfloat cx;
    gfloat cy;

    /* 1408 x 1280 at 2x in an 800 x 600 view: 400 x 300 map pixels show */
    cx = 0.0f;
    cy = 5000.0f;
    clawt_office_view_clamp_centre(800.0f, 600.0f, 1408.0f, 1280.0f, 2.0f, &cx, &cy);
    g_assert_cmpfloat(cx, ==, 200.0f);
    g_assert_cmpfloat(cy, ==, 1280.0f - 150.0f);

    /* somebody in the middle stays in the middle */
    cx = 700.0f;
    cy = 640.0f;
    clawt_office_view_clamp_centre(800.0f, 600.0f, 1408.0f, 1280.0f, 2.0f, &cx, &cy);
    g_assert_cmpfloat(cx, ==, 700.0f);
    g_assert_cmpfloat(cy, ==, 640.0f);

    /* each axis alone: wider than the view across, shorter than it down */
    cx = 0.0f;
    cy = 0.0f;
    clawt_office_view_clamp_centre(800.0f, 2000.0f, 1408.0f, 640.0f, 1.0f, &cx, &cy);
    g_assert_cmpfloat(cx, ==, 400.0f);
    g_assert_cmpfloat(cy, ==, 320.0f);
}

/* ── Labels ───────────────────────────────────────────────────────── */

static ClawtOfficeLabel
label_at(gfloat x, gfloat y, gfloat w, gfloat h, gboolean pinned)
{
    ClawtOfficeLabel l;

    memset(&l, 0, sizeof(l));
    l.x = x;
    l.y = y;
    l.width = w;
    l.height = h;
    l.pinned = pinned;
    /* garbage the arrangement must overwrite */
    l.dy = 99.0f;
    l.shown = TRUE;
    return l;
}

static gboolean
overlap(const ClawtOfficeLabel *a, const ClawtOfficeLabel *b)
{
    return a->x < b->x + b->width && b->x < a->x + a->width &&
           a->y + a->dy < b->y + b->dy + b->height && b->y + b->dy < a->y + a->dy + a->height;
}

/* Labels apart from each other stay exactly where they were. */
static void
test_labels_apart_stay(void)
{
    ClawtOfficeLabel labels[2];

    labels[0] = label_at(0.0f, 100.0f, 60.0f, 14.0f, FALSE);
    labels[1] = label_at(200.0f, 100.0f, 60.0f, 14.0f, FALSE);
    g_assert_cmpuint(clawt_office_labels_arrange(labels, 2, 2.0f, 2, 1000.0f), ==, 2);
    g_assert_true(labels[0].shown && labels[1].shown);
    g_assert_cmpfloat(labels[0].dy, ==, 0.0f);
    g_assert_cmpfloat(labels[1].dy, ==, 0.0f);
}

/*
 * Three people at adjacent desks, their plates on top of each other --
 * "aye-a elephant neyguide": the first keeps its place and the others
 * stack just below it, a pixel off each other's anchors or not, none
 * covering another.
 */
static void
test_labels_crowded_stack(void)
{
    ClawtOfficeLabel labels[3];
    guint i;
    guint j;

    labels[0] = label_at(100.0f, 100.0f, 60.0f, 14.0f, FALSE);
    labels[1] = label_at(120.0f, 101.0f, 60.0f, 14.0f, FALSE);
    labels[2] = label_at(140.0f, 99.0f, 60.0f, 14.0f, FALSE);
    g_assert_cmpuint(clawt_office_labels_arrange(labels, 3, 2.0f, 2, 1000.0f), ==, 3);
    g_assert_cmpfloat(labels[0].dy, ==, 0.0f);
    g_assert_cmpfloat(labels[1].dy, ==, 15.0f);   /* 100 + 14 + 2 - 101 */
    g_assert_cmpfloat(labels[2].dy, ==, 33.0f);   /* below both: 116 + 14 + 2 - 99 */
    for (i = 0; i < 3; i++)
        for (j = i + 1; j < 3; j++)
            g_assert_false(overlap(&labels[i], &labels[j]));
}

/*
 * Too many for the rows allowed: the first ones win, the rest wait for a
 * hover.  Nothing shown covers anything else shown.
 */
static void
test_labels_too_many_hide(void)
{
    ClawtOfficeLabel labels[5];
    guint i;
    guint j;

    for (i = 0; i < 5; i++)
        labels[i] = label_at(100.0f + (gfloat)i, 100.0f, 60.0f, 14.0f, FALSE);
    g_assert_cmpuint(clawt_office_labels_arrange(labels, 5, 2.0f, 2, 1000.0f), ==, 3);
    g_assert_true(labels[0].shown && labels[1].shown && labels[2].shown);
    g_assert_false(labels[3].shown);
    g_assert_false(labels[4].shown);
    for (i = 0; i < 5; i++)
        for (j = i + 1; j < 5; j++)
            if (labels[i].shown && labels[j].shown)
                g_assert_false(overlap(&labels[i], &labels[j]));
}

/*
 * The hovered (pinned) label is placed first, where it is, even when it
 * comes last; whoever was in its way moves.
 */
static void
test_labels_pinned_wins(void)
{
    ClawtOfficeLabel labels[2];

    labels[0] = label_at(100.0f, 100.0f, 60.0f, 14.0f, FALSE);
    labels[1] = label_at(110.0f, 100.0f, 60.0f, 14.0f, TRUE);
    g_assert_cmpuint(clawt_office_labels_arrange(labels, 2, 2.0f, 2, 1000.0f), ==, 2);
    g_assert_cmpfloat(labels[1].dy, ==, 0.0f);
    g_assert_cmpfloat(labels[0].dy, ==, 16.0f);

    /* pinned is shown even with no room for it at all */
    labels[0] = label_at(100.0f, 100.0f, 60.0f, 14.0f, TRUE);
    labels[1] = label_at(100.0f, 100.0f, 60.0f, 14.0f, TRUE);
    g_assert_cmpuint(clawt_office_labels_arrange(labels, 2, 2.0f, 0, 1000.0f), ==, 2);
}

/* A label is not pushed off the bottom of the view: it waits for a hover. */
static void
test_labels_bottom_edge(void)
{
    ClawtOfficeLabel labels[2];

    labels[0] = label_at(100.0f, 580.0f, 60.0f, 14.0f, FALSE);
    labels[1] = label_at(110.0f, 580.0f, 60.0f, 14.0f, FALSE);
    g_assert_cmpuint(clawt_office_labels_arrange(labels, 2, 2.0f, 2, 600.0f), ==, 1);
    g_assert_true(labels[0].shown);
    g_assert_false(labels[1].shown);

    /* nothing to arrange is not an error */
    g_assert_cmpuint(clawt_office_labels_arrange(NULL, 0, 2.0f, 2, 600.0f), ==, 0);
}

/* ── Names and the control line ───────────────────────────────────── */

static void
test_nicks(void)
{
    ClawtOfficeViewMode mode = CLAWT_OFFICE_VIEW_2D;
    ClawtOfficeZoom zoom = CLAWT_OFFICE_ZOOM_FIT;

    g_assert_cmpstr(clawt_office_view_mode_nick(CLAWT_OFFICE_VIEW_2D), ==, "2d");
    g_assert_cmpstr(clawt_office_view_mode_nick(CLAWT_OFFICE_VIEW_3D), ==, "3d");
    g_assert_true(clawt_office_view_mode_from_nick("3D", &mode));
    g_assert_cmpint(mode, ==, CLAWT_OFFICE_VIEW_3D);
    g_assert_false(clawt_office_view_mode_from_nick("4d", &mode));
    g_assert_false(clawt_office_view_mode_from_nick(NULL, &mode));
    g_assert_cmpint(mode, ==, CLAWT_OFFICE_VIEW_3D);

    g_assert_cmpstr(clawt_office_zoom_nick(CLAWT_OFFICE_ZOOM_FIT), ==, "fit");
    g_assert_cmpstr(clawt_office_zoom_nick(CLAWT_OFFICE_ZOOM_1X), ==, "1x");
    g_assert_cmpstr(clawt_office_zoom_nick(CLAWT_OFFICE_ZOOM_2X), ==, "2x");
    g_assert_cmpstr(clawt_office_zoom_nick(CLAWT_OFFICE_ZOOM_PIXEL), ==, "pixel");
    g_assert_true(clawt_office_zoom_from_nick("Pixel-Perfect", &zoom));
    g_assert_cmpint(zoom, ==, CLAWT_OFFICE_ZOOM_PIXEL);
    g_assert_true(clawt_office_zoom_from_nick("2X", &zoom));
    g_assert_cmpint(zoom, ==, CLAWT_OFFICE_ZOOM_2X);
    g_assert_false(clawt_office_zoom_from_nick("3x", &zoom));
    g_assert_cmpint(zoom, ==, CLAWT_OFFICE_ZOOM_2X);

    {
        ClawtOfficeUsagePlacement usage = CLAWT_OFFICE_USAGE_WORLD;

        g_assert_cmpstr(clawt_office_usage_placement_nick(CLAWT_OFFICE_USAGE_WORLD), ==,
                        "world");
        g_assert_cmpstr(clawt_office_usage_placement_nick(CLAWT_OFFICE_USAGE_CORNER), ==,
                        "corner");
        g_assert_cmpstr(clawt_office_usage_placement_nick(CLAWT_OFFICE_USAGE_OFF), ==, "off");
        g_assert_true(clawt_office_usage_placement_from_nick("Corner", &usage));
        g_assert_cmpint(usage, ==, CLAWT_OFFICE_USAGE_CORNER);
        g_assert_false(clawt_office_usage_placement_from_nick("wall", &usage));
        g_assert_false(clawt_office_usage_placement_from_nick(NULL, &usage));
        g_assert_cmpint(usage, ==, CLAWT_OFFICE_USAGE_CORNER);
    }
}

/* What the host writes is exactly what the renderer reads back. */
static void
test_control_round_trip(void)
{
    g_autofree gchar *line = clawt_office_view_control_line(CLAWT_OFFICE_VIEW_3D, "chief",
                                                            CLAWT_OFFICE_ZOOM_2X, 2,
                                                            CLAWT_OFFICE_USAGE_CORNER);
    g_autofree gchar *nobody = clawt_office_view_control_line(CLAWT_OFFICE_VIEW_2D, NULL,
                                                              CLAWT_OFFICE_ZOOM_FIT, 9,
                                                              CLAWT_OFFICE_USAGE_WORLD);
    g_autofree gchar *bare = NULL;
    ClawtOfficeViewControl control;
    g_autoptr(GError) error = NULL;

    g_assert_cmpstr(line, ==,
                    "{\"mode\":\"3d\",\"focus\":\"chief\",\"zoom\":\"2x\",\"ui_scale\":2,"
                    "\"usage\":\"corner\"}\n");
    bare = g_strndup(line, strlen(line) - 1);
    g_assert_true(clawt_office_view_control_parse(bare, &control, &error));
    g_assert_no_error(error);
    g_assert_true(control.has_mode);
    g_assert_cmpint(control.mode, ==, CLAWT_OFFICE_VIEW_3D);
    g_assert_true(control.has_focus);
    g_assert_cmpstr(control.focus, ==, "chief");
    g_assert_true(control.has_zoom);
    g_assert_cmpint(control.zoom, ==, CLAWT_OFFICE_ZOOM_2X);
    g_assert_cmpint(control.ui_scale, ==, 2);
    g_assert_true(control.has_usage);
    g_assert_cmpint(control.usage, ==, CLAWT_OFFICE_USAGE_CORNER);
    clawt_office_view_control_clear(&control);
    g_assert_null(control.focus);

    /* nobody is "", and a scale past the limit is the limit */
    g_assert_nonnull(strstr(nobody, "\"focus\":\"\""));
    g_assert_nonnull(strstr(nobody, "\"ui_scale\":4"));
    g_assert_true(clawt_office_view_control_parse(nobody, &control, &error));
    g_assert_true(control.has_focus);
    g_assert_null(control.focus);
    clawt_office_view_control_clear(&control);
}

/* Each member is optional; unknown members are ignored, for newer hosts. */
static void
test_control_partial(void)
{
    ClawtOfficeViewControl control;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_office_view_control_parse("{\"zoom\":\"1x\",\"later\":[1,2]}",
                                                  &control, &error));
    g_assert_no_error(error);
    g_assert_false(control.has_mode);
    g_assert_false(control.has_focus);
    g_assert_true(control.has_zoom);
    g_assert_cmpint(control.zoom, ==, CLAWT_OFFICE_ZOOM_1X);
    g_assert_cmpint(control.ui_scale, ==, 0);
    clawt_office_view_control_clear(&control);
    g_assert_true(clawt_office_view_control_parse("{\"zoom\":\"pixel\"}",
                                                  &control, &error));
    g_assert_no_error(error);
    g_assert_cmpint(control.zoom, ==, CLAWT_OFFICE_ZOOM_PIXEL);
    /* an older host says nothing of usage: the renderer keeps what it has */
    g_assert_false(control.has_usage);
    clawt_office_view_control_clear(&control);

    g_assert_true(clawt_office_view_control_parse("{\"usage\":\"OFF\"}", &control, &error));
    g_assert_true(control.has_usage);
    g_assert_cmpint(control.usage, ==, CLAWT_OFFICE_USAGE_OFF);
    clawt_office_view_control_clear(&control);

    g_assert_true(clawt_office_view_control_parse("{\"focus\":null}", &control, &error));
    g_assert_true(control.has_focus);
    g_assert_null(control.focus);
    clawt_office_view_control_clear(&control);
}

/* A line two versions disagree on is refused whole, naming what is wrong. */
static void
test_control_refused(void)
{
    static const gchar *const bad[] = {
        "not json",
        "[1,2]",
        "{\"mode\":\"4d\"}",
        "{\"mode\":3}",
        "{\"zoom\":\"3x\"}",
        "{\"ui_scale\":\"two\"}",
        "{\"ui_scale\":1.5}",
        "{\"focus\":7}",
        "{\"focus\":\"chief\",\"mode\":\"sideways\"}",
        "{\"usage\":\"ceiling\"}",
        "{\"usage\":2}",
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(bad); i++) {
        ClawtOfficeViewControl control;
        g_autoptr(GError) error = NULL;

        g_assert_false(clawt_office_view_control_parse(bad[i], &control, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
        /* refused whole: nothing was kept, so nothing leaks */
        g_assert_null(control.focus);
        g_assert_false(control.has_focus);
    }
}

/* ── A control line to a renderer that is gone ─────────────────────── */

typedef struct {
    GMainLoop *loop;
    GError    *error;
    gboolean   ok;
} Written;

static void
on_written(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Written *written = user_data;

    written->ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL,
                                                   &written->error);
    g_main_loop_quit(written->loop);
}

/*
 * What clawtilla-gtk's send_control() does, minus the widgets: a child
 * spawned with its stdin a pipe, which has already exited, and a control
 * line written to it asynchronously.  The write meets a pipe with no
 * reader.  Returns how the write ended.
 */
static gboolean
write_to_a_dead_child(GError **error)
{
    g_autoptr(GSubprocess) child = NULL;
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_autofree gchar *line = clawt_office_view_control_line(CLAWT_OFFICE_VIEW_3D, "chief",
                                                            CLAWT_OFFICE_ZOOM_FIT, 1,
                                                            CLAWT_OFFICE_USAGE_WORLD);
    g_autoptr(GError) spawn_error = NULL;
    Written written = { NULL, NULL, FALSE };

    child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE, &spawn_error, "true", NULL);
    g_assert_no_error(spawn_error);
    /* gone, and with it the only read end of the pipe */
    g_assert_true(g_subprocess_wait(child, NULL, &spawn_error));
    g_assert_no_error(spawn_error);

    written.loop = loop;
    g_output_stream_write_all_async(g_subprocess_get_stdin_pipe(child), line, strlen(line),
                                    G_PRIORITY_DEFAULT, NULL, on_written, &written);
    g_main_loop_run(loop);
    if (written.error != NULL)
        g_propagate_error(error, written.error);
    return written.ok;
}

/*
 * With SIGPIPE ignored, as clawtilla-gtk's main() now does before
 * anything runs, the write fails with G_IO_ERROR_BROKEN_PIPE -- which the
 * Office page treats as the child on its way out -- and this process is
 * still here to say so.
 *
 * There is no companion test showing the default disposition killing
 * the writer through this path: creating a GSubprocess sets SIGPIPE to
 * ignored itself (GLib's class initialisation, seen on 2.88), so the
 * default cannot be had once the child exists.  main()'s line is there
 * so that is not what the window's survival rests on.
 */
static void
test_control_to_dead_child_is_an_error(void)
{
    g_autoptr(GError) error = NULL;
    void (*was)(gint) = signal(SIGPIPE, SIG_IGN);

    g_assert_false(write_to_a_dead_child(&error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
    signal(SIGPIPE, was);
}

/* ── The 3D camera ────────────────────────────────────────────────── */

/* The 3D view's bearing when nobody has orbited it: office-scene.c's start. */
#define DEFAULT_ORBIT (0.65f)

/* The view sizes the default framing has to hold at: the window, 1080p, 4K, 4:3. */
static const gint view_sizes[][2] = {
    { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 }, { 1024, 768 },
};

/*
 * The projection is raylib's: straight ahead is the middle, the edge of
 * the field of view is the edge of the view, and behind is refused.
 */
static void
test_project_3d(void)
{
    const gfloat eye[3] = { 0.0f, 0.0f, 10.0f };
    const gfloat target[3] = { 0.0f, 0.0f, 0.0f };
    const gfloat ahead[3] = { 0.0f, 0.0f, 0.0f };
    const gfloat right_edge[3] = { 2.0f, 0.0f, 9.0f };
    const gfloat top_edge[3] = { 0.0f, 1.0f, 9.0f };
    const gfloat behind[3] = { 0.0f, 0.0f, 11.0f };
    gfloat x = -1.0f;
    gfloat y = -1.0f;

    /* fovy 90: one unit deep, the edge is one unit up, two across at 2:1 */
    g_assert_true(clawt_office_view_project_3d(eye, target, 90.0f, 200.0f, 100.0f, ahead,
                                               &x, &y));
    g_assert_cmpfloat_with_epsilon(x, 100.0f, 0.001f);
    g_assert_cmpfloat_with_epsilon(y, 50.0f, 0.001f);

    g_assert_true(clawt_office_view_project_3d(eye, target, 90.0f, 200.0f, 100.0f,
                                               right_edge, &x, &y));
    g_assert_cmpfloat_with_epsilon(x, 200.0f, 0.001f);
    g_assert_cmpfloat_with_epsilon(y, 50.0f, 0.001f);

    g_assert_true(clawt_office_view_project_3d(eye, target, 90.0f, 200.0f, 100.0f,
                                               top_edge, &x, &y));
    g_assert_cmpfloat_with_epsilon(x, 100.0f, 0.001f);
    g_assert_cmpfloat_with_epsilon(y, 0.0f, 0.001f);

    x = y = -1.0f;
    g_assert_false(clawt_office_view_project_3d(eye, target, 90.0f, 200.0f, 100.0f, behind,
                                                &x, &y));
    g_assert_cmpfloat(x, ==, -1.0f);
}

/* The board stands upright, square to the camera, on its anchor. */
static void
test_board_corners_3d(void)
{
    const gfloat anchor[3] = { 10.0f, 0.0f, 20.0f };
    gfloat corners[12];

    /* looking along -z, right is +x: the board runs along x, left to right */
    g_assert_true(clawt_office_view_board_corners_3d(anchor, 4.0f, 3.0f, 0.0f, -1.0f,
                                                     corners));
    g_assert_cmpfloat_with_epsilon(corners[0], 8.0f, 0.001f);    /* bottom left */
    g_assert_cmpfloat_with_epsilon(corners[1], 0.2f, 0.001f);
    g_assert_cmpfloat_with_epsilon(corners[2], 20.0f, 0.001f);
    g_assert_cmpfloat_with_epsilon(corners[3], 12.0f, 0.001f);   /* bottom right */
    g_assert_cmpfloat_with_epsilon(corners[5], 20.0f, 0.001f);
    g_assert_cmpfloat_with_epsilon(corners[9], 12.0f, 0.001f);   /* top right */
    g_assert_cmpfloat_with_epsilon(corners[10], 3.2f, 0.001f);

    /* straight down has nothing to face */
    g_assert_false(clawt_office_view_board_corners_3d(anchor, 4.0f, 3.0f, 0.0f, 0.0f,
                                                      corners));
}

/* Where clawt_office_view_frame_3d() put the camera, as eye and target. */
static void
frame_camera(gfloat target_x, gfloat target_z, gfloat reach, gfloat orbit, gfloat eye[3],
             gfloat target[3])
{
    eye[0] = target_x + cosf(orbit) * reach;
    eye[1] = reach * CLAWT_OFFICE_VIEW_3D_LIFT;
    eye[2] = target_z + sinf(orbit) * reach;
    target[0] = target_x;
    target[1] = 0.0f;
    target[2] = target_z;
}

/*
 * Every point in front of the camera and inside the view; and, unless
 * @loose, at least one of them at the margin -- the framing is the
 * closest that holds them, not merely one that does.
 */
static void
assert_framed(const gfloat *points, guint n, const gfloat eye[3], const gfloat target[3],
              gint width, gint height, gboolean loose)
{
    gfloat edge = 1.0f;
    guint i;

    for (i = 0; i < n; i++) {
        gfloat x = 0.0f;
        gfloat y = 0.0f;

        g_assert_true(clawt_office_view_project_3d(eye, target, CLAWT_OFFICE_VIEW_3D_FOVY,
                                                   (gfloat)width, (gfloat)height,
                                                   points + i * 3, &x, &y));
        g_assert_cmpfloat(x, >=, 0.0f);
        g_assert_cmpfloat(x, <=, (gfloat)width);
        g_assert_cmpfloat(y, >=, 0.0f);
        g_assert_cmpfloat(y, <=, (gfloat)height);

        /* how near this point is to an edge, as a share of the half view */
        edge = MIN(edge, MIN(MIN(x, (gfloat)width - x) / ((gfloat)width / 2.0f),
                             MIN(y, (gfloat)height - y) / ((gfloat)height / 2.0f)));
    }
    if (!loose)
        g_assert_cmpfloat(edge, <, CLAWT_OFFICE_VIEW_3D_MARGIN + 0.01f);
}

/* A box of floor 40 x 30 with head height, at every bearing and view shape. */
static void
test_frame_3d_any_bearing(void)
{
    gfloat points[3 * 8];
    gint i;
    gint k;

    for (i = 0; i < 8; i++) {
        points[i * 3 + 0] = (i & 1) ? 40.0f : 0.0f;
        points[i * 3 + 1] = (i & 4) ? 2.6f : 0.0f;
        points[i * 3 + 2] = (i & 2) ? 30.0f : 0.0f;
    }

    for (k = 0; k < 24; k++) {
        gfloat orbit = (gfloat)k * (gfloat)G_PI / 12.0f;
        gsize v;

        for (v = 0; v < G_N_ELEMENTS(view_sizes); v++) {
            gfloat tx = 0.0f;
            gfloat tz = 0.0f;
            gfloat reach = 0.0f;
            gfloat eye[3];
            gfloat target[3];
            gfloat aspect = (gfloat)view_sizes[v][0] / (gfloat)view_sizes[v][1];

            g_assert_true(clawt_office_view_frame_3d(points, 8, orbit,
                                                     CLAWT_OFFICE_VIEW_3D_LIFT,
                                                     CLAWT_OFFICE_VIEW_3D_FOVY, aspect,
                                                     CLAWT_OFFICE_VIEW_3D_MARGIN, &tx, &tz,
                                                     &reach));
            g_assert_cmpfloat(reach, >, 0.0f);
            frame_camera(tx, tz, reach, orbit, eye, target);
            assert_framed(points, 8, eye, target, view_sizes[v][0], view_sizes[v][1],
                          FALSE);
        }
    }

    /* nothing to frame is refused, and says nothing */
    {
        gfloat tx = -1.0f;
        gfloat tz = -1.0f;
        gfloat reach = -1.0f;

        g_assert_false(clawt_office_view_frame_3d(points, 0, 0.0f, 0.95f, 45.0f, 1.0f,
                                                  0.0f, &tx, &tz, &reach));
        g_assert_cmpfloat(reach, ==, -1.0f);
    }
}

/*
 * A shipped world as office-scene.c sees it for the 3D view: the map's
 * size in tiles and the usage board's anchor and size, from office.yaml
 * and map.tmj -- the same rules as board_zone(): rect_2d's centre unless
 * anchors_3d gives one, as wide as rect_2d in tiles and as tall in
 * proportion.
 */
typedef struct {
    gfloat map_w;
    gfloat map_h;
    gfloat anchor[3];
    gfloat board_w;
    gfloat board_h;
} ShippedWorld;

static void
shipped_world(const gchar *name, ShippedWorld *out)
{
    g_autofree gchar *dir = g_build_filename(CLAWT_TEST_FIXTURES, "..", "..", "data",
                                             "office", "worlds", name, NULL);
    g_autofree gchar *tmj = g_build_filename(dir, "map.tmj", NULL);
    g_autoptr(ClawtOfficeWorld) world = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    const ClawtOfficeZone *zone;
    JsonObject *map;
    gfloat tile;

    world = clawt_office_world_load(dir, &error);
    g_assert_no_error(error);
    g_assert_true(json_parser_load_from_file(parser, tmj, &error));
    g_assert_no_error(error);
    map = json_node_get_object(json_parser_get_root(parser));
    tile = (gfloat)json_object_get_int_member(map, "tilewidth");
    out->map_w = (gfloat)json_object_get_int_member(map, "width");
    out->map_h = (gfloat)json_object_get_int_member(map, "height");

    zone = clawt_office_world_find_zone(world, "usage_board", NULL, 0);
    g_assert_nonnull(zone);
    g_assert_true(zone->has_rect_2d);
    out->board_w = (gfloat)zone->rect_2d[2] / tile;
    out->board_h = out->board_w * (gfloat)(zone->rect_2d[3] / zone->rect_2d[2]);
    out->anchor[0] = (gfloat)(zone->rect_2d[0] + zone->rect_2d[2] / 2.0) / tile;
    out->anchor[1] = 0.0f;
    out->anchor[2] = (gfloat)(zone->rect_2d[1] + zone->rect_2d[3] / 2.0) / tile;
    if (zone->anchors_3d != NULL && zone->anchors_3d->len >= 3) {
        out->anchor[0] = (gfloat)g_array_index(zone->anchors_3d, gdouble, 0);
        out->anchor[1] = (gfloat)g_array_index(zone->anchors_3d, gdouble, 1);
        out->anchor[2] = (gfloat)g_array_index(zone->anchors_3d, gdouble, 2);
    }
}

/* Twelve points, as office-scene.c's frame_office_3d() gathers them. */
static void
office_points(const ShippedWorld *w, gfloat orbit, gfloat points[36])
{
    gint i;

    for (i = 0; i < 8; i++) {
        points[i * 3 + 0] = (i & 1) ? w->map_w : 0.0f;
        points[i * 3 + 1] = (i & 4) ? 2.6f : 0.0f;
        points[i * 3 + 2] = (i & 2) ? w->map_h : 0.0f;
    }
    g_assert_true(clawt_office_view_board_corners_3d(w->anchor, w->board_w, w->board_h,
                                                     -cosf(orbit), -sinf(orbit),
                                                     points + 24));
}

/* The screen rectangle the board's four corners cover. */
static void
board_on_screen(const gfloat points[36], const gfloat eye[3], const gfloat target[3],
                gint width, gint height, gfloat rect[4])
{
    gfloat lo_x = G_MAXFLOAT;
    gfloat lo_y = G_MAXFLOAT;
    gfloat hi_x = -G_MAXFLOAT;
    gfloat hi_y = -G_MAXFLOAT;
    gint i;

    for (i = 8; i < 12; i++) {
        gfloat x = 0.0f;
        gfloat y = 0.0f;

        g_assert_true(clawt_office_view_project_3d(eye, target, CLAWT_OFFICE_VIEW_3D_FOVY,
                                                   (gfloat)width, (gfloat)height,
                                                   points + i * 3, &x, &y));
        lo_x = MIN(lo_x, x);
        lo_y = MIN(lo_y, y);
        hi_x = MAX(hi_x, x);
        hi_y = MAX(hi_y, y);
    }
    rect[0] = lo_x;
    rect[1] = lo_y;
    rect[2] = hi_x - lo_x;
    rect[3] = hi_y - lo_y;
}

/*
 * VENTURE #24: with the default 3D camera the modern-office usage board
 * stood below the frame.  In both shipped worlds, at the default bearing
 * and at the window's size, 1080p, 4K and 4:3, the default framing now
 * has the board's projected rectangle inside the view, along with every
 * corner of the floor at head height -- so no desk or person was pushed
 * out to make room.
 *
 * And the old camera, for the record: aimed at the middle of the map
 * from a fixed distance, it put modern-office's board below the frame.
 * If that ever passes, this test is no longer testing the fix.
 */
static void
test_frame_3d_shipped_worlds(void)
{
    static const gchar *const worlds[] = { "modern-office", "cozy-town" };
    gsize n;

    for (n = 0; n < G_N_ELEMENTS(worlds); n++) {
        ShippedWorld w;
        gfloat points[36];
        gsize v;

        shipped_world(worlds[n], &w);
        office_points(&w, DEFAULT_ORBIT, points);

        for (v = 0; v < G_N_ELEMENTS(view_sizes); v++) {
            gint width = view_sizes[v][0];
            gint height = view_sizes[v][1];
            gfloat tx = 0.0f;
            gfloat tz = 0.0f;
            gfloat reach = 0.0f;
            gfloat eye[3];
            gfloat target[3];
            gfloat rect[4];

            g_assert_true(clawt_office_view_frame_3d(points, 12, DEFAULT_ORBIT,
                                                     CLAWT_OFFICE_VIEW_3D_LIFT,
                                                     CLAWT_OFFICE_VIEW_3D_FOVY,
                                                     (gfloat)width / (gfloat)height,
                                                     CLAWT_OFFICE_VIEW_3D_MARGIN, &tx, &tz,
                                                     &reach));
            frame_camera(tx, tz, reach, DEFAULT_ORBIT, eye, target);
            assert_framed(points, 12, eye, target, width, height, FALSE);

            board_on_screen(points, eye, target, width, height, rect);
            g_test_message("%s at %dx%d: reach %.1f, board at %.0f,%.0f %.0fx%.0f",
                           worlds[n], width, height, reach, rect[0], rect[1], rect[2],
                           rect[3]);
            g_assert_cmpfloat(rect[0], >=, 0.0f);
            g_assert_cmpfloat(rect[1], >=, 0.0f);
            g_assert_cmpfloat(rect[0] + rect[2], <=, (gfloat)width);
            g_assert_cmpfloat(rect[1] + rect[3], <=, (gfloat)height);

            /* big enough to be read as a board, not a speck */
            g_assert_cmpfloat(rect[2], >=, (gfloat)width / 20.0f);

            /* and the whole office costs at most a third more distance than the old camera */
            g_assert_cmpfloat(reach, <=, MAX(w.map_w, w.map_h) / 2.0f * 1.2f * 1.34f);
        }

        /* the camera this replaced */
        if (g_strcmp0(worlds[n], "modern-office") == 0) {
            gfloat old = MAX(w.map_w, w.map_h) / 2.0f * 1.2f;
            gfloat eye[3];
            gfloat target[3];
            gfloat rect[4];

            frame_camera(w.map_w / 2.0f, w.map_h / 2.0f, old, DEFAULT_ORBIT, eye, target);
            board_on_screen(points, eye, target, 1280, 720, rect);
            g_assert_cmpfloat(rect[1] + rect[3], >, 720.0f);
        }
    }
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/office-view/height-for-page", test_height_for_page);
    g_test_add_func("/office-view/device-size/scales", test_device_size_scales);
    g_test_add_func("/office-view/device-size/clamps", test_device_size_clamps);
    g_test_add_func("/office-view/ui-scale", test_ui_scale_clamped);
    g_test_add_func("/office-view/zoom/fit-whole", test_fit_zoom_is_fractional);
    g_test_add_func("/office-view/zoom/fixed", test_fixed_zoom_follows_ui_scale);
    g_test_add_func("/office-view/zoom/clamp-centre", test_clamp_centre);
    g_test_add_func("/office-view/labels/apart", test_labels_apart_stay);
    g_test_add_func("/office-view/labels/crowded", test_labels_crowded_stack);
    g_test_add_func("/office-view/labels/too-many", test_labels_too_many_hide);
    g_test_add_func("/office-view/labels/pinned", test_labels_pinned_wins);
    g_test_add_func("/office-view/labels/bottom", test_labels_bottom_edge);
    g_test_add_func("/office-view/nicks", test_nicks);
    g_test_add_func("/office-view/control/round-trip", test_control_round_trip);
    g_test_add_func("/office-view/control/partial", test_control_partial);
    g_test_add_func("/office-view/control/refused", test_control_refused);
    g_test_add_func("/office-view/control/dead-child-broken-pipe",
                    test_control_to_dead_child_is_an_error);
    g_test_add_func("/office-view/3d/project", test_project_3d);
    g_test_add_func("/office-view/3d/board-corners", test_board_corners_3d);
    g_test_add_func("/office-view/3d/frame-any-bearing", test_frame_3d_any_bearing);
    g_test_add_func("/office-view/3d/frame-shipped-worlds", test_frame_3d_shipped_worlds);

    return g_test_run();
}
