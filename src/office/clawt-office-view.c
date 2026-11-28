/*
 * clawt-office-view.c - The arithmetic of a drawn office in a view
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Pure functions, no toolkit and no GL: the GTK client sizes its view and
 * writes control lines with them, clawtilla-office zooms its map, places
 * its name labels and reads those lines with them, and the tests drive
 * both halves.
 */

#include "clawt-office-view.h"
#include "clawt-error.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

/* ── Names ────────────────────────────────────────────────────────── */

const gchar *
clawt_office_view_mode_nick(ClawtOfficeViewMode mode)
{
    return mode == CLAWT_OFFICE_VIEW_3D ? "3d" : "2d";
}

gboolean
clawt_office_view_mode_from_nick(const gchar *nick, ClawtOfficeViewMode *out)
{
    g_return_val_if_fail(out != NULL, FALSE);

    if (nick == NULL)
        return FALSE;
    if (g_ascii_strcasecmp(nick, "2d") == 0) {
        *out = CLAWT_OFFICE_VIEW_2D;
        return TRUE;
    }
    if (g_ascii_strcasecmp(nick, "3d") == 0) {
        *out = CLAWT_OFFICE_VIEW_3D;
        return TRUE;
    }
    return FALSE;
}

const gchar *
clawt_office_zoom_nick(ClawtOfficeZoom zoom)
{
    switch (zoom) {
    case CLAWT_OFFICE_ZOOM_1X:    return "1x";
    case CLAWT_OFFICE_ZOOM_2X:    return "2x";
    case CLAWT_OFFICE_ZOOM_PIXEL: return "pixel";
    case CLAWT_OFFICE_ZOOM_FIT:
    default:                      return "fit";
    }
}

gboolean
clawt_office_zoom_from_nick(const gchar *nick, ClawtOfficeZoom *out)
{
    g_return_val_if_fail(out != NULL, FALSE);

    if (nick == NULL)
        return FALSE;
    if (g_ascii_strcasecmp(nick, "fit") == 0) {
        *out = CLAWT_OFFICE_ZOOM_FIT;
        return TRUE;
    }
    if (g_ascii_strcasecmp(nick, "1x") == 0) {
        *out = CLAWT_OFFICE_ZOOM_1X;
        return TRUE;
    }
    if (g_ascii_strcasecmp(nick, "2x") == 0) {
        *out = CLAWT_OFFICE_ZOOM_2X;
        return TRUE;
    }
    /* the button says Pixel-perfect; the file and the flag say pixel */
    if (g_ascii_strcasecmp(nick, "pixel") == 0 ||
        g_ascii_strcasecmp(nick, "pixel-perfect") == 0) {
        *out = CLAWT_OFFICE_ZOOM_PIXEL;
        return TRUE;
    }
    return FALSE;
}

const gchar *
clawt_office_usage_placement_nick(ClawtOfficeUsagePlacement placement)
{
    switch (placement) {
    case CLAWT_OFFICE_USAGE_CORNER: return "corner";
    case CLAWT_OFFICE_USAGE_OFF:    return "off";
    case CLAWT_OFFICE_USAGE_WORLD:
    default:                        return "world";
    }
}

gboolean
clawt_office_usage_placement_from_nick(const gchar *nick, ClawtOfficeUsagePlacement *out)
{
    g_return_val_if_fail(out != NULL, FALSE);

    if (nick == NULL)
        return FALSE;
    if (g_ascii_strcasecmp(nick, "world") == 0) {
        *out = CLAWT_OFFICE_USAGE_WORLD;
        return TRUE;
    }
    if (g_ascii_strcasecmp(nick, "corner") == 0) {
        *out = CLAWT_OFFICE_USAGE_CORNER;
        return TRUE;
    }
    if (g_ascii_strcasecmp(nick, "off") == 0) {
        *out = CLAWT_OFFICE_USAGE_OFF;
        return TRUE;
    }
    return FALSE;
}

/* ── Size ─────────────────────────────────────────────────────────── */

gint
clawt_office_view_height_for_page(gint page_height)
{
    if (page_height <= 0)
        return CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT;
    /* integer arithmetic: the same height for the same page, every time */
    return MAX(CLAWT_OFFICE_VIEW_MIN_HEIGHT, page_height * 3 / 4);
}

gboolean
clawt_office_view_device_size(gint width, gint height, gint scale,
                              guint *out_width, guint *out_height)
{
    gdouble w;
    gdouble h;
    gdouble longest;

    g_return_val_if_fail(out_width != NULL && out_height != NULL, FALSE);

    if (width <= 0 || height <= 0)
        return FALSE;
    w = (gdouble)width * (gdouble)MAX(scale, 1);
    h = (gdouble)height * (gdouble)MAX(scale, 1);

    /* too big for the renderer: both sides down by one factor, keeping the shape */
    longest = MAX(w, h);
    if (longest > CLAWT_OFFICE_VIEW_MAX_DIMENSION) {
        w = floor(w * CLAWT_OFFICE_VIEW_MAX_DIMENSION / longest);
        h = floor(h * CLAWT_OFFICE_VIEW_MAX_DIMENSION / longest);
    }
    *out_width = (guint)CLAMP(w, CLAWT_OFFICE_VIEW_MIN_DIMENSION,
                              CLAWT_OFFICE_VIEW_MAX_DIMENSION);
    *out_height = (guint)CLAMP(h, CLAWT_OFFICE_VIEW_MIN_DIMENSION,
                               CLAWT_OFFICE_VIEW_MAX_DIMENSION);
    return TRUE;
}

gint
clawt_office_view_clamp_ui_scale(gint ui_scale)
{
    return CLAMP(ui_scale, 1, CLAWT_OFFICE_VIEW_MAX_UI_SCALE);
}

/* ── Zoom ─────────────────────────────────────────────────────────── */

gfloat
clawt_office_view_map_zoom(ClawtOfficeZoom zoom, gint ui_scale, gfloat view_width,
                           gfloat view_height, gfloat map_width, gfloat map_height)
{
    gfloat fit;
    gint scale = clawt_office_view_clamp_ui_scale(ui_scale);

    switch (zoom) {
    case CLAWT_OFFICE_ZOOM_1X:
        return (gfloat)scale;
    case CLAWT_OFFICE_ZOOM_2X:
        return 2.0f * (gfloat)scale;
    case CLAWT_OFFICE_ZOOM_PIXEL:
    case CLAWT_OFFICE_ZOOM_FIT:
        break;
    default:
        zoom = CLAWT_OFFICE_ZOOM_FIT;
        break;
    }

    if (map_width <= 0.0f || map_height <= 0.0f || view_width <= 0.0f ||
        view_height <= 0.0f)
        return 1.0f;
    fit = MIN(view_width / map_width, view_height / map_height);

    /*
     * Pixel-perfect keeps the old Fit: a whole number when the map fits
     * at one or more, because a 2.6x map has some pixels three wide and
     * some two, which shimmers when anything walks.  Below one there is
     * no whole number to take.  Fit itself is the fraction, so the map
     * fills the view between 1x and 2x; the camera target is snapped to
     * a screen pixel instead (office-scene.c) so the art does not crawl.
     */
    if (zoom == CLAWT_OFFICE_ZOOM_PIXEL && fit >= 1.0f)
        return floorf(fit);
    return fit;
}

void
clawt_office_view_clamp_centre(gfloat view_width, gfloat view_height, gfloat map_width,
                               gfloat map_height, gfloat zoom, gfloat *centre_x,
                               gfloat *centre_y)
{
    gfloat half;

    g_return_if_fail(centre_x != NULL && centre_y != NULL);

    if (zoom <= 0.0f)
        zoom = 1.0f;

    /* each axis alone: a wide map in a tall view scrolls across, not down */
    if (map_width * zoom <= view_width) {
        *centre_x = map_width / 2.0f;
    } else {
        half = view_width / (2.0f * zoom);
        *centre_x = CLAMP(*centre_x, half, map_width - half);
    }
    if (map_height * zoom <= view_height) {
        *centre_y = map_height / 2.0f;
    } else {
        half = view_height / (2.0f * zoom);
        *centre_y = CLAMP(*centre_y, half, map_height - half);
    }
}

/* ── Labels ───────────────────────────────────────────────────────── */

/* Whether two boxes come closer than @gap on both axes. */
static gboolean
boxes_crowd(gfloat ax, gfloat ay, gfloat aw, gfloat ah,
            gfloat bx, gfloat by, gfloat bw, gfloat bh, gfloat gap)
{
    return ax < bx + bw + gap && bx < ax + aw + gap &&
           ay < by + bh + gap && by < ay + ah + gap;
}

/*
 * How far down @self must go, from where it is, to clear every shown
 * label it would crowd at @dy: just below the lowest of them.  0 when it
 * crowds none.
 */
static gfloat
clear_of_shown(ClawtOfficeLabel *labels, guint n_labels, guint self, gfloat dy, gfloat gap)
{
    ClawtOfficeLabel *l = &labels[self];
    gfloat needed = 0.0f;
    guint j;

    for (j = 0; j < n_labels; j++) {
        ClawtOfficeLabel *other = &labels[j];
        gfloat below;

        if (j == self || !other->shown)
            continue;
        if (!boxes_crowd(l->x, l->y + dy, l->width, l->height,
                         other->x, other->y + other->dy, other->width, other->height, gap))
            continue;
        below = other->y + other->dy + other->height + gap - l->y;
        needed = MAX(needed, below);
    }
    return needed;
}

guint
clawt_office_labels_arrange(ClawtOfficeLabel *labels, guint n_labels, gfloat gap,
                            guint max_steps, gfloat bottom)
{
    guint shown = 0;
    guint i;

    g_return_val_if_fail(labels != NULL || n_labels == 0, 0);

    for (i = 0; i < n_labels; i++) {
        labels[i].dy = 0.0f;
        labels[i].shown = FALSE;
    }

    /* pinned first, where they are: the one under the pointer is never moved */
    for (i = 0; i < n_labels; i++) {
        if (!labels[i].pinned)
            continue;
        labels[i].shown = TRUE;
        shown++;
    }

    /*
     * Then everybody else in order: their own place if it is free, else
     * just below the lowest label in the way, and again, up to @max_steps
     * pushes.  Pushing to the obstacle's bottom rather than a fixed row
     * stacks names whose people stand a pixel apart into neat rows.
     */
    for (i = 0; i < n_labels; i++) {
        ClawtOfficeLabel *l = &labels[i];
        gfloat dy = 0.0f;
        guint step;

        if (l->pinned)
            continue;
        for (step = 0; step <= max_steps; step++) {
            gfloat needed = clear_of_shown(labels, n_labels, i, dy, gap);

            if (needed <= dy) {
                l->dy = dy;
                l->shown = TRUE;
                shown++;
                break;
            }
            dy = needed;
            /* pushed off the bottom is as good as hidden, and worse to read */
            if (l->y + dy + l->height > bottom)
                break;
        }
    }
    return shown;
}

/* ── The control line ─────────────────────────────────────────────── */

gchar *
clawt_office_view_control_line(ClawtOfficeViewMode mode, const gchar *focus,
                               ClawtOfficeZoom zoom, gint ui_scale,
                               ClawtOfficeUsagePlacement usage)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *text = NULL;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mode");
    json_builder_add_string_value(b, clawt_office_view_mode_nick(mode));
    json_builder_set_member_name(b, "focus");
    json_builder_add_string_value(b, focus != NULL ? focus : "");
    json_builder_set_member_name(b, "zoom");
    json_builder_add_string_value(b, clawt_office_zoom_nick(zoom));
    json_builder_set_member_name(b, "ui_scale");
    json_builder_add_int_value(b, clawt_office_view_clamp_ui_scale(ui_scale));
    json_builder_set_member_name(b, "usage");
    json_builder_add_string_value(b, clawt_office_usage_placement_nick(usage));
    json_builder_end_object(b);

    root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    text = json_generator_to_data(gen, NULL);
    return g_strconcat(text, "\n", NULL);
}

/* A member that must be a string, or absent. */
static gboolean
string_member(JsonObject *object, const gchar *name, const gchar **out, GError **error)
{
    JsonNode *node;

    *out = NULL;
    if (!json_object_has_member(object, name))
        return TRUE;
    node = json_object_get_member(object, name);
    if (JSON_NODE_HOLDS_NULL(node))
        return TRUE;
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the control line's \"%s\" is not a string", name);
        return FALSE;
    }
    *out = json_node_get_string(node);
    return TRUE;
}

gboolean
clawt_office_view_control_parse(const gchar *line, ClawtOfficeViewControl *out,
                                GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) parse_error = NULL;
    JsonNode *root;
    JsonObject *object;
    const gchar *mode = NULL;
    const gchar *focus = NULL;
    const gchar *zoom = NULL;
    const gchar *usage = NULL;

    g_return_val_if_fail(line != NULL, FALSE);
    g_return_val_if_fail(out != NULL, FALSE);

    memset(out, 0, sizeof(*out));

    if (!json_parser_load_from_data(parser, line, -1, &parse_error)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the control line is not JSON: %s", parse_error->message);
        return FALSE;
    }
    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the control line is not a JSON object");
        return FALSE;
    }
    object = json_node_get_object(root);

    /* every member checked before any is kept: refused whole, never half-applied */
    if (!string_member(object, "mode", &mode, error) ||
        !string_member(object, "focus", &focus, error) ||
        !string_member(object, "zoom", &zoom, error) ||
        !string_member(object, "usage", &usage, error))
        return FALSE;

    if (mode != NULL) {
        if (!clawt_office_view_mode_from_nick(mode, &out->mode)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "the control line's mode \"%s\" is neither 2d nor 3d", mode);
            return FALSE;
        }
        out->has_mode = TRUE;
    }
    if (zoom != NULL) {
        if (!clawt_office_zoom_from_nick(zoom, &out->zoom)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "the control line's zoom \"%s\" is not fit, pixel, 1x or 2x",
                        zoom);
            return FALSE;
        }
        out->has_zoom = TRUE;
    }
    if (usage != NULL) {
        if (!clawt_office_usage_placement_from_nick(usage, &out->usage)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "the control line's usage \"%s\" is not world, corner or off",
                        usage);
            return FALSE;
        }
        out->has_usage = TRUE;
    }
    if (json_object_has_member(object, "ui_scale")) {
        JsonNode *node = json_object_get_member(object, "ui_scale");

        if (!JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_INT64) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "the control line's \"ui_scale\" is not a whole number");
            return FALSE;
        }
        out->ui_scale = clawt_office_view_clamp_ui_scale(
            (gint)CLAMP(json_node_get_int(node), 1, CLAWT_OFFICE_VIEW_MAX_UI_SCALE));
    }
    /* focus last: the only member that allocates, so nothing above can leak it */
    if (json_object_has_member(object, "focus")) {
        out->has_focus = TRUE;
        out->focus = focus != NULL && *focus != '\0' ? g_strdup(focus) : NULL;
    }
    return TRUE;
}

void
clawt_office_view_control_clear(ClawtOfficeViewControl *control)
{
    g_return_if_fail(control != NULL);

    g_free(control->focus);
    memset(control, 0, sizeof(*control));
}

/* ── The 3D view's camera ──────────────────────────────────────────── */

/*
 * The nearest a framed point may be to the camera, measured along the
 * view: raylib's near plane is a hundredth of a unit, and a point that
 * close is drawn enormous, so this stays well clear of it.
 */
#define FRAME_NEAR (1.0)

/* How many halvings the search for the best aim makes on each axis. */
#define FRAME_STEPS (48)

gboolean
clawt_office_view_project_3d(const gfloat eye[3], const gfloat target[3], gfloat fovy,
                             gfloat view_width, gfloat view_height, const gfloat point[3],
                             gfloat *x, gfloat *y)
{
    gdouble f[3];
    gdouble r[3];
    gdouble u[3];
    gdouble q[3];
    gdouble length;
    gdouble depth;
    gdouble across;
    gdouble upward;
    gdouble t;
    gdouble aspect;
    gint i;

    g_return_val_if_fail(eye != NULL && target != NULL && point != NULL, FALSE);

    /* forward, right = forward x up, and the camera's own up = right x forward */
    for (i = 0; i < 3; i++)
        f[i] = (gdouble)target[i] - (gdouble)eye[i];
    length = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (length <= 0.0)
        return FALSE;
    for (i = 0; i < 3; i++)
        f[i] /= length;

    r[0] = -f[2];
    r[1] = 0.0;
    r[2] = f[0];
    length = sqrt(r[0] * r[0] + r[2] * r[2]);
    if (length <= 0.0)
        return FALSE;
    r[0] /= length;
    r[2] /= length;

    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];

    for (i = 0; i < 3; i++)
        q[i] = (gdouble)point[i] - (gdouble)eye[i];
    depth = q[0] * f[0] + q[1] * f[1] + q[2] * f[2];
    if (depth <= 0.0)
        return FALSE;
    across = q[0] * r[0] + q[1] * r[1] + q[2] * r[2];
    upward = q[0] * u[0] + q[1] * u[1] + q[2] * u[2];

    /* the perspective divide, then normalised device coordinates to pixels */
    t = tan((gdouble)fovy * G_PI / 360.0);
    aspect = (gdouble)view_width / MAX((gdouble)view_height, 1.0);
    if (x != NULL)
        *x = (gfloat)((across / (depth * t * aspect) + 1.0) / 2.0 * (gdouble)view_width);
    if (y != NULL)
        *y = (gfloat)((1.0 - upward / (depth * t)) / 2.0 * (gdouble)view_height);
    return TRUE;
}

gboolean
clawt_office_view_board_corners_3d(const gfloat anchor[3], gfloat width, gfloat height,
                                   gfloat forward_x, gfloat forward_z, gfloat corners[12])
{
    gfloat right_x = -forward_z;
    gfloat right_z = forward_x;
    gfloat length = sqrtf(right_x * right_x + right_z * right_z);
    gint i;

    g_return_val_if_fail(anchor != NULL && corners != NULL, FALSE);

    /* right = forward x up, flattened: the screen stands upright */
    if (length < 0.0001f)
        return FALSE;
    right_x /= length;
    right_z /= length;

    for (i = 0; i < 4; i++) {
        gfloat side = (i & 1) ? 0.5f : -0.5f;
        gfloat rise = (i & 2) ? 1.0f : 0.0f;

        corners[i * 3 + 0] = anchor[0] + right_x * width * side;
        corners[i * 3 + 1] = anchor[1] + 0.2f + height * rise;
        corners[i * 3 + 2] = anchor[2] + right_z * width * side;
    }
    return TRUE;
}

/* The camera's frame at a bearing and pitch; the same for any aim and distance. */
typedef struct {
    gdouble f[3];       /* forward */
    gdouble r[3];       /* right */
    gdouble u[3];       /* the camera's up */
    gdouble kx;         /* how far off the axis a point may be, per unit of depth */
    gdouble ky;
    const gfloat *points;
    guint   n;
} Frame;

/*
 * How far from the aim (@tx, @tz), along the view, the camera has to
 * stand for every point to be in the frame.
 *
 * The camera stands at the aim plus L times -forward, so a point's
 * depth is its depth from the aim plus L, and its offsets across and up
 * do not move with L at all: forward is square to both.  In view is
 * |across| <= kx * depth and |up| <= ky * depth, so each point asks for
 * L >= |across| / kx - its depth from the aim, likewise up, and the
 * camera stands at the largest of those.
 */
static gdouble
frame_distance(const Frame *frame, gdouble tx, gdouble tz)
{
    gdouble need = FRAME_NEAR;
    guint i;

    for (i = 0; i < frame->n; i++) {
        gdouble q[3];
        gdouble depth;
        gdouble across;
        gdouble upward;

        q[0] = (gdouble)frame->points[i * 3 + 0] - tx;
        q[1] = (gdouble)frame->points[i * 3 + 1];
        q[2] = (gdouble)frame->points[i * 3 + 2] - tz;
        depth = q[0] * frame->f[0] + q[1] * frame->f[1] + q[2] * frame->f[2];
        across = q[0] * frame->r[0] + q[1] * frame->r[1] + q[2] * frame->r[2];
        upward = q[0] * frame->u[0] + q[1] * frame->u[1] + q[2] * frame->u[2];

        need = MAX(need, fabs(across) / frame->kx - depth);
        need = MAX(need, fabs(upward) / frame->ky - depth);
        need = MAX(need, FRAME_NEAR - depth);
    }
    return need;
}

/* The aim at (@s along the floor's view direction, @t across it) from (@cx, @cz). */
static void
frame_aim(gdouble cx, gdouble cz, gdouble cos_o, gdouble sin_o, gdouble s, gdouble t,
          gdouble *tx, gdouble *tz)
{
    *tx = cx + cos_o * s + sin_o * t;
    *tz = cz + sin_o * s - cos_o * t;
}

/* The least distance for aims along @s, searching across: convex, so a ternary search. */
static gdouble
frame_best_across(const Frame *frame, gdouble cx, gdouble cz, gdouble cos_o, gdouble sin_o,
                  gdouble s, gdouble span, gdouble *best_t)
{
    gdouble lo = -span;
    gdouble hi = span;
    gdouble tx;
    gdouble tz;
    gint step;

    for (step = 0; step < FRAME_STEPS; step++) {
        gdouble a = lo + (hi - lo) / 3.0;
        gdouble b = hi - (hi - lo) / 3.0;
        gdouble ax;
        gdouble az;
        gdouble bx;
        gdouble bz;

        frame_aim(cx, cz, cos_o, sin_o, s, a, &ax, &az);
        frame_aim(cx, cz, cos_o, sin_o, s, b, &bx, &bz);
        if (frame_distance(frame, ax, az) <= frame_distance(frame, bx, bz))
            hi = b;
        else
            lo = a;
    }
    *best_t = (lo + hi) / 2.0;
    frame_aim(cx, cz, cos_o, sin_o, s, *best_t, &tx, &tz);
    return frame_distance(frame, tx, tz);
}

gboolean
clawt_office_view_frame_3d(const gfloat *points, guint n_points, gfloat orbit, gfloat lift,
                           gfloat fovy, gfloat aspect, gfloat margin, gfloat *target_x,
                           gfloat *target_z, gfloat *reach)
{
    Frame frame;
    gdouble cos_o = cos((gdouble)orbit);
    gdouble sin_o = sin((gdouble)orbit);
    gdouble m = sqrt(1.0 + (gdouble)lift * (gdouble)lift);
    gdouble t = tan((gdouble)fovy * G_PI / 360.0);
    gdouble keep = 1.0 - CLAMP((gdouble)margin, 0.0, 0.5);
    gdouble lo_x = G_MAXDOUBLE;
    gdouble lo_z = G_MAXDOUBLE;
    gdouble hi_x = -G_MAXDOUBLE;
    gdouble hi_z = -G_MAXDOUBLE;
    gdouble cx;
    gdouble cz;
    gdouble span;
    gdouble lo;
    gdouble hi;
    gdouble s;
    gdouble across = 0.0;
    gdouble tx;
    gdouble tz;
    gdouble distance;
    guint i;
    gint step;

    g_return_val_if_fail(target_x != NULL && target_z != NULL && reach != NULL, FALSE);

    if (points == NULL || n_points == 0)
        return FALSE;

    /*
     * The camera's frame: it looks back along (cos, lift, sin), so
     * forward is the opposite; right is forward x up, which across the
     * floor is (sin, 0, -cos); its own up is right x forward.
     */
    frame.f[0] = -cos_o / m;
    frame.f[1] = -(gdouble)lift / m;
    frame.f[2] = -sin_o / m;
    frame.r[0] = sin_o;
    frame.r[1] = 0.0;
    frame.r[2] = -cos_o;
    frame.u[0] = frame.r[1] * frame.f[2] - frame.r[2] * frame.f[1];
    frame.u[1] = frame.r[2] * frame.f[0] - frame.r[0] * frame.f[2];
    frame.u[2] = frame.r[0] * frame.f[1] - frame.r[1] * frame.f[0];
    frame.ky = t * keep;
    frame.kx = t * MAX((gdouble)aspect, 0.01) * keep;
    frame.points = points;
    frame.n = n_points;

    /* search round the middle of what is framed, as far as it reaches */
    for (i = 0; i < n_points; i++) {
        lo_x = MIN(lo_x, (gdouble)points[i * 3 + 0]);
        hi_x = MAX(hi_x, (gdouble)points[i * 3 + 0]);
        lo_z = MIN(lo_z, (gdouble)points[i * 3 + 2]);
        hi_z = MAX(hi_z, (gdouble)points[i * 3 + 2]);
    }
    cx = (lo_x + hi_x) / 2.0;
    cz = (lo_z + hi_z) / 2.0;
    span = MAX(hi_x - lo_x, hi_z - lo_z) + 1.0;

    /*
     * The distance is the largest of functions linear in the aim, so it
     * is convex in the aim; so is its least value over every aim across,
     * as a function of the aim along.  A ternary search over each finds
     * the best aim, plateaus included.
     */
    lo = -span;
    hi = span;
    for (step = 0; step < FRAME_STEPS; step++) {
        gdouble a = lo + (hi - lo) / 3.0;
        gdouble b = hi - (hi - lo) / 3.0;
        gdouble ignored;

        if (frame_best_across(&frame, cx, cz, cos_o, sin_o, a, span, &ignored) <=
            frame_best_across(&frame, cx, cz, cos_o, sin_o, b, span, &ignored))
            hi = b;
        else
            lo = a;
    }
    s = (lo + hi) / 2.0;
    distance = frame_best_across(&frame, cx, cz, cos_o, sin_o, s, span, &across);
    frame_aim(cx, cz, cos_o, sin_o, s, across, &tx, &tz);

    /* the distance is along the view; the reach is its share across the floor */
    *target_x = (gfloat)tx;
    *target_z = (gfloat)tz;
    *reach = (gfloat)(distance / m);
    return TRUE;
}
