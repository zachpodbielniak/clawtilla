/*
 * lrg-face.c - An agent's generated face in the libregnum client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The geometry is libclawt's (src/agent/clawt-face.h), shared with GTK
 * and the web client; this paints it as an #LrgCustomWidget.  The body
 * is a texture made once per shape, colour and size from
 * clawt_face_coverage(), and the eyes are two rounded rects on top.
 *
 * Not a triangle fan, which was the first version: libregnum
 * anti-aliases every triangle's edges, including the ones inside the
 * fan, and the seams showed as a faint star of lines through every face.
 * A coverage mask has no inside edges to show.
 *
 * The frame loop sleeps when nothing moves (lrg-app.c, "pace"), drawing
 * about ten frames a second, and a 160 ms blink would get one frame of
 * that.  So step() answers "busy" for a moment *before* each blink as
 * well as during it, and while the eyes are easing: the loop then runs
 * at full rate for exactly as long as something is moving, and goes
 * back to sleep after.
 */

#include "clawt-lrg.h"

#include <graylib.h>
#include <math.h>
#include <string.h>

/* defines */
#define FACE_STATE          "clawt-lrg-face"
#define FACE_SUPERSAMPLE    (2)     /* texture pixels per canvas unit */
#define FACE_PREROLL_MS     (120)   /* wake this long before a blink */
#define FACE_FALLBACK_COLOR "#7d7f86"

typedef struct {
    gchar    *agent_id;
    guint32   seed;
    gint      shape;
    GrlColor  fill;
    GrlColor  eye;
    gfloat    size;

    gdouble   look_x;
    gdouble   look_y;
    gdouble   target_x;
    gdouble   target_y;
    gdouble   blink;
    gint64    last_ms;

    void    (*on_click)(gpointer user_data);
    gpointer  click_data;
} FaceState;

static gboolean faces_animate = TRUE;
static GHashTable *body_textures = NULL; /* "shape:#rrggbbaa:px" -> GrlTexture */
static GHashTable *noted_faces = NULL;   /* agent id -> resolved face */

static gint64
now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

static void
face_state_free(gpointer data)
{
    FaceState *state = data;

    g_free(state->agent_id);
    g_free(state);
}

static FaceState *
state_of(LrgWidget *face)
{
    return (face != NULL) ? g_object_get_data(G_OBJECT(face), FACE_STATE)
                          : NULL;
}

/* #rgb / #rrggbb into a GrlColor; anything else is the neutral grey. */
static void
parse_color(const gchar *hex, GrlColor *out)
{
    const gchar *use = (clawt_color_ink(hex) != NULL) ? hex
                                                       : FACE_FALLBACK_COLOR;
    gsize digits = strlen(use + 1);
    guint i;
    guint8 channel[3];

    for (i = 0; i < 3; i++) {
        if (digits == 3)
            channel[i] = (guint8)(g_ascii_xdigit_value(use[1 + i]) * 17);
        else
            channel[i] = (guint8)(g_ascii_xdigit_value(use[1 + i * 2]) * 16 +
                                  g_ascii_xdigit_value(use[2 + i * 2]));
    }

    out->r = channel[0];
    out->g = channel[1];
    out->b = channel[2];
    out->a = 255;
}

static void
apply_color(FaceState *state, const gchar *color)
{
    const gchar *use = (clawt_color_ink(color) != NULL) ? color
                                                         : FACE_FALLBACK_COLOR;

    parse_color(use, &state->fill);
    parse_color(clawt_face_eye_color(use), &state->eye);
}

/* ── Callbacks ────────────────────────────────────────────────────── */

static void
face_measure(LrgCustomWidget *widget, gfloat avail_width, gfloat avail_height,
             LrgCustomWidgetSize *size, gpointer user_data)
{
    FaceState *state = user_data;

    (void)widget;
    (void)avail_width;
    (void)avail_height;

    size->width = state->size;
    size->height = state->size;
}

/*
 * The body's texture, made on first use and kept: a fleet draws the same
 * few faces every frame, and the mask is the expensive part.  Made at
 * twice the drawn size and filtered down, so the edge stays smooth at
 * any scale the canvas draws it.
 */
static GrlTexture *
body_texture(FaceState *state, gfloat drawn)
{
    guint px = (guint)CLAMP(ceilf(drawn * FACE_SUPERSAMPLE), 8.0f, 1024.0f);
    g_autofree gchar *key = g_strdup_printf("%d:%02x%02x%02x%02x:%u",
                                            state->shape, state->fill.r,
                                            state->fill.g, state->fill.b,
                                            state->fill.a, px);
    g_autofree guint8 *alpha = NULL;
    g_autofree guint8 *pixels = NULL;
    g_autoptr(GrlImage) image = NULL;
    GrlTexture *texture;
    guint i;

    if (body_textures == NULL)
        body_textures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              g_object_unref);

    texture = g_hash_table_lookup(body_textures, key);
    if (texture != NULL)
        return texture;

    alpha = g_malloc((gsize)px * px);
    pixels = g_malloc((gsize)px * px * 4);
    clawt_face_coverage(state->shape, px, alpha);

    for (i = 0; i < px * px; i++) {
        pixels[i * 4] = state->fill.r;
        pixels[i * 4 + 1] = state->fill.g;
        pixels[i * 4 + 2] = state->fill.b;
        pixels[i * 4 + 3] = alpha[i];
    }

    image = grl_image_new_from_pixels((gint)px, (gint)px,
                                      GRL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
                                      pixels);
    if (image == NULL)
        return NULL;

    texture = grl_texture_new_from_image(image);
    if (texture == NULL)
        return NULL;
    grl_texture_set_filter(texture, GRL_TEXTURE_FILTER_BILINEAR);
    g_hash_table_insert(body_textures, g_steal_pointer(&key), texture);

    return texture;
}

static void
face_paint(LrgCustomWidget *widget, LrgPaintList *list, gfloat x, gfloat y,
           gfloat width, gfloat height, gpointer user_data)
{
    FaceState *state = user_data;
    gfloat side = MIN(width, height);
    gfloat scale = side / 2.0f;
    gfloat cx = x + width / 2.0f;
    gfloat cy = y + height / 2.0f;
    GrlTexture *body;
    ClawtFacePose pose;
    ClawtFaceEyes eyes;
    const ClawtFaceEye *each[2];
    guint i;

    (void)widget;

    if (scale <= 0.0f)
        return;

    /* ── The body ── */
    body = body_texture(state, side);
    if (body != NULL)
        lrg_paint_list_image(list, body, NULL, cx - scale, cy - scale, side,
                             side, 0.0f, NULL);

    /* ── The eyes, in the current pose ── */
    pose.look_x = state->look_x;
    pose.look_y = state->look_y;
    pose.blink = state->blink;
    clawt_face_eyes(state->shape, &pose, &eyes);
    each[0] = &eyes.left;
    each[1] = &eyes.right;

    for (i = 0; i < 2; i++) {
        const ClawtFaceEye *eye = each[i];

        lrg_paint_list_rect(list,
                            cx + (gfloat)(eye->x - eye->width / 2.0) * scale,
                            cy + (gfloat)(eye->y - eye->height / 2.0) * scale,
                            (gfloat)eye->width * scale,
                            (gfloat)eye->height * scale,
                            (gfloat)eye->radius * scale, &state->eye);
    }
}

/*
 * One frame: ease the eyes, take the blink from the shared schedule.
 * Busy while the eyes move, during a blink, and for FACE_PREROLL_MS
 * before one, so the loop is at full rate when the eyes start to close.
 */
static gboolean
face_step(LrgCustomWidget *widget, gfloat delta_time, gpointer user_data)
{
    FaceState *state = user_data;
    gint64 now = now_ms();
    gdouble dt = (state->last_ms > 0) ? (gdouble)(now - state->last_ms)
                                      : (gdouble)delta_time * 1000.0;
    gboolean moving;

    (void)widget;

    state->last_ms = now;

    if (!faces_animate) {
        state->look_x = state->look_y = 0.0;
        state->target_x = state->target_y = 0.0;
        state->blink = 0.0;
        return FALSE;
    }

    state->look_x = clawt_face_ease(state->look_x, state->target_x, dt);
    state->look_y = clawt_face_ease(state->look_y, state->target_y, dt);
    moving = state->look_x != state->target_x ||
             state->look_y != state->target_y;

    state->blink = clawt_face_blink_at(state->seed, now);

    return moving || state->blink > 0.0 ||
           clawt_face_next_blink(state->seed, now) - now < FACE_PREROLL_MS;
}

/* Over the face itself: look at the pointer; leaving: look away. */
static gboolean
face_event(LrgCustomWidget *widget, const LrgUIEvent *event,
           gpointer user_data)
{
    FaceState *state = user_data;

    switch (lrg_ui_event_get_event_type(event)) {
    case LRG_UI_EVENT_MOUSE_MOVE:
    case LRG_UI_EVENT_POINTER_ENTER:
        clawt_lrg_face_point_at(LRG_WIDGET(widget),
                                lrg_ui_event_get_x(event),
                                lrg_ui_event_get_y(event));
        break;
    case LRG_UI_EVENT_POINTER_LEAVE:
        clawt_lrg_face_look_away(LRG_WIDGET(widget));
        break;
    case LRG_UI_EVENT_MOUSE_BUTTON_DOWN:
        /* the press is taken so the release comes back here (the row
         * catcher's rule, lrg-sidebar.c) -- only by a face that clicks */
        return state->on_click != NULL &&
               lrg_ui_event_get_button(event) == GRL_MOUSE_BUTTON_LEFT;
    case LRG_UI_EVENT_MOUSE_BUTTON_UP:
        /* a face that is a button: a picker's tile */
        if (state->on_click != NULL &&
            lrg_ui_event_get_button(event) == GRL_MOUSE_BUTTON_LEFT &&
            lrg_widget_contains_point(LRG_WIDGET(widget),
                                      lrg_ui_event_get_x(event),
                                      lrg_ui_event_get_y(event))) {
            state->on_click(state->click_data);
            return TRUE;
        }
        break;
    default:
        break;
    }

    /* otherwise never consumed: a click on a face is a click on its row */
    return FALSE;
}

static const LrgCustomWidgetFuncs face_funcs = {
    face_measure, face_paint, face_step, face_event, { NULL }
};

/* ── Public ───────────────────────────────────────────────────────── */

LrgWidget *
clawt_lrg_face_new(const gchar *agent_id, gint shape, const gchar *color,
                   gfloat size)
{
    LrgCustomWidget *face = lrg_custom_widget_new();
    FaceState *state = g_new0(FaceState, 1);

    state->agent_id = g_strdup(agent_id);
    state->seed = clawt_face_seed(agent_id);
    state->shape = shape;
    state->size = MAX(size, 1.0f);
    apply_color(state, color);

    g_object_set_data_full(G_OBJECT(face), FACE_STATE, state,
                           face_state_free);
    lrg_custom_widget_set_paint_box(face, FALSE);
    lrg_custom_widget_set_funcs(face, &face_funcs, state, NULL);
    lrg_widget_set_min_size(LRG_WIDGET(face), state->size, state->size);
    lrg_widget_set_max_size(LRG_WIDGET(face), state->size, state->size);

    return LRG_WIDGET(face);
}

gboolean
clawt_lrg_is_face(LrgWidget *widget)
{
    return state_of(widget) != NULL;
}

void
clawt_lrg_face_set_shape(LrgWidget *face, gint shape)
{
    FaceState *state = state_of(face);

    g_return_if_fail(state != NULL);
    state->shape = shape;
}

gint
clawt_lrg_face_get_shape(LrgWidget *face)
{
    FaceState *state = state_of(face);

    g_return_val_if_fail(state != NULL, 0);
    return state->shape;
}

void
clawt_lrg_face_set_color(LrgWidget *face, const gchar *color)
{
    FaceState *state = state_of(face);

    g_return_if_fail(state != NULL);
    apply_color(state, color);
}

void
clawt_lrg_face_set_agent_id(LrgWidget *face, const gchar *agent_id)
{
    FaceState *state = state_of(face);

    g_return_if_fail(state != NULL);
    g_free(state->agent_id);
    state->agent_id = g_strdup(agent_id);
    state->seed = clawt_face_seed(agent_id);
}

void
clawt_lrg_face_point_at(LrgWidget *face, gfloat world_x, gfloat world_y)
{
    FaceState *state = state_of(face);
    gfloat width;
    gfloat height;

    g_return_if_fail(state != NULL);

    if (!faces_animate)
        return;

    width = lrg_widget_get_width(face);
    height = lrg_widget_get_height(face);
    if (width <= 0.0f || height <= 0.0f)
        width = height = state->size;

    clawt_face_look_at(lrg_widget_get_world_x(face) + width / 2.0f,
                       lrg_widget_get_world_y(face) + height / 2.0f,
                       MIN(width, height) / 2.0f, world_x, world_y,
                       &state->target_x, &state->target_y);
}

void
clawt_lrg_face_look_away(LrgWidget *face)
{
    FaceState *state = state_of(face);

    g_return_if_fail(state != NULL);
    state->target_x = 0.0;
    state->target_y = 0.0;
}

void
clawt_lrg_face_get_pose(LrgWidget *face, gdouble *look_x, gdouble *look_y,
                        gdouble *target_x, gdouble *target_y)
{
    FaceState *state = state_of(face);

    g_return_if_fail(state != NULL);

    if (look_x != NULL)
        *look_x = state->look_x;
    if (look_y != NULL)
        *look_y = state->look_y;
    if (target_x != NULL)
        *target_x = state->target_x;
    if (target_y != NULL)
        *target_y = state->target_y;
}

gboolean
clawt_lrg_face_step(LrgWidget *face, gdouble dt_ms)
{
    FaceState *state = state_of(face);

    g_return_val_if_fail(state != NULL, FALSE);

    state->look_x = clawt_face_ease(state->look_x, state->target_x, dt_ms);
    state->look_y = clawt_face_ease(state->look_y, state->target_y, dt_ms);

    return state->look_x != state->target_x ||
           state->look_y != state->target_y;
}

/*
 * The tracked area's events -- the sidebar row's catcher, which sits on
 * top of the row and so receives the pointer before the face beneath
 * it would.  Not consumed: the row still has to see its click.
 */
static gboolean
on_area_event(LrgWidget *area, const LrgUIEvent *event, gpointer user_data)
{
    LrgWidget *face = user_data;

    (void)area;

    switch (lrg_ui_event_get_event_type(event)) {
    case LRG_UI_EVENT_MOUSE_MOVE:
    case LRG_UI_EVENT_POINTER_ENTER:
        clawt_lrg_face_point_at(face, lrg_ui_event_get_x(event),
                                lrg_ui_event_get_y(event));
        break;
    case LRG_UI_EVENT_POINTER_LEAVE:
        clawt_lrg_face_look_away(face);
        break;
    default:
        break;
    }

    return FALSE;
}

void
clawt_lrg_face_track(LrgWidget *face, LrgWidget *area)
{
    g_return_if_fail(state_of(face) != NULL);
    g_return_if_fail(LRG_IS_WIDGET(area));

    g_signal_connect_object(area, "event", G_CALLBACK(on_area_event), face, 0);
}

void
clawt_lrg_faces_set_animate(gboolean animate)
{
    faces_animate = animate ? TRUE : FALSE;
}

gboolean
clawt_lrg_faces_get_animate(void)
{
    return faces_animate;
}

void
clawt_lrg_face_note(const gchar *agent_id, const gchar *face)
{
    g_return_if_fail(agent_id != NULL);

    if (noted_faces == NULL)
        noted_faces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            g_free);

    if (face == NULL)
        g_hash_table_remove(noted_faces, agent_id);
    else
        g_hash_table_replace(noted_faces, g_strdup(agent_id), g_strdup(face));
}

gint
clawt_lrg_face_for(const gchar *agent_id)
{
    const gchar *noted = NULL;
    gint shape;

    if (noted_faces != NULL && agent_id != NULL)
        noted = g_hash_table_lookup(noted_faces, agent_id);

    if (noted == NULL)
        return clawt_face_resolve(agent_id, NULL);

    /* a shape this build does not know is the disc, as in GTK */
    shape = clawt_face_shape_lookup(noted);

    return (shape == -2) ? CLAWT_FACE_INITIALS : shape;
}

/* ── The picker ───────────────────────────────────────────────────── */

#define PICKER_STATE        "clawt-lrg-face-picker"
#define PICKER_PREVIEW      (80.0f)
#define PICKER_TILE         (34.0f)
#define PICKER_SWATCH       (22.0f)
#define PICKER_PER_ROW      (8)

typedef struct {
    gchar             *agent_id;
    gchar             *face;      /* "" automatic, a shape name, initials */
    gchar             *color;     /* "" automatic, or #rrggbb */
    LrgWidget         *preview;   /* a face */
    LrgWidget         *initials;  /* the disc, shown for initials */
    LrgWidget         *caption;
    LrgWidget         *auto_tile;
    GPtrArray         *tiles;     /* faces, borrowed: recoloured */
    GPtrArray         *rings;     /* LrgPanel per shape choice, same order */
    GPtrArray         *ring_values;
    GPtrArray         *swatch_rings;
    GPtrArray         *swatch_values;
    ClawtLrgFacePicked func;
    gpointer           func_data;
    GrlColor          *auto_swatch; /* the automatic dot's colour, borrowed */
} Picker;

typedef struct {
    Picker *picker;
    gchar  *value;
    gboolean is_color;
} PickerChoice;

static void on_shuffle_clicked(LrgButton *button, gpointer user_data);

static void
picker_free(gpointer data)
{
    Picker *picker = data;

    g_free(picker->agent_id);
    g_free(picker->face);
    g_free(picker->color);
    g_ptr_array_unref(picker->tiles);
    g_ptr_array_unref(picker->rings);
    g_ptr_array_unref(picker->ring_values);
    g_ptr_array_unref(picker->swatch_rings);
    g_ptr_array_unref(picker->swatch_values);
    g_free(picker);
}

static void
choice_free(gpointer data)
{
    PickerChoice *choice = data;

    g_free(choice->value);
    g_free(choice);
}

static const gchar *
picker_paint(Picker *picker)
{
    if (picker->color != NULL && *picker->color != '\0')
        return picker->color;

    return clawt_avatar_color(picker->agent_id, picker->agent_id, NULL);
}

/* A ring round the chosen tile and swatch, none round the rest. */
static void
mark_rings(GPtrArray *rings, GPtrArray *values, const gchar *chosen)
{
    static const GrlColor on = { 0x8c, 0xb4, 0xff, 0xff };
    static const GrlColor off = { 0, 0, 0, 0 };
    guint i;

    for (i = 0; i < rings->len; i++)
        lrg_panel_set_border_color(
            g_ptr_array_index(rings, i),
            g_strcmp0(g_ptr_array_index(values, i), chosen) == 0 ? &on : &off);
}

static void
picker_refresh(Picker *picker)
{
    const gchar *paint = picker_paint(picker);
    gint shape = clawt_face_resolve(picker->agent_id, picker->face);
    gboolean automatic = (picker->face == NULL || *picker->face == '\0');
    g_autofree gchar *caption = NULL;
    guint i;

    for (i = 0; i < picker->tiles->len; i++)
        clawt_lrg_face_set_color(g_ptr_array_index(picker->tiles, i), paint);
    clawt_lrg_face_set_shape(picker->auto_tile,
                             clawt_face_resolve(picker->agent_id, NULL));

    if (shape == CLAWT_FACE_INITIALS) {
        lrg_widget_set_visible(picker->preview, FALSE);
        lrg_widget_set_visible(picker->initials, TRUE);
        caption = g_strdup("Initials");
    } else {
        lrg_widget_set_visible(picker->preview, TRUE);
        lrg_widget_set_visible(picker->initials, FALSE);
        clawt_lrg_face_set_shape(picker->preview, shape);
        clawt_lrg_face_set_color(picker->preview, paint);
        caption = automatic
            ? g_strdup_printf("%s (automatic)", clawt_face_shape_label(shape))
            : g_strdup(clawt_face_shape_label(shape));
    }
    lrg_label_set_text(LRG_LABEL(picker->caption), caption);

    /* the automatic tone follows the id, as the automatic shape does */
    if (picker->auto_swatch != NULL)
        parse_color(clawt_avatar_color(picker->agent_id, picker->agent_id,
                                       NULL),
                    picker->auto_swatch);

    mark_rings(picker->rings, picker->ring_values, picker->face);
    mark_rings(picker->swatch_rings, picker->swatch_values, picker->color);
}

/* The stored spelling: lowercase name, initials, or "" for automatic. */
static gchar *
normalise_face(const gchar *face)
{
    gint shape = clawt_face_shape_lookup(face);

    if (shape == CLAWT_FACE_INITIALS)
        return g_strdup(CLAWT_FACE_INITIALS_NAME);
    if (shape >= 0)
        return g_strdup(clawt_face_shape_name(shape));

    return g_strdup("");
}

static void
on_choice(gpointer user_data)
{
    PickerChoice *choice = user_data;
    Picker *picker = choice->picker;

    if (choice->is_color) {
        g_free(picker->color);
        picker->color = g_strdup(choice->value);
    } else {
        g_free(picker->face);
        picker->face = g_strdup(choice->value);
    }

    picker_refresh(picker);
    if (picker->func != NULL)
        picker->func(picker->func_data, picker->face, picker->color);
}

static void
on_initials_clicked(LrgButton *button, gpointer user_data)
{
    (void)button;
    on_choice(user_data);
}

/* A selectable tile: a panel whose border is the selection ring. */
static LrgWidget *
ring_new(Picker *picker, LrgWidget *content, const gchar *value,
         gboolean is_color, gfloat side)
{
    LrgPanel *ring = lrg_panel_new();
    static const GrlColor clear = { 0, 0, 0, 0 };
    gfloat outer = side + 2.0f * 5.0f;

    lrg_panel_set_background_color(ring, &clear);
    lrg_panel_set_border_color(ring, &clear);
    lrg_panel_set_border_width(ring, 2.0f);
    lrg_panel_set_corner_radius(ring, 10.0f);
    lrg_panel_set_elevation(ring, 0);
    lrg_container_set_padding(LRG_CONTAINER(ring), 5.0f);

    /*
     * Sized outright, and filled: a panel measures nothing of its own,
     * and without both the tile inside was laid out at no size at all.
     */
    lrg_panel_set_fill_children(ring, TRUE);
    lrg_widget_set_min_size(LRG_WIDGET(ring), outer, outer);
    lrg_widget_set_max_size(LRG_WIDGET(ring), outer, outer);
    clawt_lrg_take(LRG_WIDGET(ring), content);

    if (is_color) {
        g_ptr_array_add(picker->swatch_rings, ring);
        g_ptr_array_add(picker->swatch_values, g_strdup(value));
    } else {
        g_ptr_array_add(picker->rings, ring);
        g_ptr_array_add(picker->ring_values, g_strdup(value));
    }

    return LRG_WIDGET(ring);
}

/* A face as a button for @value. */
static LrgWidget *
face_tile(Picker *picker, gint shape, const gchar *value)
{
    LrgWidget *face = clawt_lrg_face_new(picker->agent_id, shape, NULL,
                                         PICKER_TILE);
    FaceState *state = state_of(face);
    PickerChoice *choice = g_new0(PickerChoice, 1);

    choice->picker = picker;
    choice->value = g_strdup(value);
    state->on_click = on_choice;
    state->click_data = choice;
    g_object_set_data_full(G_OBJECT(face), "clawt-choice", choice,
                           choice_free);
    g_ptr_array_add(picker->tiles, face);

    return ring_new(picker, face, value, FALSE, PICKER_TILE);
}

/* ── Swatches: a clickable dot ── */

typedef struct {
    GrlColor      color;
    gboolean      automatic;
    PickerChoice *choice;
} Swatch;

static void
swatch_free(gpointer data)
{
    Swatch *swatch = data;

    choice_free(swatch->choice);
    g_free(swatch);
}

static void
swatch_measure(LrgCustomWidget *widget, gfloat avail_width,
               gfloat avail_height, LrgCustomWidgetSize *size,
               gpointer user_data)
{
    (void)widget;
    (void)avail_width;
    (void)avail_height;
    (void)user_data;

    size->width = PICKER_SWATCH;
    size->height = PICKER_SWATCH;
}

static void
swatch_paint(LrgCustomWidget *widget, LrgPaintList *list, gfloat x, gfloat y,
             gfloat width, gfloat height, gpointer user_data)
{
    Swatch *swatch = user_data;
    gfloat radius = MIN(width, height) / 2.0f;

    (void)widget;

    lrg_paint_list_circle(list, x + width / 2.0f, y + height / 2.0f, radius,
                          &swatch->color);

    /* the automatic tone: a white ring inside, the GTK picker's dashes */
    if (swatch->automatic) {
        static const GrlColor ring = { 0xff, 0xff, 0xff, 0xd8 };

        lrg_paint_list_border(list, x + 4.0f, y + 4.0f, width - 8.0f,
                              height - 8.0f, radius - 4.0f, 1.5f, &ring);
    }
}

static gboolean
swatch_event(LrgCustomWidget *widget, const LrgUIEvent *event,
             gpointer user_data)
{
    Swatch *swatch = user_data;

    if (lrg_ui_event_get_button(event) != GRL_MOUSE_BUTTON_LEFT)
        return FALSE;
    if (lrg_ui_event_get_event_type(event) == LRG_UI_EVENT_MOUSE_BUTTON_DOWN)
        return TRUE;

    if (lrg_ui_event_get_event_type(event) == LRG_UI_EVENT_MOUSE_BUTTON_UP &&
        lrg_widget_contains_point(LRG_WIDGET(widget),
                                  lrg_ui_event_get_x(event),
                                  lrg_ui_event_get_y(event))) {
        on_choice(swatch->choice);
        return TRUE;
    }

    return FALSE;
}

static const LrgCustomWidgetFuncs swatch_funcs = {
    swatch_measure, swatch_paint, NULL, swatch_event, { NULL }
};

static LrgWidget *
swatch_new(Picker *picker, const gchar *value)
{
    LrgCustomWidget *dot = lrg_custom_widget_new();
    Swatch *swatch = g_new0(Swatch, 1);

    swatch->automatic = (*value == '\0');
    parse_color(swatch->automatic
                    ? clawt_avatar_color(picker->agent_id, picker->agent_id,
                                         NULL)
                    : value,
                &swatch->color);
    swatch->choice = g_new0(PickerChoice, 1);
    swatch->choice->picker = picker;
    swatch->choice->value = g_strdup(value);
    swatch->choice->is_color = TRUE;

    g_object_set_data_full(G_OBJECT(dot), "clawt-swatch", swatch,
                           swatch_free);
    lrg_custom_widget_set_paint_box(dot, FALSE);
    lrg_custom_widget_set_funcs(dot, &swatch_funcs, swatch, NULL);
    if (swatch->automatic)
        picker->auto_swatch = &swatch->color;
    lrg_widget_set_min_size(LRG_WIDGET(dot), PICKER_SWATCH, PICKER_SWATCH);
    lrg_widget_set_max_size(LRG_WIDGET(dot), PICKER_SWATCH, PICKER_SWATCH);

    return ring_new(picker, LRG_WIDGET(dot), value, TRUE, PICKER_SWATCH);
}

LrgWidget *
clawt_lrg_face_picker_new(const gchar *agent_id, const gchar *face,
                          const gchar *color, ClawtLrgFacePicked func,
                          gpointer user_data)
{
    LrgWidget *box = clawt_lrg_vbox(8.0f);
    Picker *picker = g_new0(Picker, 1);
    LrgWidget *row = NULL;
    LrgWidget *shapes = clawt_lrg_vbox(4.0f);
    LrgWidget *swatches = clawt_lrg_hbox(4.0f);
    guint count = 0;
    guint i;

    picker->agent_id = g_strdup(agent_id);
    picker->face = normalise_face(face);
    picker->color = g_strdup(clawt_color_ink(color) != NULL ? color : "");
    picker->tiles = g_ptr_array_new();
    picker->rings = g_ptr_array_new();
    picker->ring_values = g_ptr_array_new_with_free_func(g_free);
    picker->swatch_rings = g_ptr_array_new();
    picker->swatch_values = g_ptr_array_new_with_free_func(g_free);
    picker->func = func;
    picker->func_data = user_data;
    g_object_set_data_full(G_OBJECT(box), PICKER_STATE, picker, picker_free);
    lrg_widget_add_class(box, "clawt-face-picker");

    /* ── The preview, which watches the pointer while it is over it ── */
    picker->preview = clawt_lrg_face_new(
        agent_id, clawt_face_resolve(agent_id, NULL), NULL, PICKER_PREVIEW);
    lrg_widget_set_halign(picker->preview, LRG_ALIGN_CENTER);
    g_object_ref(picker->preview);
    clawt_lrg_take(box, picker->preview);

    /*
     * As tall as the face it stands in for: a preview that changed
     * height moved every tile below it between two clicks, and the
     * second click landed on a different shape.
     */
    picker->initials = clawt_lrg_label("Aa", "title");
    lrg_label_set_alignment(LRG_LABEL(picker->initials),
                            LRG_TEXT_ALIGN_CENTER);
    lrg_widget_set_min_size(picker->initials, PICKER_PREVIEW, PICKER_PREVIEW);
    lrg_widget_set_halign(picker->initials, LRG_ALIGN_CENTER);
    lrg_widget_set_valign(picker->initials, LRG_ALIGN_CENTER);
    g_object_ref(picker->initials);
    clawt_lrg_take(box, picker->initials);

    /* the caption, and Shuffle beside it: a random shape and colour */
    {
        LrgWidget *line = clawt_lrg_hbox(8.0f);

        lrg_widget_set_halign(line, LRG_ALIGN_CENTER);
        picker->caption = clawt_lrg_label("", "muted");
        lrg_widget_set_valign(picker->caption, LRG_ALIGN_CENTER);
        g_object_ref(picker->caption);
        clawt_lrg_take(line, picker->caption);
        clawt_lrg_take(line, clawt_lrg_button("Shuffle", "flat",
                                              G_CALLBACK(on_shuffle_clicked),
                                              box));
        clawt_lrg_take(box, line);
    }

    /* ── Shapes: automatic, each shape, initials; PICKER_PER_ROW a row ── */
    for (i = 0; i <= clawt_face_shape_count() + 1; i++) {
        LrgWidget *tile;

        if (count % PICKER_PER_ROW == 0) {
            row = clawt_lrg_hbox(4.0f);
            lrg_widget_set_halign(row, LRG_ALIGN_CENTER);
            clawt_lrg_take(shapes, row);
        }

        if (i == 0) {
            tile = face_tile(picker, clawt_face_resolve(agent_id, NULL), "");
            picker->auto_tile = g_ptr_array_index(picker->tiles,
                                                  picker->tiles->len - 1);
        } else if (i <= clawt_face_shape_count()) {
            tile = face_tile(picker, (gint)(i - 1),
                             clawt_face_shape_name((gint)(i - 1)));
        } else {
            PickerChoice *choice = g_new0(PickerChoice, 1);
            LrgWidget *button;

            choice->picker = picker;
            choice->value = g_strdup(CLAWT_FACE_INITIALS_NAME);
            button = clawt_lrg_button("Aa", "flat",
                                      G_CALLBACK(on_initials_clicked), choice);
            g_object_set_data_full(G_OBJECT(button), "clawt-choice", choice,
                                   choice_free);
            tile = ring_new(picker, button, CLAWT_FACE_INITIALS_NAME, FALSE,
                            PICKER_TILE);
        }

        clawt_lrg_take(row, tile);
        count++;
    }
    clawt_lrg_take(box, shapes);

    /* ── Colours: automatic, then the palette ── */
    lrg_widget_set_halign(swatches, LRG_ALIGN_CENTER);
    clawt_lrg_take(swatches, swatch_new(picker, ""));
    for (i = 0; i < CLAWT_FACE_PALETTE_SIZE; i++)
        clawt_lrg_take(swatches,
                       swatch_new(picker, clawt_face_palette_color(i)));
    clawt_lrg_take(box, swatches);

    picker_refresh(picker);

    /* the extra references were only to keep them while being added */
    g_object_unref(picker->preview);
    g_object_unref(picker->initials);
    g_object_unref(picker->caption);

    return box;
}

static Picker *
picker_of(LrgWidget *widget)
{
    return (widget != NULL) ? g_object_get_data(G_OBJECT(widget), PICKER_STATE)
                            : NULL;
}

void
clawt_lrg_face_picker_set_agent_id(LrgWidget *widget, const gchar *agent_id)
{
    Picker *picker = picker_of(widget);
    guint i;

    g_return_if_fail(picker != NULL);

    g_free(picker->agent_id);
    picker->agent_id = g_strdup(agent_id);
    clawt_lrg_face_set_agent_id(picker->preview, agent_id);
    for (i = 0; i < picker->tiles->len; i++)
        clawt_lrg_face_set_agent_id(g_ptr_array_index(picker->tiles, i),
                                    agent_id);
    picker_refresh(picker);
}

void
clawt_lrg_face_picker_select(LrgWidget *widget, const gchar *face,
                             const gchar *color)
{
    Picker *picker = picker_of(widget);

    g_return_if_fail(picker != NULL);

    g_free(picker->face);
    picker->face = normalise_face(face);
    g_free(picker->color);
    picker->color = g_strdup(clawt_color_ink(color) != NULL ? color : "");
    picker_refresh(picker);
    if (picker->func != NULL)
        picker->func(picker->func_data, picker->face, picker->color);
}

void
clawt_lrg_face_picker_shuffle(LrgWidget *widget)
{
    gint shape;
    const gchar *color;

    clawt_face_pick_random(&shape, &color);
    clawt_lrg_face_picker_select(widget, clawt_face_shape_name(shape), color);
}

static void
on_shuffle_clicked(LrgButton *button, gpointer user_data)
{
    (void)button;
    clawt_lrg_face_picker_shuffle(LRG_WIDGET(user_data));
}

const gchar *
clawt_lrg_face_picker_get_face(LrgWidget *widget)
{
    Picker *picker = picker_of(widget);

    g_return_val_if_fail(picker != NULL, "");
    return picker->face;
}

const gchar *
clawt_lrg_face_picker_get_color(LrgWidget *widget)
{
    Picker *picker = picker_of(widget);

    g_return_val_if_fail(picker != NULL, "");
    return picker->color;
}

LrgWidget *
clawt_lrg_face_picker_get_preview(LrgWidget *widget)
{
    Picker *picker = picker_of(widget);

    g_return_val_if_fail(picker != NULL, NULL);
    return picker->preview;
}
