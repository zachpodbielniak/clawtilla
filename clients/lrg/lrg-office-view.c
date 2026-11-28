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

#define VIEW_HEIGHT (440.0f)

static struct {
    ClawtLrgApp      *app;
    LrgViewport      *viewport;
    ClawtOfficeScene *scene;
    ClawtOfficeFeed  *feed;
    gdouble           last_time;
    gfloat            pointer_x;
    gfloat            pointer_y;
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
    if (clawt_office_scene_update(view.scene, MIN(dt, 0.1f)))
        clawt_lrg_app_wake(user_data);
    clawt_office_scene_draw(view.scene, (gint)width, (gint)height);
}

static void
on_pointer_move(LrgViewport *viewport, gfloat u, gfloat v, gfloat dx, gfloat dy,
                gpointer user_data)
{
    (void)viewport;
    (void)user_data;
    view.pointer_x = u;
    view.pointer_y = v;
    clawt_office_scene_pointer(view.scene, u, v);
    if (lrg_viewport_get_dragging(view.viewport))
        clawt_office_scene_orbit(view.scene, dx * 0.01f, 0.0f);
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
    const gchar *under;

    (void)viewport;
    (void)modifiers;
    if (button != 0)
        return;
    if (clawt_office_scene_click(view.scene, u, v)) {
        clawt_lrg_app_wake(user_data);
        return;
    }
    under = clawt_office_scene_pointer(view.scene, u, v);
    if (under != NULL) {
        g_autofree gchar *id = g_strdup(under);

        clawt_lrg_select_agent(user_data, id);
    }
}

static void
on_pointer_scroll(LrgViewport *viewport, gfloat u, gfloat v, gfloat sx, gfloat sy,
                  gpointer user_data)
{
    (void)viewport;
    (void)u;
    (void)v;
    (void)user_data;
    clawt_office_scene_orbit(view.scene, sx * 0.1f, sy);
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

    view.viewport = lrg_viewport_new();
    lrg_viewport_set_mode(view.viewport, LRG_VIEWPORT_MODE_RENDER);
    lrg_viewport_set_continuous(view.viewport, TRUE);
    lrg_viewport_set_fit(view.viewport, LRG_VIEWPORT_FIT_CONTAIN);
    lrg_viewport_set_render_func(view.viewport, render, app, NULL);
    lrg_widget_set_min_size(LRG_WIDGET(view.viewport), 320.0f, VIEW_HEIGHT);
    g_signal_connect(view.viewport, "pointer-move", G_CALLBACK(on_pointer_move), app);
    g_signal_connect(view.viewport, "pointer-press", G_CALLBACK(on_pointer_press), app);
    g_signal_connect(view.viewport, "pointer-scroll", G_CALLBACK(on_pointer_scroll), app);

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
            "Click somebody to open their conversation; drag or scroll to turn the 3D view.",
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
