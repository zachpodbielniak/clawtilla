/*
 * lrg-office-view.c - The drawn office, natively in the lrg client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * In an OFFICE=1 build the Office page draws the office in this process:
 * the scene and feed clawtilla-office uses (clients/office/), drawn into
 * an LrgViewport's render texture every frame.  No child process and no
 * frames over a socket -- this client already is libregnum.
 *
 * The feed's requests are asynchronous on the app's context, which the
 * app drains once a frame (AGENTS.md: nothing in clients/lrg/ waits).
 * The world is loaded, and its map baked into a texture, from the feed's
 * callback, which runs in that drain -- outside any texture mode, since
 * raylib's do not nest and the viewport draws inside its own.
 */

#include "clawt-lrg.h"

#ifdef CLAWT_HAVE_OFFICE

#include "office-feed.h"
#include "office-scene.h"

#include <raylib.h>

#include <math.h>
#include <string.h>

#define VIEW_HEIGHT (440.0f)
/* how far a press may wander, in pixels, and still be a click */
#define CLICK_SLOP (4.0f)

static struct {
    ClawtLrgApp      *app;
    LrgViewport      *viewport;
    ClawtOfficeScene *scene;
    ClawtOfficeFeed  *feed;
    gdouble           last_time;
    gfloat            pointer_x;
    gfloat            pointer_y;
    gboolean          button_down[3]; /* 0 left, 1 right, 2 middle */
    gboolean          left_pressed;
    gboolean          click_pending;
    gboolean          dragged;
    gboolean          shift_mod;
    gfloat            press_x;
    gfloat            press_y;
    gfloat            click_x;
    gfloat            click_y;
    gfloat            drag_dx;
    gfloat            drag_dy;
    gfloat            wheel;
} view;

static void
on_world(const gchar *pack_dir, JsonObject *world, gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    if (!clawt_office_scene_load_world(view.scene, pack_dir, world, &error)) {
        g_autofree gchar *line = g_strdup_printf("The world did not load: %s",
                                                 error->message);

        clawt_office_scene_set_status(view.scene, line);
    } else {
        clawt_office_scene_prepare(view.scene);
    }
    clawt_lrg_app_wake(user_data);
}

static void
on_layout(JsonObject *layout, gpointer user_data)
{
    clawt_office_scene_set_layout(view.scene, layout);
    clawt_lrg_app_wake(user_data);
}

static void
on_color(const gchar *id, const gchar *color, gpointer user_data)
{
    (void)user_data;
    clawt_office_scene_set_color(view.scene, id, color);
}

static void
on_status(const gchar *text, gpointer user_data)
{
    clawt_office_scene_set_status(view.scene, text);
    clawt_lrg_app_wake(user_data);
}

static void
on_character(const gchar *agent, const gchar *dir, JsonObject *character,
             gpointer user_data)
{
    clawt_office_scene_set_character(view.scene, agent, dir, character);
    /* the texture loads here, in the drain, outside the viewport's texture mode */
    clawt_office_scene_prepare(view.scene);
    clawt_lrg_app_wake(user_data);
}

static void
on_usage(JsonObject *usage, gpointer user_data)
{
    clawt_office_scene_set_usage(view.scene, usage);
}

static void
on_look(const gchar *id, const gchar *name, const gchar *face, gboolean has_avatar,
        gpointer user_data)
{
    (void)user_data;
    clawt_office_scene_set_look(view.scene, id, name, face, has_avatar);
}

static void
on_portrait(const gchar *id, const guint8 *rgba, gint size, const gchar *etag,
            gpointer user_data)
{
    clawt_office_scene_set_portrait(view.scene, id, rgba, size, etag);
    /* uploaded here, in the drain, outside the viewport's texture mode */
    clawt_office_scene_prepare(view.scene);
    clawt_lrg_app_wake(user_data);
}

static const ClawtOfficeFeedFuncs feed_funcs = {
    on_world, on_layout, on_color, on_status, on_character, on_usage,
    on_look, on_portrait
};

/* Keys move the camera only while the viewport itself has them. */
static gboolean
viewport_focused(void)
{
    LrgWidget *root;

    if (view.viewport == NULL)
        return FALSE;
    root = lrg_widget_get_root(LRG_WIDGET(view.viewport));
    if (!LRG_IS_CANVAS(root))
        return FALSE;
    return lrg_canvas_get_focused_widget(LRG_CANVAS(root)) == LRG_WIDGET(view.viewport);
}

/*
 * One sample for the shared camera.  The key signal is press-only, so
 * held movement is polled here, and only while this viewport is focused
 * -- otherwise a chat's keys would walk the office.
 */
static void
apply_input(gfloat dt, gfloat width, gfloat height, gpointer user_data)
{
    ClawtOfficeInput input;
    gboolean eat;
    guint i;

    memset(&input, 0, sizeof(input));
    input.dt = dt;
    input.view_w = width;
    input.view_h = height;
    input.x = view.pointer_x;
    input.y = view.pointer_y;
    input.pointer_valid = TRUE;
    input.dx = view.drag_dx;
    input.dy = view.drag_dy;
    input.wheel = view.wheel;
    input.left_down = view.button_down[0];
    input.right_down = view.button_down[1];
    input.middle_down = view.button_down[2];
    input.left_pressed = view.left_pressed;
    input.shift = view.shift_mod;
    view.drag_dx = 0.0f;
    view.drag_dy = 0.0f;
    view.wheel = 0.0f;
    view.left_pressed = FALSE;
    if (viewport_focused()) {
        for (i = 0; i < clawt_office_binding_count(); i++) {
            guint key = clawt_office_binding_key(i);

            clawt_office_input_note_key(&input, key, IsKeyPressed((int)key),
                                        IsKeyDown((int)key));
        }
    }
    eat = clawt_office_scene_input(view.scene, &input);
    if (view.click_pending && !eat) {
        const gchar *under = NULL;

        if (!clawt_office_scene_click(view.scene, view.click_x, view.click_y))
            under = clawt_office_scene_pointer(view.scene, view.click_x, view.click_y);
        if (under != NULL) {
            g_autofree gchar *id = g_strdup(under);

            clawt_lrg_select_agent(user_data, id);
        }
        clawt_lrg_app_wake(user_data);
    }
    view.click_pending = FALSE;
    clawt_office_scene_pointer(view.scene, view.pointer_x, view.pointer_y);
}

/* Every frame, inside the viewport's render texture. */
static void
render(LrgViewport *viewport, GrlRenderTexture *target, gfloat width, gfloat height,
       gpointer user_data)
{
    gdouble now = g_get_monotonic_time() / (gdouble)G_USEC_PER_SEC;
    gfloat dt = view.last_time > 0.0 ? (gfloat)(now - view.last_time) : 0.0f;

    (void)viewport;
    (void)target;
    view.last_time = now;
    dt = MIN(dt, 0.1f);
    apply_input(dt, width, height, user_data);
    if (clawt_office_scene_update(view.scene, dt))
        clawt_lrg_app_wake(user_data);
    clawt_office_scene_draw(view.scene, (gint)width, (gint)height);
}

static void
on_pointer_move(LrgViewport *viewport, gfloat u, gfloat v, gfloat dx, gfloat dy,
                gpointer user_data)
{
    gfloat slop = CLICK_SLOP;

    (void)viewport;
    (void)user_data;
    view.pointer_x = u;
    view.pointer_y = v;
    if (view.button_down[1] || view.button_down[2]) {
        view.drag_dx += dx;
        view.drag_dy += dy;
    } else if (view.button_down[0]) {
        if (!view.dragged &&
            (fabsf(u - view.press_x) > slop || fabsf(v - view.press_y) > slop))
            view.dragged = TRUE;
        if (view.dragged) {
            view.drag_dx += dx;
            view.drag_dy += dy;
        }
    }
}

/*
 * A click on somebody opens their conversation, as a sidebar click does.
 * The usage board takes its clicks first: on it, its details panel
 * opens; away from an open panel, the panel closes and nothing else.
 */
static void
on_pointer_press(LrgViewport *viewport, gint button, gfloat u, gfloat v, guint modifiers,
                 gpointer user_data)
{
    (void)viewport;
    if (button < 0 || button > 2)
        return;
    view.button_down[button] = TRUE;
    view.pointer_x = u;
    view.pointer_y = v;
    view.shift_mod = (modifiers & LRG_INPUT_MODIFIER_SHIFT) != 0;
    if (button == 0) {
        view.left_pressed = TRUE;
        view.dragged = FALSE;
        view.press_x = u;
        view.press_y = v;
    }
    clawt_lrg_app_wake(user_data);
}

static void
on_pointer_release(LrgViewport *viewport, gint button, gfloat u, gfloat v, guint modifiers,
                   gpointer user_data)
{
    (void)viewport;
    if (button < 0 || button > 2)
        return;
    view.button_down[button] = FALSE;
    view.pointer_x = u;
    view.pointer_y = v;
    view.shift_mod = (modifiers & LRG_INPUT_MODIFIER_SHIFT) != 0;
    if (button == 0 && !view.dragged) {
        view.click_pending = TRUE;
        view.click_x = u;
        view.click_y = v;
    }
    clawt_lrg_app_wake(user_data);
}

static void
on_pointer_scroll(LrgViewport *viewport, gfloat u, gfloat v, gfloat sx, gfloat sy,
                  gpointer user_data)
{
    (void)viewport;
    (void)sx;
    view.pointer_x = u;
    view.pointer_y = v;
    view.wheel += sy;
    clawt_lrg_app_wake(user_data);
}

/*
 * Press only.  Arrows are camera keys while the viewport is focused, so
 * they must not also move focus.  Escape closes the usage panel when it
 * is open and otherwise leaves, so focus can leave the drawing.
 */
static gboolean
on_viewport_key(LrgViewport *viewport, GrlKey key, LrgInputModifiers modifiers,
                gpointer user_data)
{
    (void)viewport;
    view.shift_mod = (modifiers & LRG_INPUT_MODIFIER_SHIFT) != 0;
    if ((guint)key == CLAWT_OFFICE_KEY_ESCAPE) {
        if (clawt_office_scene_dismiss(view.scene)) {
            clawt_lrg_app_wake(user_data);
            return TRUE;
        }
        return FALSE;
    }
    if (clawt_office_binding_action((guint)key) != CLAWT_OFFICE_BIND_NONE) {
        clawt_lrg_app_wake(user_data);
        return TRUE;
    }
    return FALSE;
}

/* Fit, Pixel-perfect, 1x, 2x.  The zoom is stashed on the button: the
 * callback's user data is the app, which the wake needs. */
static void
on_zoom(LrgButton *button, gpointer user_data)
{
    clawt_office_scene_set_zoom(view.scene,
        (ClawtOfficeZoom)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "zoom")));
    clawt_lrg_app_wake(user_data);
}

static LrgWidget *
zoom_button(const gchar *label, ClawtOfficeZoom zoom, ClawtLrgApp *app)
{
    LrgWidget *button = clawt_lrg_button(label, "flat", G_CALLBACK(on_zoom), app);

    /* FIT is 0, and GINT_TO_POINTER(0) is NULL; reading it back is 0 */
    g_object_set_data(G_OBJECT(button), "zoom", GINT_TO_POINTER((gint)zoom));
    return button;
}

/* 2D and 3D, beside the drawing. */
static void
on_mode(LrgButton *button, gpointer user_data)
{
    (void)button;
    clawt_office_scene_set_mode(view.scene,
        clawt_office_scene_get_mode(view.scene) == CLAWT_OFFICE_SCENE_2D
        ? CLAWT_OFFICE_SCENE_3D : CLAWT_OFFICE_SCENE_2D);
    clawt_lrg_app_wake(user_data);
}

/* Open office, beside the switch: clawtilla-office in the mode drawn here. */
static void
on_open_office(LrgButton *button, gpointer user_data)
{
    (void)button;
    clawt_lrg_office_open_window(user_data,
        clawt_office_scene_get_mode(view.scene) == CLAWT_OFFICE_SCENE_3D
        ? CLAWT_OFFICE_VIEW_3D : CLAWT_OFFICE_VIEW_2D);
}

void
clawt_lrg_office_view_add(ClawtLrgApp *app, LrgWidget *parent)
{
    LrgWidget *box = clawt_lrg_vbox(6.0f);
    LrgWidget *bar = clawt_lrg_hbox(8.0f);

    view.app = app;
    clawt_office_feed_free(view.feed);
    clawt_office_scene_free(view.scene);
    view.scene = clawt_office_scene_new();
    view.last_time = 0.0;
    /* a second visit must not keep the previous page's buttons down */
    view.button_down[0] = FALSE;
    view.button_down[1] = FALSE;
    view.button_down[2] = FALSE;
    view.left_pressed = FALSE;
    view.click_pending = FALSE;
    view.dragged = FALSE;
    view.shift_mod = FALSE;
    view.drag_dx = 0.0f;
    view.drag_dy = 0.0f;
    view.wheel = 0.0f;

    view.viewport = lrg_viewport_new();
    lrg_viewport_set_mode(view.viewport, LRG_VIEWPORT_MODE_RENDER);
    lrg_viewport_set_continuous(view.viewport, TRUE);
    lrg_viewport_set_fit(view.viewport, LRG_VIEWPORT_FIT_CONTAIN);
    lrg_viewport_set_render_func(view.viewport, render, app, NULL);
    lrg_widget_set_min_size(LRG_WIDGET(view.viewport), 320.0f, VIEW_HEIGHT);
    g_signal_connect(view.viewport, "pointer-move", G_CALLBACK(on_pointer_move), app);
    g_signal_connect(view.viewport, "pointer-press", G_CALLBACK(on_pointer_press), app);
    g_signal_connect(view.viewport, "pointer-release", G_CALLBACK(on_pointer_release), app);
    g_signal_connect(view.viewport, "pointer-scroll", G_CALLBACK(on_pointer_scroll), app);
    g_signal_connect(view.viewport, "key", G_CALLBACK(on_viewport_key), app);

    clawt_lrg_take(bar, clawt_lrg_button("2D / 3D", "flat", G_CALLBACK(on_mode), app));
    clawt_lrg_take(bar, clawt_lrg_button("Open office", "flat", G_CALLBACK(on_open_office),
                                         app));
    clawt_lrg_take(bar, zoom_button("Fit", CLAWT_OFFICE_ZOOM_FIT, app));
    clawt_lrg_take(bar, zoom_button("Pixel-perfect", CLAWT_OFFICE_ZOOM_PIXEL, app));
    clawt_lrg_take(bar, zoom_button("1x", CLAWT_OFFICE_ZOOM_1X, app));
    clawt_lrg_take(bar, zoom_button("2x", CLAWT_OFFICE_ZOOM_2X, app));
    {
        /*
         * One line beside a fixed button: it takes the row's slack and
         * ellipsizes, or at the minimum window it pushed past the edge
         * (/lrgui/widgets/every-page-fits).
         */
        LrgWidget *hint = clawt_lrg_label(
            "Right-drag to look, WASD to move, Q/E to turn, wheel to zoom, F to reset. "
            "Click somebody to open their conversation.",
            "muted");

        lrg_label_set_ellipsize(LRG_LABEL(hint), TRUE);
        lrg_widget_set_fill_weight(hint, 1.0f);
        clawt_lrg_take(bar, hint);
    }
    clawt_lrg_take(box, bar);
    clawt_lrg_add(box, LRG_WIDGET(view.viewport));
    g_object_unref(view.viewport);
    clawt_lrg_take(parent, clawt_lrg_card(box));

    view.feed = clawt_office_feed_new(app->client, app->context, NULL, &feed_funcs, app);
}

void
clawt_lrg_office_view_event(ClawtLrgApp *app, ClawtEvent *event)
{
    (void)app;
    if (view.feed != NULL)
        clawt_office_feed_event(view.feed, event);
}

#endif /* CLAWT_HAVE_OFFICE */
