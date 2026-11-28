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
 *     the docs and tests.  With --pack it needs no daemon at all: the
 *     pack is read from disk and the layout and usage from files, which
 *     is how tests/test-office-board-shots.sh draws a fixed fixture;
 *   - --embed FD: draw offscreen and stream the frames over the socket
 *     FD with libregnum's frame protocol, taking input back, for the GTK
 *     client, which never links libregnum.  A click on somebody is
 *     written to stdout as {"event":"open_agent","agent":"<id>"}, and
 *     what is drawn as {"event":"view","mode":"2d","has_3d":true}
 *     whenever that changes.  Lines on stdin say how to draw:
 *     {"mode":"3d","focus":"chief","zoom":"fit","ui_scale":2,"usage":"world"}
 *     (clawt_office_view_control_line()), so switching needs no restart.
 *     A click on the usage board pins its details panel open, and the
 *     Escape key (sent over the frame socket) closes it.
 *
 * It is an ordinary client of the daemon: --socket, --profile or --host,
 * as every other client takes them.  Run bare, it opens on the daemon a
 * running GTK or lrg client is on, then the one the clients were last
 * switched to, then the local one (clawt_connection_choose_following()),
 * so the office drawn is the fleet on screen beside it.
 */

#include "office-feed.h"
#include "office-scene.h"

#include <libregnum.h>
#include <raylib.h>
#include <gio/gunixinputstream.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLAWT_OFFICE_BIN_NAME "clawtilla-office"
#define FRAME_MS              (33)
#define SIMULATE_EVERY_S      (4.0f)

static gchar    *opt_socket = NULL;
static gchar    *opt_profile = NULL;
static gchar    *opt_host = NULL;
static gint      opt_port = 0;
static gchar    *opt_token = NULL;
static gchar    *opt_token_file = NULL;
static gchar    *opt_world = NULL;
static gboolean  opt_2d = FALSE;
static gboolean  opt_3d = FALSE;
static gboolean  opt_simulate = FALSE;
static gchar    *opt_screenshot = NULL;
static gint      opt_frames = 90;
static gint      opt_embed = -1;
static gint      opt_width = 1280;
static gint      opt_height = 720;
static gchar   **opt_adapter_dirs = NULL;
static gchar    *opt_focus = NULL;
static gchar    *opt_camera = NULL;
static gchar    *opt_zoom = NULL;
static gint      opt_ui_scale = 1;
static gchar    *opt_usage = NULL;
static gboolean  opt_usage_open = FALSE;
static gchar    *opt_usage_report = NULL;
static gchar    *opt_pack = NULL;
static gchar    *opt_layout_file = NULL;
static gchar    *opt_usage_file = NULL;
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
    { "token-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_token_file,
      "Read the bearer token for --host from this file", "FILE" },
    { "token", 0, 0, G_OPTION_ARG_STRING, &opt_token,
      "Bearer token for --host (deprecated: other users can read it; "
      "use --token-file or --profile)", "TOKEN" },
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
    { "focus", 'f', 0, G_OPTION_ARG_STRING, &opt_focus,
      "Follow this participant with the 3D camera, close up", "ID" },
    { "camera", 0, 0, G_OPTION_ARG_STRING, &opt_camera,
      "Snap the 3D camera to x,z,yaw,pitch,distance (tiles and radians) "
      "and stop Fit and --focus from moving it", "X,Z,YAW,PITCH,DIST" },
    { "zoom", 'z', 0, G_OPTION_ARG_STRING, &opt_zoom,
      "Zoom the 2D map: fit (default), pixel, 1x or 2x", "ZOOM" },
    { "ui-scale", 0, 0, G_OPTION_ARG_INT, &opt_ui_scale,
      "Draw text this many times larger, for a HiDPI screen (1-4, default 1)", "N" },
    { "usage", 'u', 0, G_OPTION_ARG_STRING, &opt_usage,
      "Provider usage: world (on the usage_board wall, the default), corner or off",
      "WHERE" },
    { "usage-open", 0, 0, G_OPTION_ARG_NONE, &opt_usage_open,
      "Open the usage details panel beside the board, as a click does", NULL },
    { "usage-report", 0, 0, G_OPTION_ARG_FILENAME, &opt_usage_report,
      "With --screenshot: also write where the usage board and panel were drawn, "
      "as JSON", "FILE" },
    { "pack", 0, 0, G_OPTION_ARG_FILENAME, &opt_pack,
      "Draw the world pack in this directory, with no daemon (needs --layout-file)",
      "DIR" },
    { "layout-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_layout_file,
      "With --pack: who sits where, as office.layout answers it", "FILE" },
    { "usage-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_usage_file,
      "With --pack: provider usage, as usage.providers answers it; its \"now\" is "
      "the board's clock", "FILE" },
    { "adapter-dir", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_adapter_dirs,
      "Look for world adapter modules here first; repeatable", "DIR" },
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
    "In a window: Tab switches 2D and 3D.  Right-drag looks, middle-drag\n"
    "or Shift+right-drag pans the floor, W/A/S/D and the arrows move,\n"
    "Q/E turn, the wheel zooms toward the cursor, F or Home resets to\n"
    "Fit.  Shift moves and turns faster.  A click on somebody prints\n"
    "them.  U cycles where provider usage is shown (world, corner, off),\n"
    "a click on the usage board opens its details, Escape closes them --\n"
    "and quits when nothing is open.  In 2D, W/A/S/D and the arrows pan,\n"
    "the wheel zooms around the cursor, and F resets to Fit.\n"
    "\n"
    "Embedded (--embed FD), the same controls arrive as frame-protocol\n"
    "keys and buttons, and a line of JSON on stdin switches the view\n"
    "without a restart:\n"
    "{\"mode\":\"3d\",\"focus\":\"chief\",\"zoom\":\"fit\",\"ui_scale\":2,\n"
    "\"usage\":\"world\"}.\n"
    "Each member is optional.\n"
    "\n"
    "Which daemon: --host, --profile or --socket when given; otherwise\n"
    "the one a running clawtilla-gtk or clawtilla-lrg is on, then the one\n"
    "the clients were last switched to, then the local one.  It says which\n"
    "on stderr.  A --host takes its token from --token-file; --token still\n"
    "works but puts the token where other users can read it.\n"
    "\n"
    "Examples:\n"
    "  clawtilla-office\n"
    "  clawtilla-office --3d --world cozy-town\n"
    "  clawtilla-office --profile workstation\n"
    "  clawtilla-office --host 10.0.0.9 --token-file ~/.config/clawtilla/ws.token\n"
    "  clawtilla-office --simulate\n"
    "  clawtilla-office --screenshot office.png --width 1600 --height 900\n"
    "  clawtilla-office --adapter-dir ~/src/my-world/build\n"
    "  clawtilla-office --3d --focus chief\n"
    "  clawtilla-office --3d --camera 22,20,2.0,0.76,40\n"
    "  clawtilla-office --zoom 2x --focus chief\n"
    "  clawtilla-office --screenshot office@2x.png --width 3840 --height 2160 --ui-scale 2\n"
    "  clawtilla-office --usage corner\n"
    "  clawtilla-office --screenshot board.png --zoom 2x --usage-open\n"
    "  clawtilla-office --pack data/office/worlds/cozy-town --layout-file layout.json \\\n"
    "      --usage-file usage.json --screenshot cozy.png --usage-report cozy.json\n";

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
    ClawtOfficeViewMode want_mode;   /* asked for: --3d, or the last control line */
    gint              reported_mode; /* the last "view" event: -1 before the first */
    gint              reported_3d;
    gboolean          press_down;    /* embed: a left press not yet released */
    gboolean          dragged;       /* ...that moved far enough to be a drag */
    gboolean          edge_left;     /* a left press this dispatch, for a double-click */
    gboolean          click_pending; /* a left release that may still open somebody */
    gboolean          have_pointer;  /* embed: last_x/y are a real sample */
    gfloat            press_x;
    gfloat            press_y;
    gfloat            click_x;
    gfloat            click_y;
    gboolean          held_key[360]; /* raylib codes held, from the embed's key signal */
    gboolean          key_edge[360]; /* went down since the previous frame */
    gint              ui_scale;      /* --ui-scale, or the last control line's */
} Office;

/* how far a press may wander, in logical pixels, and still be a click */
#define CLICK_SLOP (4.0f)

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

static void
on_character(const gchar *agent, const gchar *dir, JsonObject *character,
             gpointer user_data)
{
    clawt_office_scene_set_character(((Office *)user_data)->scene, agent, dir,
                                     character);
}

static void
on_usage(JsonObject *usage, gpointer user_data)
{
    clawt_office_scene_set_usage(((Office *)user_data)->scene, usage);
}

static void
on_look(const gchar *id, const gchar *name, const gchar *face, gboolean has_avatar,
        gpointer user_data)
{
    clawt_office_scene_set_look(((Office *)user_data)->scene, id, name, face, has_avatar);
}

static void
on_portrait(const gchar *id, const guint8 *rgba, gint size, const gchar *etag,
            gpointer user_data)
{
    clawt_office_scene_set_portrait(((Office *)user_data)->scene, id, rgba, size, etag);
}

static const ClawtOfficeFeedFuncs feed_funcs = {
    on_world, on_layout, on_color, on_status, on_character, on_usage,
    on_look, on_portrait
};

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
        /* what was asked for, which a control line may have changed since --3d */
        clawt_office_scene_set_mode(office->scene,
                                    office->want_mode == CLAWT_OFFICE_VIEW_3D
                                    ? CLAWT_OFFICE_SCENE_3D : CLAWT_OFFICE_SCENE_2D);
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

/*
 * {"event":"view","mode":"2d","has_3d":true}, when either changed: the
 * host shows what is drawn, which a world without a level keeps at 2D
 * whatever was asked.
 */
static void
announce_view(Office *office)
{
    gint mode = clawt_office_scene_get_mode(office->scene) == CLAWT_OFFICE_SCENE_3D &&
                clawt_office_scene_has_3d(office->scene) ? 1 : 0;
    gint has_3d = clawt_office_scene_has_3d(office->scene) ? 1 : 0;

    if (!office->loaded || (mode == office->reported_mode && has_3d == office->reported_3d))
        return;
    office->reported_mode = mode;
    office->reported_3d = has_3d;
    fprintf(stdout, "{\"event\":\"view\",\"mode\":\"%s\",\"has_3d\":%s}\n",
            mode ? "3d" : "2d", has_3d ? "true" : "false");
    fflush(stdout);
}

/* One control line from the host, applied whole or not at all. */
static void
apply_control(Office *office, const gchar *line)
{
    ClawtOfficeViewControl control;
    g_autoptr(GError) error = NULL;

    if (*line == '\0')
        return;
    if (!clawt_office_view_control_parse(line, &control, &error)) {
        g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
        return;
    }
    if (control.ui_scale > 0) {
        office->ui_scale = control.ui_scale;
        clawt_office_scene_set_ui_scale(office->scene, control.ui_scale);
    }
    if (control.has_zoom)
        clawt_office_scene_set_zoom(office->scene, control.zoom);
    if (control.has_focus)
        clawt_office_scene_set_focus(office->scene, control.focus);
    if (control.has_usage)
        clawt_office_scene_set_usage_placement(office->scene, control.usage);
    if (control.has_mode) {
        office->want_mode = control.mode;
        clawt_office_scene_set_mode(office->scene,
                                    control.mode == CLAWT_OFFICE_VIEW_3D
                                    ? CLAWT_OFFICE_SCENE_3D : CLAWT_OFFICE_SCENE_2D);
    }
    clawt_office_view_control_clear(&control);
}

typedef struct {
    Office           *office;
    GDataInputStream *in;
} ControlReader;

static void control_next(ControlReader *reader);

static void
on_control_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ControlReader *reader = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = g_data_input_stream_read_line_finish_utf8(
        G_DATA_INPUT_STREAM(source), result, NULL, &error);

    /* end of stdin (or a host that never writes): the view stays as it is */
    if (line == NULL) {
        if (error != NULL)
            g_printerr("%s: stdin: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
        g_object_unref(reader->in);
        g_free(reader);
        return;
    }
    apply_control(reader->office, line);
    control_next(reader);
}

static void
control_next(ControlReader *reader)
{
    g_data_input_stream_read_line_async(reader->in, G_PRIORITY_DEFAULT, NULL,
                                        on_control_line, reader);
}

/*
 * Control lines from the host on stdin, read on the context pump()
 * drains, so a line is applied between frames.  Started before the
 * client connects: a line written at spawn is waiting before the world
 * is, and the world loads in the mode it asked for.
 */
static void
read_control(Office *office)
{
    ControlReader *reader = g_new0(ControlReader, 1);
    g_autoptr(GInputStream) raw = g_unix_input_stream_new(STDIN_FILENO, FALSE);

    reader->office = office;
    reader->in = g_data_input_stream_new(raw);
    control_next(reader);
}

/* A JSON file's top-level object, or NULL with @error set. */
static JsonObject *
read_json_object(const gchar *path, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;

    if (!json_parser_load_from_file(parser, path, error))
        return NULL;
    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "%s is not a JSON object", path);
        return NULL;
    }
    return json_object_ref(json_node_get_object(root));
}

/*
 * --pack: no daemon.  The pack is read from disk as the daemon would
 * serve it (clawt_office_world_to_json()), the layout and the usage from
 * the files given, and the board's clock is the usage file's `now`, so
 * the same fixture draws the same frame every time.
 */
static gboolean
load_offline(Office *office, GError **error)
{
    g_autoptr(ClawtOfficeWorld) world = clawt_office_world_load(opt_pack, error);
    g_autoptr(JsonNode) world_json = NULL;
    g_autoptr(JsonObject) layout = NULL;

    if (world == NULL)
        return FALSE;
    world_json = clawt_office_world_to_json(world);
    layout = read_json_object(opt_layout_file, error);
    if (layout == NULL)
        return FALSE;
    on_world(opt_pack, json_node_get_object(world_json), office);
    on_layout(layout, office);
    if (opt_usage_file != NULL) {
        g_autoptr(JsonObject) usage = read_json_object(opt_usage_file, error);

        if (usage == NULL)
            return FALSE;
        if (json_object_has_member(usage, "now"))
            clawt_office_scene_set_clock(office->scene, json_object_get_int_member(usage, "now"));
        clawt_office_scene_set_usage(office->scene, usage);
    }
    return TRUE;
}

/* ── The three ways to run ──────────────────────────────────────── */

/* The binding table, sampled from raylib.  U and Tab stay outside it. */
static void
note_window_keys(ClawtOfficeInput *input)
{
    guint i;

    for (i = 0; i < clawt_office_binding_count(); i++) {
        guint key = clawt_office_binding_key(i);

        clawt_office_input_note_key(input, key, IsKeyPressed((int)key),
                                    IsKeyDown((int)key));
    }
}

static gint
run_window(Office *office)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt_width, opt_height, "clawtilla office");
    /* Escape closes the usage panel first, and quits only when none is open */
    SetExitKey(KEY_NULL);
    while (!WindowShouldClose()) {
        gfloat dt = GetFrameTime();
        gint sw = MAX(GetScreenWidth(), 1);
        gint sh = MAX(GetScreenHeight(), 1);
        gint rw = GetRenderWidth();
        gint rh = GetRenderHeight();
        gfloat sx = (gfloat)rw / (gfloat)sw;
        gfloat sy = (gfloat)rh / (gfloat)sh;
        Vector2 mouse = GetMousePosition();
        Vector2 delta = GetMouseDelta();
        ClawtOfficeInput input;
        const gchar *under;
        gboolean eat;
        gboolean left_pressed;
        gfloat slop;

        memset(&input, 0, sizeof(input));
        pump(office->context, 1);
        load_pending(office);
        simulate(office, dt);

        if (IsKeyPressed(KEY_ESCAPE) && !clawt_office_scene_dismiss(office->scene))
            break;
        if (IsKeyPressed(KEY_U))
            clawt_office_scene_set_usage_placement(office->scene,
                (ClawtOfficeUsagePlacement)((clawt_office_scene_get_usage_placement(
                    office->scene) + 1) % 3));
        if (IsKeyPressed(KEY_TAB))
            clawt_office_scene_set_mode(office->scene,
                clawt_office_scene_get_mode(office->scene) == CLAWT_OFFICE_SCENE_2D
                ? CLAWT_OFFICE_SCENE_3D : CLAWT_OFFICE_SCENE_2D);

        input.dt = dt;
        input.view_w = (gfloat)rw;
        input.view_h = (gfloat)rh;
        input.x = mouse.x * sx;
        input.y = mouse.y * sy;
        input.pointer_valid = TRUE;
        input.wheel = GetMouseWheelMove();
        input.left_down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        input.right_down = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
        input.middle_down = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
        left_pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        input.left_pressed = left_pressed;
        if (left_pressed) {
            office->press_x = input.x;
            office->press_y = input.y;
            office->dragged = FALSE;
        }
        slop = CLICK_SLOP * (gfloat)office->ui_scale;
        if (input.left_down && !office->dragged &&
            (fabsf(input.x - office->press_x) > slop ||
             fabsf(input.y - office->press_y) > slop))
            office->dragged = TRUE;
        /*
         * Right and middle move the 3D camera at once.  A left drag pans
         * the 2D map, and only once it has left the click.
         */
        if (input.right_down || input.middle_down ||
            (input.left_down && office->dragged &&
             clawt_office_scene_get_mode(office->scene) != CLAWT_OFFICE_SCENE_3D)) {
            input.dx = delta.x * sx;
            input.dy = delta.y * sy;
        }
        note_window_keys(&input);
        eat = clawt_office_scene_input(office->scene, &input);

        under = clawt_office_scene_pointer(office->scene, input.x, input.y);
        if (left_pressed && !eat &&
            !clawt_office_scene_click(office->scene, input.x, input.y) && under != NULL)
            announce_open(under);

        clawt_office_scene_update(office->scene, dt);
        BeginDrawing();
        clawt_office_scene_draw(office->scene, rw, rh);
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
        /* the deadline is for loading; settling a slow software renderer may take longer */
        if (settled == 0 && g_get_monotonic_time() > deadline) {
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
    if (ok && opt_usage_report != NULL) {
        g_autoptr(JsonObject) report = clawt_office_scene_describe_usage(office->scene);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        g_autoptr(JsonGenerator) gen = json_generator_new();
        g_autoptr(GError) error = NULL;

        json_node_set_object(node, report);
        json_generator_set_root(gen, node);
        json_generator_set_pretty(gen, TRUE);
        if (!json_generator_to_file(gen, opt_usage_report, &error)) {
            g_printerr("%s: could not write %s: %s\n", CLAWT_OFFICE_BIN_NAME,
                       opt_usage_report, error->message);
            ok = FALSE;
        }
    }
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
 * A press, from the signal rather than the button's state: a click is a
 * press and a release inside one dispatch, and polling "is it down" once
 * a frame never saw it.
 */
static void
on_inject_button(LrgInputInject *in, guint button, gboolean pressed, gfloat x, gfloat y,
                 gpointer user_data)
{
    Office *office = user_data;

    (void)in;
    /*
     * Right and middle are read with is_button_down() after the
     * dispatch, so the drag keeps the button that started it.  A left
     * click opens after scene_input(), which may have eaten it as a
     * double-click on the floor.  Press and release of one click both
     * arrive inside this dispatch.
     */
    if (button != 0)
        return;
    if (pressed) {
        office->press_down = TRUE;
        office->dragged = FALSE;
        office->press_x = x;
        office->press_y = y;
        office->edge_left = TRUE;
        return;
    }
    if (!office->press_down)
        return;
    office->press_down = FALSE;
    if (office->dragged)
        return;
    office->click_pending = TRUE;
    office->click_x = x;
    office->click_y = y;
}

/* Movement keys, Shift, F, Home and Escape.  Anything else is ignored. */
static void
on_inject_key(LrgInputInject *in, guint key, gboolean pressed, guint modifiers,
              guint codepoint, gpointer user_data)
{
    Office *office = user_data;

    (void)in;
    (void)modifiers;
    (void)codepoint;
    if (key < G_N_ELEMENTS(office->held_key)) {
        if (pressed)
            office->key_edge[key] = TRUE;
        office->held_key[key] = pressed;
    }
    if (pressed && key == (guint)KEY_ESCAPE)
        clawt_office_scene_dismiss(office->scene);
}

static void
note_embed_keys(Office *office, ClawtOfficeInput *input)
{
    guint i;

    for (i = 0; i < clawt_office_binding_count(); i++) {
        guint key = clawt_office_binding_key(i);

        if (key >= G_N_ELEMENTS(office->held_key))
            continue;
        clawt_office_input_note_key(input, key, office->key_edge[key], office->held_key[key]);
        office->key_edge[key] = FALSE;
    }
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
    guint w = (guint)opt_width;
    guint h = (guint)opt_height;
    gfloat last_x = 0.0f;
    gfloat last_y = 0.0f;

    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt_width, opt_height, "clawtilla office");
    out = lrg_frame_export_new(opt_embed, w, h, TRUE);
    in = lrg_input_inject_new(opt_embed);
    g_signal_connect(in, "button", G_CALLBACK(on_inject_button), office);
    g_signal_connect(in, "key", G_CALLBACK(on_inject_key), office);

    for (;;) {
        gfloat x = 0.0f;
        gfloat y = 0.0f;
        gfloat sx = 0.0f;
        gfloat sy = 0.0f;
        guint nw = 0;
        guint nh = 0;

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
        /* horizontal wheel is not a camera control; the vertical one dollies */
        (void)sx;
        {
            ClawtOfficeInput input;
            gboolean left = lrg_input_inject_is_button_down(in, 0);
            gboolean right = lrg_input_inject_is_button_down(in, 1);
            gboolean middle = lrg_input_inject_is_button_down(in, 2);
            gboolean eat;
            gfloat dx = 0.0f;
            gfloat dy = 0.0f;
            gfloat slop = CLICK_SLOP * (gfloat)office->ui_scale;

            memset(&input, 0, sizeof(input));
            if (office->have_pointer) {
                dx = x - last_x;
                dy = y - last_y;
            }
            office->have_pointer = TRUE;
            if (office->press_down && left && !office->dragged &&
                (fabsf(x - office->press_x) > slop || fabsf(y - office->press_y) > slop))
                office->dragged = TRUE;
            input.dt = FRAME_MS / 1000.0f;
            input.view_w = (gfloat)w;
            input.view_h = (gfloat)h;
            input.x = x;
            input.y = y;
            input.pointer_valid = x >= 0.0f && y >= 0.0f;
            input.wheel = sy;
            input.left_down = left;
            input.right_down = right;
            input.middle_down = middle;
            input.left_pressed = office->edge_left;
            office->edge_left = FALSE;
            if (right || middle || (left && office->dragged &&
                clawt_office_scene_get_mode(office->scene) != CLAWT_OFFICE_SCENE_3D)) {
                input.dx = dx;
                input.dy = dy;
            }
            note_embed_keys(office, &input);
            eat = clawt_office_scene_input(office->scene, &input);
            if (office->click_pending && !eat) {
                const gchar *under;

                if (!clawt_office_scene_click(office->scene, office->click_x, office->click_y)) {
                    under = clawt_office_scene_pointer(office->scene, office->click_x,
                                                       office->click_y);
                    if (under != NULL)
                        announce_open(under);
                }
            }
            office->click_pending = FALSE;
            clawt_office_scene_pointer(office->scene, x, y);
        }
        last_x = x;
        last_y = y;

        pump(office->context, FRAME_MS);
        load_pending(office);
        simulate(office, FRAME_MS / 1000.0f);
        announce_view(office);
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

/* Five numbers, commas between, nothing after.  g_ascii_strtod, not the locale. */
static gboolean
parse_camera(const gchar *text, gfloat out[5])
{
    const gchar *p = text;
    guint i;

    if (text == NULL)
        return FALSE;
    for (i = 0; i < 5; i++) {
        gchar *end = NULL;

        if (*p == '\0')
            return FALSE;
        out[i] = (gfloat)g_ascii_strtod(p, &end);
        if (end == p)
            return FALSE;
        p = end;
        if (i < 4) {
            if (*p != ',')
                return FALSE;
            p++;
        }
    }
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == '\0';
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
    if (opt_ui_scale < 1 || opt_ui_scale > CLAWT_OFFICE_VIEW_MAX_UI_SCALE) {
        g_printerr("%s: --ui-scale must be between 1 and %d\n", CLAWT_OFFICE_BIN_NAME,
                   CLAWT_OFFICE_VIEW_MAX_UI_SCALE);
        return EXIT_FAILURE;
    }
    {
        ClawtOfficeZoom zoom = CLAWT_OFFICE_ZOOM_FIT;

        if (opt_zoom != NULL && !clawt_office_zoom_from_nick(opt_zoom, &zoom)) {
            g_printerr("%s: --zoom is fit, pixel, 1x or 2x, not \"%s\"\n",
                       CLAWT_OFFICE_BIN_NAME, opt_zoom);
            return EXIT_FAILURE;
        }
    }
    {
        ClawtOfficeUsagePlacement usage = CLAWT_OFFICE_USAGE_WORLD;

        if (opt_usage != NULL && !clawt_office_usage_placement_from_nick(opt_usage, &usage)) {
            g_printerr("%s: --usage is world, corner or off, not \"%s\"\n",
                       CLAWT_OFFICE_BIN_NAME, opt_usage);
            return EXIT_FAILURE;
        }
    }
    if (opt_pack != NULL && (opt_layout_file == NULL || opt_embed >= 0)) {
        g_printerr("%s: --pack needs --layout-file, and draws in a window or for "
                   "--screenshot, never --embed\n", CLAWT_OFFICE_BIN_NAME);
        return EXIT_FAILURE;
    }
    if (opt_pack == NULL && (opt_layout_file != NULL || opt_usage_file != NULL)) {
        g_printerr("%s: --layout-file and --usage-file go with --pack\n",
                   CLAWT_OFFICE_BIN_NAME);
        return EXIT_FAILURE;
    }
    if (opt_usage_report != NULL && opt_screenshot == NULL) {
        g_printerr("%s: --usage-report goes with --screenshot\n", CLAWT_OFFICE_BIN_NAME);
        return EXIT_FAILURE;
    }

    /*
     * Still accepted -- scripts use it -- but said: a token in argv is in
     * /proc/PID/cmdline for every user on the machine, and in the shell's
     * history for whoever reads that.
     */
    if (opt_token != NULL)
        g_printerr("%s: --token is deprecated: other users can read it; "
                   "use --token-file FILE or a saved --profile\n",
                   CLAWT_OFFICE_BIN_NAME);

    office.context = g_main_context_ref(g_main_context_default());
    office.scene = clawt_office_scene_new();
    clawt_office_scene_set_adapter_dirs(office.scene, (const gchar *const *)opt_adapter_dirs);
    clawt_office_scene_set_focus(office.scene, opt_focus);
    clawt_office_scene_set_ui_scale(office.scene, opt_ui_scale);
    {
        ClawtOfficeZoom zoom = CLAWT_OFFICE_ZOOM_FIT;
        ClawtOfficeUsagePlacement usage = CLAWT_OFFICE_USAGE_WORLD;

        /* checked above: an unknown nick has already failed */
        clawt_office_zoom_from_nick(opt_zoom, &zoom);
        clawt_office_scene_set_zoom(office.scene, zoom);
        clawt_office_usage_placement_from_nick(opt_usage, &usage);
        clawt_office_scene_set_usage_placement(office.scene, usage);
        clawt_office_scene_set_usage_open(office.scene, opt_usage_open);
    }
    if (opt_camera != NULL) {
        gfloat pose[5];

        if (!parse_camera(opt_camera, pose)) {
            g_printerr("%s: --camera is x,z,yaw,pitch,distance, not \"%s\"\n",
                       CLAWT_OFFICE_BIN_NAME, opt_camera);
            clawt_office_scene_free(office.scene);
            g_main_context_unref(office.context);
            return EXIT_FAILURE;
        }
        /* after focus and zoom, so the pose is what the first frame shows */
        clawt_office_scene_set_camera(office.scene, pose[0], pose[1], pose[2],
                                      pose[3], pose[4]);
    }
    office.want_mode = opt_3d ? CLAWT_OFFICE_VIEW_3D : CLAWT_OFFICE_VIEW_2D;
    office.ui_scale = opt_ui_scale;
    office.reported_mode = -1;
    office.reported_3d = -1;
    office.rng = g_rand_new();

    if (opt_pack != NULL) {
        /* no daemon: the pack, the layout and the usage from disk */
        if (!load_offline(&office, &error)) {
            g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
            status = EXIT_FAILURE;
        } else {
            status = opt_screenshot != NULL ? run_screenshot(&office) : run_window(&office);
        }
    } else {
        /*
         * --host, --profile, --socket; else the daemon a running GTK or lrg
         * client is on; else the one they were last switched to; else the
         * local one -- and why, on stderr, every time: an office drawing a
         * different fleet from the window beside it is otherwise a puzzle.
         */
        connection = clawt_connection_choose_following(
            opt_host, opt_port > 0 ? (guint16)opt_port : 0, opt_token,
            opt_token_file, opt_profile, opt_socket, NULL, NULL, NULL, NULL,
            &notice, &error);

        /* the connection has its own copy; this one is not kept about */
        if (opt_token != NULL) {
            memset(opt_token, 0, strlen(opt_token));
            g_clear_pointer(&opt_token, g_free);
        }

        if (connection == NULL) {
            g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
            status = EXIT_FAILURE;
            goto out;
        }
        if (notice != NULL)
            g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, notice);

        if (opt_embed >= 0)
            read_control(&office);
        client = clawt_connection_create_client(connection);
        clawt_client_set_auto_reconnect(client, TRUE);
        if (!clawt_client_connect(client, &error)) {
            g_printerr("%s: %s\n", CLAWT_OFFICE_BIN_NAME, error->message);
            status = EXIT_FAILURE;
            goto out;
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
    }

out:
    clawt_office_scene_free(office.scene);
    g_clear_pointer(&office.layout, json_object_unref);
    g_clear_pointer(&office.world, json_object_unref);
    g_clear_pointer(&office.pending_world, json_object_unref);
    g_free(office.pending_dir);
    g_rand_free(office.rng);
    g_main_context_unref(office.context);
    return status;
}
