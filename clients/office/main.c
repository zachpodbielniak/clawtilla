/*
 * main.c - clawtilla-office: the fleet drawn as people in a building
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three ways to run, one scene (office-scene.c) and one feed
 * (office-feed.c):
 *
 *   - a window of its own;
 *   - --screenshot FILE: draw offscreen until the office has loaded and
 *     settled, write a PNG, exit -- for the custodial team's previews,
 *     the docs and tests;
 *   - --embed FD: draw offscreen and stream the frames over the socket
 *     FD with libregnum's frame protocol, taking input back, for the GTK
 *     client, which never links libregnum.  A click on somebody is
 *     written to stdout as {"event":"open_agent","agent":"<id>"}.
 *
 * It is an ordinary client of the daemon: --socket, --profile or --host,
 * as every other client takes them.
 */

#include "office-feed.h"
#include "office-scene.h"

#include <libregnum.h>
#include <raylib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAWT_OFFICE_BIN_NAME "clawtilla-office"
#define FRAME_MS              (33)
#define SIMULATE_EVERY_S      (4.0f)

static gchar    *opt_socket = NULL;
static gchar    *opt_profile = NULL;
static gchar    *opt_host = NULL;
static gint      opt_port = 0;
static gchar    *opt_token = NULL;
static gchar    *opt_world = NULL;
static gboolean  opt_2d = FALSE;
static gboolean  opt_3d = FALSE;
static gboolean  opt_simulate = FALSE;
static gchar    *opt_screenshot = NULL;
static gint      opt_frames = 90;
static gint      opt_embed = -1;
static gint      opt_width = 1280;
static gint      opt_height = 720;
static gboolean  opt_version = FALSE;
static gboolean  opt_license = FALSE;

static GOptionEntry entries[] = {
    { "socket", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket,
      "Path to the daemon's socket", "FILE" },
    { "profile", 'p', 0, G_OPTION_ARG_STRING, &opt_profile,
      "A saved connection (clawtilla remote add)", "NAME" },
    { "host", 'H', 0, G_OPTION_ARG_STRING, &opt_host,
      "A daemon at this address", "HOST" },
    { "daemon-port", 0, 0, G_OPTION_ARG_INT, &opt_port, "Port for --host", "PORT" },
    { "token", 0, 0, G_OPTION_ARG_STRING, &opt_token, "Bearer token for --host", "TOKEN" },
    { "world", 'w', 0, G_OPTION_ARG_STRING, &opt_world,
      "Draw this world pack rather than the configured one", "ID" },
    { "2d", 0, 0, G_OPTION_ARG_NONE, &opt_2d, "Draw the map from above", NULL },
    { "3d", 0, 0, G_OPTION_ARG_NONE, &opt_3d, "Draw the 3D level", NULL },
    { "simulate", 0, 0, G_OPTION_ARG_NONE, &opt_simulate,
      "Invent activity for the real participants, labelled, here only", NULL },
    { "screenshot", 0, 0, G_OPTION_ARG_FILENAME, &opt_screenshot,
      "Draw offscreen, write this PNG and exit", "FILE" },
    { "frames", 0, 0, G_OPTION_ARG_INT, &opt_frames,
      "Frames to settle before --screenshot (default 90)", "N" },
    { "embed", 0, 0, G_OPTION_ARG_INT, &opt_embed,
      "Stream frames over this socket fd (libregnum frame protocol)", "FD" },
    { "width", 0, 0, G_OPTION_ARG_INT, &opt_width, "Width in pixels (default 1280)", "W" },
    { "height", 0, 0, G_OPTION_ARG_INT, &opt_height, "Height in pixels (default 720)", "H" },
    { "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version, "Print the version", NULL },
    { "license", 0, 0, G_OPTION_ARG_NONE, &opt_license, "Print the licence", NULL },
    { NULL }
};

static const gchar description_text[] =
    "The office draws each agent where the daemon says it is: at its desk\n"
    "while a turn runs, on the phone while two agents talk, round a table\n"
    "in a group round, waving a question mark while it waits on you.\n"
    "Turn it on with `clawtilla office on`.\n"
    "\n"
    "In a window: Tab switches 2D and 3D, the arrow keys or a right-drag\n"
    "turn the 3D camera, the wheel zooms, a click on somebody prints them,\n"
    "Escape quits.\n"
    "\n"
    "Examples:\n"
    "  clawtilla-office\n"
    "  clawtilla-office --3d --world cozy-town\n"
    "  clawtilla-office --profile workstation\n"
    "  clawtilla-office --simulate\n"
    "  clawtilla-office --screenshot office.png --width 1600 --height 900\n";

static const gchar license_text[] =
    "clawtilla-office - the fleet drawn as people in a building\n"
    "Copyright (C) 2026\n"
    "\n"
    "This program is free software: you can redistribute it and/or modify\n"
    "it under the terms of the GNU Affero General Public License as\n"
    "published by the Free Software Foundation, either version 3 of the\n"
    "License, or (at your option) any later version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, but\n"
    "WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n"
    "Affero General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU Affero General Public\n"
    "License along with this program.  If not, see\n"
    "<https://www.gnu.org/licenses/>.\n";

/* ── The host's state ───────────────────────────────────────────── */

typedef struct {
    ClawtOfficeScene *scene;
    GMainContext     *context;
    JsonObject       *world;       /* for simulate mode */
    JsonObject       *layout;      /* the last real one */
    gchar            *pending_dir; /* a world waiting for a GL context to load into */
    JsonObject       *pending_world;
    gboolean          loaded;
    gboolean          have_layout;
    GRand            *rng;
    gfloat            since_simulated;
} Office;

static void
on_world(const gchar *pack_dir, JsonObject *world, gpointer user_data)
{
    Office *office = user_data;

    /* loaded on the drawing side, where the GL context is current */
    g_free(office->pending_dir);
    office->pending_dir = g_strdup(pack_dir);
    g_clear_pointer(&office->pending_world, json_object_unref);
    office->pending_world = json_object_ref(world);
}

static void
on_layout(JsonObject *layout, gpointer user_data)
{
    Office *office = user_data;

    g_clear_pointer(&office->layout, json_object_unref);
    office->layout = json_object_ref(layout);
    office->have_layout = TRUE;
    if (!opt_simulate)
        clawt_office_scene_set_layout(office->scene, layout);
}

static void
on_color(const gchar *id, const gchar *color, gpointer user_data)
{
    clawt_office_scene_set_color(((Office *)user_data)->scene, id, color);
}

static void
on_status(const gchar *text, gpointer user_data)
{
    Office *office = user_data;

    clawt_office_scene_set_status(office->scene,
                                  text != NULL ? text
                                  : opt_simulate ? "Simulation -- not what the fleet is doing"
                                  : NULL);
}

static const ClawtOfficeFeedFuncs feed_funcs = { on_world, on_layout, on_color, on_status };

/* The world, loaded once a GL context exists; the pack's own words on failure. */
static void
load_pending(Office *office)
{
    g_autoptr(GError) error = NULL;

    if (office->pending_dir == NULL)
        return;
    if (!clawt_office_scene_load_world(office->scene, office->pending_dir,
                                       office->pending_world, &error)) {
        g_autofree gchar *line = g_strdup_printf("The world did not load: %s",
                                                 error->message);

        clawt_office_scene_set_status(office->scene, line);
        g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, line);
    } else {
        office->loaded = TRUE;
        g_clear_pointer(&office->world, json_object_unref);
        office->world = json_object_ref(office->pending_world);
        clawt_office_scene_set_mode(office->scene,
                                    opt_3d ? CLAWT_OFFICE_SCENE_3D : CLAWT_OFFICE_SCENE_2D);
        if (opt_simulate)
            clawt_office_scene_set_status(office->scene,
                                          "Simulation -- not what the fleet is doing");
        if (office->layout != NULL && !opt_simulate)
            clawt_office_scene_set_layout(office->scene, office->layout);
    }
    g_clear_pointer(&office->pending_dir, g_free);
    g_clear_pointer(&office->pending_world, json_object_unref);
}

/* Simulate mode re-deals the room every few seconds, from the real cast. */
static void
simulate(Office *office, gfloat delta)
{
    g_autoptr(JsonObject) fake = NULL;

    if (!opt_simulate || !office->loaded || office->layout == NULL)
        return;
    office->since_simulated += delta;
    if (office->since_simulated < SIMULATE_EVERY_S && office->since_simulated > delta)
        return;
    office->since_simulated = delta;
    fake = clawt_office_simulate_layout(office->world, office->layout, office->rng);
    clawt_office_scene_set_layout(office->scene, fake);
}

/* GLib's work, then wait inside GLib for at most @ms so an event wakes us. */
static gboolean
on_ceiling(gpointer user_data)
{
    *(gboolean *)user_data = TRUE;
    return G_SOURCE_REMOVE;
}

static void
pump(GMainContext *context, guint ms)
{
    gboolean done = FALSE;
    GSource *ceiling = g_timeout_source_new(ms);
    guint n = 0;

    g_source_set_callback(ceiling, on_ceiling, &done, NULL);
    g_source_attach(ceiling, context);
    while (!done && n++ < 256)
        g_main_context_iteration(context, TRUE);
    g_source_destroy(ceiling);
    g_source_unref(ceiling);
    while (g_main_context_iteration(context, FALSE))
        ;
}

static void
on_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    (void)client;
    clawt_office_feed_event(user_data, event);
}

static void
announce_open(const gchar *id)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autofree gchar *line = NULL;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "event");
    json_builder_add_string_value(b, "open_agent");
    json_builder_set_member_name(b, "agent");
    json_builder_add_string_value(b, id);
    json_builder_end_object(b);
    json_generator_set_root(gen, json_builder_get_root(b));
    line = json_generator_to_data(gen, NULL);
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
}

/* ── The three ways to run ──────────────────────────────────────── */

static gint
run_window(Office *office)
{
    gboolean dragging = FALSE;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt_width, opt_height, "clawtilla office");
    SetExitKey(KEY_ESCAPE);
    while (!WindowShouldClose()) {
        gfloat dt = GetFrameTime();
        Vector2 mouse = GetMousePosition();
        const gchar *under;

        pump(office->context, 1);
        load_pending(office);
        simulate(office, dt);

        if (IsKeyPressed(KEY_TAB))
            clawt_office_scene_set_mode(office->scene,
                clawt_office_scene_get_mode(office->scene) == CLAWT_OFFICE_SCENE_2D
                ? CLAWT_OFFICE_SCENE_3D : CLAWT_OFFICE_SCENE_2D);
        if (IsKeyDown(KEY_LEFT))
            clawt_office_scene_orbit(office->scene, -1.2f * dt, 0.0f);
        if (IsKeyDown(KEY_RIGHT))
            clawt_office_scene_orbit(office->scene, 1.2f * dt, 0.0f);
        if (GetMouseWheelMove() != 0.0f)
            clawt_office_scene_orbit(office->scene, 0.0f, GetMouseWheelMove());
        dragging = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
        if (dragging)
            clawt_office_scene_orbit(office->scene, GetMouseDelta().x * 0.01f, 0.0f);

        under = clawt_office_scene_pointer(office->scene, mouse.x, mouse.y);
        if (under != NULL && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            announce_open(under);

        clawt_office_scene_update(office->scene, dt);
        BeginDrawing();
        clawt_office_scene_draw(office->scene, GetScreenWidth(), GetScreenHeight());
        EndDrawing();
    }
    CloseWindow();
    return EXIT_SUCCESS;
}

/*
 * Offscreen, into a render texture, until the world has loaded, a layout
 * has arrived and @opt_frames more frames have passed for people to walk
 * to their seats -- or a deadline, so a daemon that never answers is a
 * failure with a reason rather than a hang.
 */
static gint
run_screenshot(Office *office)
{
    RenderTexture2D target;
    Image image;
    gint settled = 0;
    gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    gboolean ok;

    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt_width, opt_height, "clawtilla office");
    target = LoadRenderTexture(opt_width, opt_height);
    while (settled < opt_frames) {
        if (g_get_monotonic_time() > deadline) {
            g_printerr("%s: the office did not load within 30 seconds; is the daemon "
                       "running and office.enabled on?\n", CLAWT_OFFICE_BIN_NAME);
            UnloadRenderTexture(target);
            CloseWindow();
            return EXIT_FAILURE;
        }
        pump(office->context, 5);
        load_pending(office);
        simulate(office, 1.0f / 30.0f);
        if (office->loaded && (office->have_layout || opt_simulate))
            settled++;
        clawt_office_scene_update(office->scene, 1.0f / 30.0f);
        BeginTextureMode(target);
        clawt_office_scene_draw(office->scene, opt_width, opt_height);
        EndTextureMode();
        BeginDrawing();
        EndDrawing();
    }
    image = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&image);
    ok = ExportImage(image, opt_screenshot);
    UnloadImage(image);
    UnloadRenderTexture(target);
    CloseWindow();
    if (!ok) {
        g_printerr("%s: could not write %s\n", CLAWT_OFFICE_BIN_NAME, opt_screenshot);
        return EXIT_FAILURE;
    }
    g_print("%s\n", opt_screenshot);
    return EXIT_SUCCESS;
}

/*
 * For the GTK client: frames out over the socket, input back.  The host
 * owns the size; a click on somebody is a line on stdout.  Ends when the
 * host closes its end.
 */
static gint
run_embed(Office *office)
{
    g_autoptr(LrgFrameExport) out = NULL;
    g_autoptr(LrgInputInject) in = NULL;
    g_autoptr(GError) error = NULL;
    gboolean was_down = FALSE;
    guint w = (guint)opt_width;
    guint h = (guint)opt_height;

    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt_width, opt_height, "clawtilla office");
    out = lrg_frame_export_new(opt_embed, w, h, TRUE);
    in = lrg_input_inject_new(opt_embed);

    for (;;) {
        gfloat x = 0.0f;
        gfloat y = 0.0f;
        gfloat sx = 0.0f;
        gfloat sy = 0.0f;
        guint nw = 0;
        guint nh = 0;
        gboolean down;
        const gchar *under;

        if (!lrg_input_inject_dispatch(in, &error)) {
            /* the host went away: that is the end, not a failure */
            break;
        }
        if (lrg_input_inject_get_size(in, &nw, &nh) && nw > 0 && nh > 0 &&
            (nw != w || nh != h)) {
            w = nw;
            h = nh;
            lrg_frame_export_set_size(out, w, h);
        }
        lrg_input_inject_get_pointer(in, &x, &y);
        lrg_input_inject_take_scroll(in, &sx, &sy);
        if (sy != 0.0f)
            clawt_office_scene_orbit(office->scene, 0.0f, sy);
        if (sx != 0.0f)
            clawt_office_scene_orbit(office->scene, sx * 0.1f, 0.0f);
        under = clawt_office_scene_pointer(office->scene, x, y);
        down = lrg_input_inject_is_button_down(in, 0);
        if (down && !was_down && under != NULL)
            announce_open(under);
        was_down = down;

        pump(office->context, FRAME_MS);
        load_pending(office);
        simulate(office, FRAME_MS / 1000.0f);
        clawt_office_scene_update(office->scene, FRAME_MS / 1000.0f);
        if (lrg_frame_export_begin(out)) {
            clawt_office_scene_draw(office->scene, (gint)w, (gint)h);
            if (!lrg_frame_export_end(out, &error))
                break;
        }
    }
    CloseWindow();
    return EXIT_SUCCESS;
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autofree gchar *notice = NULL;
    ClawtOfficeFeed *feed;
    Office office = { 0 };
    gint status;

    context = g_option_context_new("- the fleet drawn as people in a building");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description_text);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
        return EXIT_FAILURE;
    }
    if (opt_version) {
        g_print("%s %s (%s)\n", CLAWT_OFFICE_BIN_NAME, CLAWT_VERSION_STRING, CLAWT_GIT_SHA);
        return EXIT_SUCCESS;
    }
    if (opt_license) {
        g_print("%s", license_text);
        return EXIT_SUCCESS;
    }
    if (opt_2d && opt_3d) {
        g_printerr("%s: --2d or --3d, not both\n", CLAWT_OFFICE_BIN_NAME);
        return EXIT_FAILURE;
    }
    if (opt_width < 64 || opt_height < 64 || opt_width > 8192 || opt_height > 8192) {
        g_printerr("%s: the size must be between 64 and 8192 pixels\n", CLAWT_OFFICE_BIN_NAME);
        return EXIT_FAILURE;
    }

    connection = clawt_connection_choose(opt_host, opt_port > 0 ? (guint16)opt_port : 0,
                                         opt_token, opt_profile, opt_socket, NULL, NULL,
                                         FALSE, NULL, &notice, &error);
    if (connection == NULL) {
        g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
        return EXIT_FAILURE;
    }
    if (notice != NULL)
        g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, notice);

    office.context = g_main_context_ref(g_main_context_default());
    office.scene = clawt_office_scene_new();
    office.rng = g_rand_new();
    client = clawt_connection_create_client(connection);
    clawt_client_set_auto_reconnect(client, TRUE);
    if (!clawt_client_connect(client, &error)) {
        g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
        return EXIT_FAILURE;
    }
    clawt_client_subscribe_async(client, 0, NULL, NULL, NULL);
    feed = clawt_office_feed_new(client, office.context, opt_world, &feed_funcs, &office);
    g_signal_connect(client, "event", G_CALLBACK(on_event), feed);

    if (opt_embed >= 0)
        status = run_embed(&office);
    else if (opt_screenshot != NULL)
        status = run_screenshot(&office);
    else
        status = run_window(&office);

    clawt_office_feed_free(feed);
    clawt_office_scene_free(office.scene);
    g_clear_pointer(&office.layout, json_object_unref);
    g_clear_pointer(&office.world, json_object_unref);
    g_clear_pointer(&office.pending_world, json_object_unref);
    g_free(office.pending_dir);
    g_rand_free(office.rng);
    g_main_context_unref(office.context);
    return status;
}
