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
    case CLAWT_OFFICE_ZOOM_1X: return "1x";
    case CLAWT_OFFICE_ZOOM_2X: return "2x";
    case CLAWT_OFFICE_ZOOM_FIT:
    default:                   return "fit";
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
    case CLAWT_OFFICE_ZOOM_FIT:
    default:
        break;
    }

    if (map_width <= 0.0f || map_height <= 0.0f || view_width <= 0.0f ||
        view_height <= 0.0f)
        return 1.0f;
    fit = MIN(view_width / map_width, view_height / map_height);

    /*
     * Whole numbers when the map fits at one or more: a 2.6x map has
     * some pixels three wide and some two, which reads as shimmering
     * when anything walks.  Below one there is no whole number to take.
     */
    return fit >= 1.0f ? floorf(fit) : fit;
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
                               ClawtOfficeZoom zoom, gint ui_scale)
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
        !string_member(object, "zoom", &zoom, error))
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
                        "the control line's zoom \"%s\" is not fit, 1x or 2x", zoom);
            return FALSE;
        }
        out->has_zoom = TRUE;
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
