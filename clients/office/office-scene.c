/*
 * office-scene.c - The drawn office: a world pack and who is in it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Built on libregnum: the 2D view is the pack's Tiled map through
 * LrgTiledMap, the 3D view its .rlevel through LrgWorld3D and the
 * world renderer, and what floats over people -- name plates, speech
 * bubbles, emotes, the arc of a phone call -- is libregnum's world-space
 * UI.  Positions are map pixels throughout; the 3D view divides them by
 * the tile size, which is how the shipped levels are laid out.
 *
 * People walk in a straight line to their seat and stand still once
 * there.  Idle bobbing was the first thing the office visualisations
 * this was modelled on got wrong: a room where everybody bounces reads
 * as busy, which is exactly the wrong signal for an idle fleet.
 */

#include "office-scene.h"

#include <clawtilla.h>
#include <libregnum.h>
#include <raylib.h>
#include <rlgl.h>
#include <gmodule.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * 0.11.0 brought the world-space UI, the Tiled importer and frame export;
 * 0.11.1's graylib decodes JPEG, which glTF models carry as textures --
 * Meshy's do, and before it a generated character drew plain white.
 */
#if !LRG_CHECK_VERSION(0, 11, 1)
#error "The drawn office needs libregnum 0.11.1 or newer at LRG_DIR"
#endif

static void match_skeleton_units(Model *model);

#define WALK_PIXELS_PER_SECOND (150.0f)
#define ARRIVED_PIXELS         (0.6f)
#define BODY_RADIUS            (9.0f)
#define PERSON_SCALE           (1.7f)

/* ── People ─────────────────────────────────────────────────────── */

typedef struct {
    gchar           *id;
    gchar           *name;
    gchar           *kind;
    gchar           *own_color;     /* from the participant: a visitor's own fleet colour */
    gchar           *visiting_from; /* a visitor's fleet, said on hover */
    gboolean         plate_full;    /* the plate says "name · fleet" now */
    gchar           *activity;
    gchar           *pose;
    gchar           *prop;
    gchar           *emote;
    gchar           *facing;
    gchar           *detail;
    Color            color;
    gboolean         colored;     /* a colour was set, not derived */
    gfloat           x, y;        /* where they are, map pixels */
    gfloat           tx, ty;      /* where they are going */
    gfloat           walk_phase;
    gfloat           heading;     /* degrees round y, the way it last walked (3D models) */
    gboolean         placed;      /* has a position at all */
    gboolean         seen;        /* in the latest layout */
    gboolean         leaving;
    gfloat           fade;        /* 1 in the room, 0 gone */
    LrgNameplate    *plate;
    LrgSpeechBubble *bubble;
    LrgEmote        *mark;
    gchar           *bubble_text;
} Actor;

static void
actor_free(Actor *a)
{
    g_free(a->id);
    g_free(a->name);
    g_free(a->kind);
    g_free(a->own_color);
    g_free(a->visiting_from);
    g_free(a->activity);
    g_free(a->pose);
    g_free(a->prop);
    g_free(a->emote);
    g_free(a->facing);
    g_free(a->detail);
    g_free(a->bubble_text);
    g_clear_object(&a->plate);
    g_clear_object(&a->bubble);
    g_clear_object(&a->mark);
    g_free(a);
}

/* The face agent.list named, kept even before the participant is placed. */
typedef struct {
    gchar   *name;
    gchar   *face;
    gboolean has_avatar;
} Look;

/* Decoded picture.  The texture is uploaded in prepare(), when GL is current. */
typedef struct {
    gchar    *etag;
    guint8   *rgba;
    gint      size;
    Texture2D texture;
    gboolean  uploaded;
    gboolean  dirty;
} FacePortrait;

static void
look_free(Look *look)
{
    g_free(look->name);
    g_free(look->face);
    g_free(look);
}

static void
face_portrait_free(FacePortrait *portrait)
{
    if (portrait->uploaded)
        UnloadTexture(portrait->texture);
    g_free(portrait->etag);
    g_free(portrait->rgba);
    g_free(portrait);
}

/* An agent's own character: its sheet, and which row is which animation. */
typedef struct {
    gchar          *path;       /* the sheet, or NULL */
    gchar          *mesh_path;  /* the model, or NULL */
    JsonObject     *layout;
    Texture2D       texture;
    gboolean        loaded;
    gboolean        failed;
    Model           model;
    ModelAnimation *animations;
    gint            n_animations;
    gint            walk_anim;  /* -1: none */
    gfloat          model_scale;
    gfloat          model_floor;
    gboolean        model_loaded;
    gboolean        model_failed;
    gboolean        trimmed;    /* keyed: draw from trim, not the whole frame */
    Rectangle       trim;       /* the visible part, in a frame's coordinates */
} Sheet;

/*
 * A sheet as a texture, its backdrop keyed out first when the character
 * says it has one (a generator's portrait on a plain colour): drawn as
 * it stands, the backdrop is a box round the person.  The rule is
 * libclawt's, shared with the web client.
 */
static Texture2D
load_sheet_texture(Sheet *sheet)
{
    const gchar *background = json_object_get_string_member_with_default(sheet->layout,
                                                                         "background", NULL);
    JsonArray *frame = json_object_has_member(sheet->layout, "frame")
        ? json_object_get_array_member(sheet->layout, "frame") : NULL;
    Image image;
    Texture2D texture;

    if (background == NULL)
        return LoadTexture(sheet->path);
    image = LoadImage(sheet->path);
    if (image.data == NULL)
        return LoadTexture(sheet->path);
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    {
        guint fw = frame != NULL && json_array_get_length(frame) == 2
            ? (guint)json_array_get_int_element(frame, 0) : 0;
        guint fh = frame != NULL && json_array_get_length(frame) == 2
            ? (guint)json_array_get_int_element(frame, 1) : 0;
        guint tx;
        guint ty;
        guint tw;
        guint th;

        clawt_office_character_key_background((guint8 *)image.data, (guint)image.width,
                                              (guint)image.height, (gsize)image.width * 4,
                                              fw, fh, background);
        /* what was backdrop is margin now: draw the person, not the frame */
        sheet->trimmed = clawt_office_character_opaque_bounds((const guint8 *)image.data,
            (guint)image.width, (guint)image.height, (gsize)image.width * 4, fw, fh,
            &tx, &ty, &tw, &th);
        if (sheet->trimmed)
            sheet->trim = (Rectangle){ (gfloat)tx, (gfloat)ty, (gfloat)tw, (gfloat)th };
    }
    texture = LoadTextureFromImage(image);
    UnloadImage(image);
    return texture;
}

static void
sheet_free(Sheet *sheet)
{
    if (sheet->loaded)
        UnloadTexture(sheet->texture);
    if (sheet->animations != NULL)
        UnloadModelAnimations(sheet->animations, sheet->n_animations);
    if (sheet->model_loaded)
        UnloadModel(sheet->model);
    g_free(sheet->mesh_path);
    g_free(sheet->path);
    g_clear_pointer(&sheet->layout, json_object_unref);
    g_free(sheet);
}

struct _ClawtOfficeScene {
    ClawtOfficeSceneMode  mode;
    gboolean              has_2d;
    gboolean              has_3d;
    LrgTiledMap          *map;
    GPtrArray            *tilemaps;   /* one LrgTilemap per tileset */
    gfloat                map_w;      /* pixels */
    gfloat                map_h;
    gfloat                tile;
    gfloat                entrance_x;
    gfloat                entrance_y;
    LrgWorld3D           *world3d;
    LrgLevel             *level;
    LrgWorld3DRenderer   *renderer;
    LrgCamera3D          *lcamera;
    ClawtOfficeCamera    *camera;     /* target, yaw, pitch, distance; shared by every host */
    gfloat                distance_scale; /* Fit 1, 1x 0.6, 2x 0.35, on the framed slant */
    gboolean              manual;     /* a person moved it: Fit and follow stop driving it */
    gboolean              camera_ready; /* the first draw snapped; later changes ease */
    gboolean              zoom_free;  /* a 2D wheel left the Fit/1x/2x choice */
    gfloat                free_zoom;
    gfloat                click_time; /* the scene clock of the previous left press */
    gfloat                click_x;
    gfloat                click_y;

    /* the 3D Fit, for the bearing and view shape it was worked out at */
    gboolean              frame_valid;
    gfloat                frame_yaw;
    gfloat                frame_aspect;
    gfloat                frame_x;
    gfloat                frame_z;
    gfloat                frame_reach;
    GHashTable           *actors;     /* id -> Actor */
    GHashTable           *colors;     /* id -> "#rrggbb" */
    GHashTable           *beams;      /* "a|b" -> LrgBeam */
    GHashTable           *sheets;     /* id -> Sheet */
    GHashTable           *looks;      /* id -> Look */
    GHashTable           *portraits;  /* id -> FacePortrait */
    GQueue                portrait_unload; /* Texture2D *, unloaded in prepare() */
    gchar                *status;
    gchar                *hovered;
    gchar                *focus;      /* who the 3D camera follows, or NULL */
    JsonObject           *usage;      /* usage.providers, for the board */
    LrgWorldAdapter      *adapter;    /* a world adapter drawing the office, or NULL */
    gchar                *adapter_name;
    GModule              *adapter_module;
    GHashTable           *handles;    /* id -> the adapter's actor handle */
    GStrv                 adapter_dirs;
    gboolean              has_board;
    gfloat                board_x;    /* the usage_board's first anchor */
    gfloat                board_y;
    Rectangle             board_rect; /* its zone on the map (rect_2d, or round the anchor) */
    Vector3               board_3d;   /* its 3D anchor, level units */
    ClawtOfficeUsagePlacement usage_placement;
    ClawtOfficeBoard     *board;      /* self->usage, worded; rebuilt as the clock moves */
    gint64                board_built;   /* the second it was worded at; 0: stale */
    gint64                clock_now;  /* a fixed clock for the board, 0 for the real one */
    ClawtOfficeBoardLayout *board_layout;  /* what was drawn on the wall last frame */
    ClawtOfficeBoardLayout *panel_layout;  /* and the details panel, if it was open */
    gboolean              panel_pinned;  /* opened by a click: stays until Escape or a click away */
    gboolean              hover_quiet;   /* closed while hovered: not again until the pointer leaves */
    gboolean              board_hovered;
    gboolean              panel_hovered;
    Camera2D              camera2d;
    RenderTexture2D       baked;      /* the map at 1:1, drawn once */
    gfloat                person_scale;
    gboolean              baked_ok;
    Camera3D              camera3d;
    gint                  last_w;
    gint                  last_h;
    gfloat                clock;
    ClawtOfficeZoom       zoom;       /* how far the 2D map is zoomed */
    gint                  ui_scale;   /* device pixels per logical pixel: text is this much larger */
    gfloat                centre_x;   /* the map point a panned 2D view is centred on */
    gfloat                centre_y;
    gboolean              panned;     /* centre_x/y were set by a drag, not the map's middle */
};

/* name labels pushed down at most this many rows before they are left to hover */
#define LABEL_STEPS (2)
/* and kept this far apart, in logical pixels */
#define LABEL_GAP   (2.0f)

/* Where the 2D view is looking, worked out the same way draw and input do. */
typedef struct {
    gfloat zoom;
    gfloat cx;
    gfloat cy;
} View2D;

static View2D view_2d(ClawtOfficeScene *self, gfloat width, gfloat height);
static void pan_2d(ClawtOfficeScene *self, gfloat view_w, gfloat view_h,
                   gfloat dx, gfloat dy);
static void frame_office_3d(ClawtOfficeScene *self, gfloat yaw, gfloat aspect,
                            gfloat *cx, gfloat *cz, gfloat *reach);
static void camera_refresh(ClawtOfficeScene *self, gint width, gint height);

/* ── Colours ────────────────────────────────────────────────────── */

static Color
parse_color(const gchar *hex, Color fallback)
{
    guint r, g, b;

    if (hex == NULL || hex[0] != '#')
        return fallback;
    if (strlen(hex) == 7 && sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3)
        return (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, 255 };
    if (strlen(hex) == 4 && sscanf(hex + 1, "%1x%1x%1x", &r, &g, &b) == 3)
        return (Color){ (unsigned char)(r * 17), (unsigned char)(g * 17),
                        (unsigned char)(b * 17), 255 };
    return fallback;
}

static Color
with_alpha(Color c, gfloat alpha)
{
    c.a = (unsigned char)CLAMP(alpha * (gfloat)c.a, 0.0f, 255.0f);
    return c;
}

static Color
shade(Color c, gfloat factor)
{
    return (Color){ (unsigned char)MIN(255.0f, c.r * factor),
                    (unsigned char)MIN(255.0f, c.g * factor),
                    (unsigned char)MIN(255.0f, c.b * factor), c.a };
}

/*
 * The colour a client paints this participant's face in: the one the
 * daemon reported, otherwise libclawt's derived tone -- the same
 * decision every other client makes, so the office's kudu is the
 * sidebar's kudu.
 */
static Color
actor_color(ClawtOfficeScene *self, Actor *a)
{
    const gchar *configured = g_hash_table_lookup(self->colors, a->id);

    /* a visitor's colour comes with it: this fleet has no config to look it up in */
    if (configured == NULL)
        configured = a->own_color;

    return parse_color(clawt_avatar_color(a->id, a->name, configured),
                       (Color){ 120, 130, 150, 255 });
}

/* ── Life cycle ─────────────────────────────────────────────────── */

ClawtOfficeScene *
clawt_office_scene_new(void)
{
    ClawtOfficeScene *self = g_new0(ClawtOfficeScene, 1);

    self->tilemaps = g_ptr_array_new_with_free_func(g_object_unref);
    self->actors = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                         (GDestroyNotify)actor_free);
    self->colors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    self->beams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    self->sheets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)sheet_free);
    self->looks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify)look_free);
    self->portraits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify)face_portrait_free);
    g_queue_init(&self->portrait_unload);
    self->handles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->tile = 32.0f;
    self->person_scale = PERSON_SCALE;
    self->camera = clawt_office_camera_new();
    self->distance_scale = 1.0f;
    self->free_zoom = 1.0f;
    self->click_time = -1000.0f;
    self->ui_scale = 1;
    self->status = g_strdup("Loading the office...");
    return self;
}

void
clawt_office_scene_free(ClawtOfficeScene *self)
{
    if (self == NULL)
        return;
    g_hash_table_unref(self->actors);
    g_hash_table_unref(self->colors);
    g_hash_table_unref(self->beams);
    g_hash_table_unref(self->sheets);
    g_hash_table_unref(self->looks);
    g_hash_table_unref(self->portraits);
    /*
     * Teardown, on the host's thread, same as the portraits still in the
     * hash.  A clear from the feed callback only queued these.
     */
    {
        Texture2D *texture;

        while ((texture = g_queue_pop_head(&self->portrait_unload)) != NULL) {
            if (texture->id != 0)
                UnloadTexture(*texture);
            g_free(texture);
        }
    }
    g_ptr_array_unref(self->tilemaps);
    if (self->baked_ok)
        UnloadRenderTexture(self->baked);
    g_clear_object(&self->map);
    g_clear_object(&self->level);
    g_clear_object(&self->world3d);
    g_clear_object(&self->renderer);
    g_clear_object(&self->lcamera);
    g_free(self->status);
    g_free(self->hovered);
    g_free(self->focus);
    g_clear_pointer(&self->usage, json_object_unref);
    g_clear_pointer(&self->board, clawt_office_board_free);
    g_clear_pointer(&self->board_layout, clawt_office_board_layout_free);
    g_clear_pointer(&self->panel_layout, clawt_office_board_layout_free);
    g_hash_table_unref(self->handles);
    g_clear_object(&self->adapter);
    g_free(self->adapter_name);
    /* the module stays loaded: an adapter's types are registered for good */
    g_strfreev(self->adapter_dirs);
    clawt_office_camera_free(self->camera);
    g_free(self);
}

/* The middle of the first anchor of a zone tagged @tag, if there is one. */
static gboolean
zone_anchor(JsonObject *world, const gchar *tag, gfloat *x, gfloat *y)
{
    JsonArray *zones = json_object_has_member(world, "zones")
        ? json_object_get_array_member(world, "zones") : NULL;
    guint i;
    guint t;

    for (i = 0; zones != NULL && i < json_array_get_length(zones); i++) {
        JsonObject *zone = json_array_get_object_element(zones, i);
        JsonArray *tags = json_object_has_member(zone, "tags")
            ? json_object_get_array_member(zone, "tags") : NULL;
        JsonArray *anchors = json_object_has_member(zone, "anchors_2d")
            ? json_object_get_array_member(zone, "anchors_2d") : NULL;

        for (t = 0; tags != NULL && t < json_array_get_length(tags); t++) {
            const gchar *have = json_array_get_string_element(tags, t);

            if (g_strcmp0(have, tag) == 0 && anchors != NULL &&
                json_array_get_length(anchors) >= 1) {
                JsonArray *point = json_array_get_array_element(anchors, 0);

                if (point == NULL || json_array_get_length(point) < 2)
                    continue;
                *x = (gfloat)json_array_get_double_element(point, 0);
                *y = (gfloat)json_array_get_double_element(point, 1);
                return TRUE;
            }
        }
    }
    return FALSE;
}

/*
 * The usage board's zone: its rect_2d on the map when the pack gives one,
 * else five tiles square round its first anchor -- the size the shipped
 * packs' board walls are -- and its first 3D anchor, else the middle of
 * that rect on the floor.
 */
static gboolean
board_zone(JsonObject *world, gfloat tile, Rectangle *rect, Vector3 *at_3d)
{
    JsonArray *zones = json_object_has_member(world, "zones")
        ? json_object_get_array_member(world, "zones") : NULL;
    gfloat ax = 0.0f;
    gfloat ay = 0.0f;
    guint i;
    guint t;

    if (!zone_anchor(world, "usage_board", &ax, &ay))
        return FALSE;
    *rect = (Rectangle){ ax - 2.5f * tile, ay - 2.5f * tile, 5.0f * tile, 5.0f * tile };
    *at_3d = (Vector3){ ax / tile, 0.0f, ay / tile };

    for (i = 0; zones != NULL && i < json_array_get_length(zones); i++) {
        JsonObject *zone = json_array_get_object_element(zones, i);
        JsonArray *tags = json_object_has_member(zone, "tags")
            ? json_object_get_array_member(zone, "tags") : NULL;
        gboolean match = FALSE;

        for (t = 0; tags != NULL && t < json_array_get_length(tags); t++)
            if (g_strcmp0(json_array_get_string_element(tags, t), "usage_board") == 0)
                match = TRUE;
        if (!match)
            continue;
        if (json_object_has_member(zone, "rect_2d")) {
            JsonArray *r = json_object_get_array_member(zone, "rect_2d");

            if (r != NULL && json_array_get_length(r) == 4 &&
                json_array_get_double_element(r, 2) > 0.0 &&
                json_array_get_double_element(r, 3) > 0.0) {
                *rect = (Rectangle){ (gfloat)json_array_get_double_element(r, 0),
                                     (gfloat)json_array_get_double_element(r, 1),
                                     (gfloat)json_array_get_double_element(r, 2),
                                     (gfloat)json_array_get_double_element(r, 3) };
                *at_3d = (Vector3){ (rect->x + rect->width / 2.0f) / tile, 0.0f,
                                    (rect->y + rect->height / 2.0f) / tile };
            }
        }
        if (json_object_has_member(zone, "anchors_3d")) {
            JsonArray *anchors = json_object_get_array_member(zone, "anchors_3d");
            JsonArray *point = anchors != NULL && json_array_get_length(anchors) > 0
                ? json_array_get_array_element(anchors, 0) : NULL;

            if (point != NULL && json_array_get_length(point) == 3)
                *at_3d = (Vector3){ (gfloat)json_array_get_double_element(point, 0),
                                    (gfloat)json_array_get_double_element(point, 1),
                                    (gfloat)json_array_get_double_element(point, 2) };
        }
        break;
    }
    return TRUE;
}

/* ── World adapters ─────────────────────────────────────────────── */

void
clawt_office_scene_set_adapter_dirs(ClawtOfficeScene *self, const gchar *const *dirs)
{
    g_strfreev(self->adapter_dirs);
    self->adapter_dirs = g_strdupv((gchar **)dirs);
}

const gchar *
clawt_office_scene_get_adapter_name(ClawtOfficeScene *self)
{
    return self->adapter != NULL ? self->adapter_name : NULL;
}

/*
 * An adapter by name: `tiled` is libregnum's own (walls, path-finding,
 * seats from the map); anything else is a module named
 * `libclawt-office-adapter-<name>.so`, looked for in the directories the
 * host gave, then the user's, then the install location -- and the error
 * names every place it looked.
 */
static LrgWorldAdapter *
open_adapter(ClawtOfficeScene *self, const gchar *name, const gchar *pack_dir, GError **error)
{
    g_autoptr(GPtrArray) dirs = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *file = NULL;
    g_autofree gchar *plugins = g_path_get_dirname(CLAWT_PLUGIN_DIR);
    g_autoptr(GString) looked = g_string_new(NULL);
    guint i;

    if (g_strcmp0(name, "tiled") == 0)
        return LRG_WORLD_ADAPTER(lrg_tiled_world_new());
    for (i = 0; self->adapter_dirs != NULL && self->adapter_dirs[i] != NULL; i++)
        g_ptr_array_add(dirs, g_strdup(self->adapter_dirs[i]));
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "clawtilla",
                                           "office-adapters", NULL));
    g_ptr_array_add(dirs, g_build_filename(plugins, "office-adapters", NULL));
    file = g_strdup_printf("libclawt-office-adapter-%s.so", name);

    for (i = 0; i < dirs->len; i++) {
        g_autofree gchar *path = g_build_filename(g_ptr_array_index(dirs, i), file, NULL);
        LrgWorldAdapter *(*make)(const gchar *, GError **) = NULL;
        GModule *module;
        LrgWorldAdapter *adapter;

        g_string_append_printf(looked, "%s%s", looked->len > 0 ? ", " : "", path);
        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
            continue;
        module = g_module_open(path, G_MODULE_BIND_LOCAL);
        if (module == NULL) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s would not load: %s", path,
                        g_module_error());
            return NULL;
        }
        if (!g_module_symbol(module, CLAWT_OFFICE_ADAPTER_SYMBOL, (gpointer *)&make) ||
            make == NULL) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s has no %s", path,
                        CLAWT_OFFICE_ADAPTER_SYMBOL);
            g_module_close(module);
            return NULL;
        }
        adapter = make(pack_dir, error);
        if (adapter == NULL || !LRG_IS_WORLD_ADAPTER(adapter)) {
            if (error != NULL && *error == NULL)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "%s made no world adapter", path);
            g_clear_object(&adapter);
            g_module_close(module);
            return NULL;
        }
        /* resident: the adapter's GTypes outlive any one scene */
        g_module_make_resident(module);
        self->adapter_module = module;
        return adapter;
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "no world adapter '%s'; looked for %s", name, looked->str);
    return NULL;
}

/*
 * The adapter loads what the pack names: a file in the pack when the
 * source is one, otherwise the source as it stands (a place in a game's
 * own world, say), which is the adapter's to understand.
 */
static gboolean
use_adapter(ClawtOfficeScene *self, const gchar *pack_dir, JsonObject *spec, GError **error)
{
    const gchar *name = json_object_get_string_member_with_default(spec, "module", NULL);
    const gchar *source = json_object_get_string_member_with_default(spec, "source", NULL);
    g_autofree gchar *in_pack = NULL;
    g_autoptr(LrgWorldAdapter) adapter = NULL;

    adapter = open_adapter(self, name, pack_dir, error);
    if (adapter == NULL)
        return FALSE;
    if (source != NULL && clawt_office_pack_path_is_safe(source)) {
        in_pack = g_build_filename(pack_dir, source, NULL);
        if (!g_file_test(in_pack, G_FILE_TEST_EXISTS))
            g_clear_pointer(&in_pack, g_free);
    }
    if (!lrg_world_adapter_load(adapter, in_pack != NULL ? in_pack : source, error))
        return FALSE;
    lrg_world_adapter_set_camera(adapter, LRG_WORLD_CAMERA_OVERVIEW);
    g_clear_object(&self->adapter);
    self->adapter = g_steal_pointer(&adapter);
    g_free(self->adapter_name);
    self->adapter_name = g_strdup(name);
    g_hash_table_remove_all(self->handles);
    return TRUE;
}

gboolean
clawt_office_scene_load_world(ClawtOfficeScene  *self,
                              const gchar       *pack_dir,
                              JsonObject        *world,
                              GError           **error)
{
    const gchar *map = json_object_get_string_member_with_default(world, "map_2d", NULL);
    const gchar *level = json_object_get_string_member_with_default(world, "level_3d", NULL);
    guint i;

    g_ptr_array_set_size(self->tilemaps, 0);
    if (self->baked_ok)
        UnloadRenderTexture(self->baked);
    self->baked_ok = FALSE;
    g_clear_object(&self->map);
    g_clear_object(&self->level);
    g_clear_object(&self->world3d);
    self->has_2d = FALSE;
    self->has_3d = FALSE;

    /* 2D: the Tiled map, one tilemap per tileset, drawn in turn */
    if (map != NULL) {
        g_autofree gchar *path = g_build_filename(pack_dir, map, NULL);
        gint w;
        gint h;
        gint tw = 32;
        gint th = 32;

        self->map = lrg_tiled_map_new_from_file(path, error);
        if (self->map == NULL)
            return FALSE;
        if (!lrg_tiled_map_load_textures(self->map, error))
            return FALSE;
        for (i = 0; i < lrg_tiled_map_get_n_tilesets(self->map); i++) {
            LrgTilemap *tilemap = lrg_tiled_map_build_tilemap(self->map, i, error);

            if (tilemap == NULL)
                return FALSE;
            g_ptr_array_add(self->tilemaps, tilemap);
        }
        w = (gint)lrg_tiled_map_get_width(self->map);
        h = (gint)lrg_tiled_map_get_height(self->map);
        lrg_tiled_map_get_tile_size(self->map, (guint *)&tw, (guint *)&th);
        self->tile = (gfloat)tw;
        self->map_w = (gfloat)(w * tw);
        self->map_h = (gfloat)(h * th);
        self->has_2d = TRUE;
    }

    /* 3D: the level, from primitives (and any meshes the pack ships) */
    if (level != NULL) {
        g_autofree gchar *path = g_build_filename(pack_dir, level, NULL);
        g_autoptr(GError) why = NULL;

        self->world3d = lrg_world3d_new();
        self->level = lrg_level_load_world3d(path, self->world3d, &why);
        if (self->level == NULL) {
            /* a broken level costs the 3D view, never the office */
            g_warning("office: the 3D level did not load: %s", why->message);
            g_clear_object(&self->world3d);
        } else {
            if (self->renderer == NULL)
                self->renderer = lrg_world3d_renderer_new();
            if (self->lcamera == NULL)
                self->lcamera = lrg_camera3d_new();
            self->has_3d = TRUE;
        }
    }

    /* a world adapter, when the pack names one; the pack's own map if it will not load */
    g_clear_object(&self->adapter);
    if (json_object_has_member(world, "adapter") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(world, "adapter"))) {
        g_autoptr(GError) why = NULL;

        if (!use_adapter(self, pack_dir, json_object_get_object_member(world, "adapter"),
                         &why)) {
            /* nothing to fall back to: the reason is the answer */
            if (!self->has_2d && !self->has_3d) {
                g_propagate_error(error, g_steal_pointer(&why));
                return FALSE;
            }
            g_warning("office: the world adapter did not load, so the pack's own map is "
                      "drawn: %s", why->message);
        }
    }

    if (!self->has_2d && !self->has_3d && self->adapter == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "the world has neither a map nor a level this build can draw");
        return FALSE;
    }
    if (self->map_w <= 0.0f) {
        self->map_w = 44 * 32;
        self->map_h = 40 * 32;
    }

    /* the wall the allowances are drawn on, if the pack has one */
    self->has_board = zone_anchor(world, "usage_board", &self->board_x, &self->board_y) &&
                      board_zone(world, self->tile, &self->board_rect, &self->board_3d);
    /* a new map and board: the 3D Fit is worked out again.
     * A pose set with --camera (manual) is kept; only its aim is held
     * inside the new footprint. */
    self->frame_valid = FALSE;
    if (self->tile > 0.0f)
        clawt_office_camera_set_bounds(self->camera, 0.0f, self->map_w / self->tile,
                                       0.0f, self->map_h / self->tile);
    g_clear_pointer(&self->board_layout, clawt_office_board_layout_free);
    g_clear_pointer(&self->panel_layout, clawt_office_board_layout_free);

    /* where newcomers walk in from, and leavers walk out to */
    if (!zone_anchor(world, "entrance", &self->entrance_x, &self->entrance_y)) {
        self->entrance_x = self->map_w / 2.0f;
        self->entrance_y = self->map_h - self->tile;
    }
    if (self->mode == CLAWT_OFFICE_SCENE_3D && !self->has_3d)
        self->mode = CLAWT_OFFICE_SCENE_2D;
    if (self->mode == CLAWT_OFFICE_SCENE_2D && !self->has_2d)
        self->mode = CLAWT_OFFICE_SCENE_3D;
    clawt_office_scene_set_status(self, NULL);
    return TRUE;
}

/* ── The layout ─────────────────────────────────────────────────── */

static void
replace(gchar **slot, const gchar *value)
{
    if (g_strcmp0(*slot, value) == 0)
        return;
    g_free(*slot);
    *slot = g_strdup(value);
}

static const gchar *
member(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;
    node = json_object_get_member(object, name);
    return JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

static Actor *
actor_for(ClawtOfficeScene *self, const gchar *id)
{
    Actor *a = g_hash_table_lookup(self->actors, id);

    if (a != NULL)
        return a;
    a = g_new0(Actor, 1);
    a->id = g_strdup(id);
    a->plate = lrg_nameplate_new(id);
    lrg_nameplate_set_max_chars(a->plate, 12);
    a->bubble = lrg_speech_bubble_new("");
    lrg_speech_bubble_set_max_width(a->bubble, 180.0f);
    lrg_speech_bubble_set_font_size(a->bubble, 11.0f);
    a->mark = lrg_emote_new_glyph("?");
    lrg_emote_set_size(a->mark, 18.0f);
    /* libregnum's world widgets start shown; these appear only when there is something to say */
    lrg_world_widget_hide(LRG_WORLD_WIDGET(a->bubble), 0.0f);
    lrg_world_widget_hide(LRG_WORLD_WIDGET(a->mark), 0.0f);
    g_hash_table_insert(self->actors, a->id, a);
    return a;
}

/*
 * What floats over a participant, by activity: the loudest thing in the
 * room is somebody waiting on you, then something broken.  A detail
 * line from the daemon is used when it has one ("on a call with kudu").
 */
static const gchar *
mark_for(const gchar *activity, const gchar *emote)
{
    if (g_strcmp0(activity, "waiting_on_you") == 0 || g_strcmp0(emote, "question") == 0)
        return "?";
    if (g_strcmp0(activity, "error") == 0 || g_strcmp0(emote, "alert") == 0)
        return "!";
    if (g_strcmp0(activity, "thinking") == 0 || g_strcmp0(emote, "thought") == 0)
        return "...";
    if (g_strcmp0(emote, "wave") == 0)
        return "o/";
    return NULL;
}

void
clawt_office_scene_set_layout(ClawtOfficeScene *self, JsonObject *layout)
{
    JsonArray *placements = json_object_has_member(layout, "placements")
        ? json_object_get_array_member(layout, "placements") : NULL;
    JsonArray *people = json_object_has_member(layout, "participants")
        ? json_object_get_array_member(layout, "participants") : NULL;
    g_autoptr(GHashTable) by_id = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTableIter iter;
    gpointer value;
    guint i;

    for (i = 0; people != NULL && i < json_array_get_length(people); i++) {
        JsonObject *p = json_array_get_object_element(people, i);
        const gchar *id = member(p, "id");

        if (id != NULL)
            g_hash_table_insert(by_id, (gpointer)id, p);
    }

    g_hash_table_iter_init(&iter, self->actors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        ((Actor *)value)->seen = FALSE;

    for (i = 0; placements != NULL && i < json_array_get_length(placements); i++) {
        JsonObject *pl = json_array_get_object_element(placements, i);
        const gchar *id = member(pl, "id");
        JsonObject *p;
        Actor *a;
        const gchar *mark;
        g_autofree gchar *say = NULL;

        if (id == NULL || !json_object_has_member(pl, "x"))
            continue;
        p = g_hash_table_lookup(by_id, id);
        a = actor_for(self, id);
        a->seen = TRUE;
        a->leaving = FALSE;
        replace(&a->name, p != NULL && member(p, "name") != NULL ? member(p, "name") : id);
        replace(&a->kind, p != NULL ? member(p, "kind") : NULL);
        replace(&a->own_color, p != NULL ? member(p, "color") : NULL);
        replace(&a->visiting_from, p != NULL ? member(p, "visiting_from") : NULL);
        a->plate_full = FALSE;
        replace(&a->activity, member(pl, "activity"));
        replace(&a->pose, member(pl, "pose"));
        replace(&a->prop, member(pl, "prop"));
        replace(&a->emote, member(pl, "emote"));
        replace(&a->facing, member(pl, "facing"));
        replace(&a->detail, p != NULL ? member(p, "detail") : NULL);
        lrg_nameplate_set_text(a->plate, a->name);
        a->color = actor_color(self, a);
        a->tx = (gfloat)json_object_get_double_member(pl, "x");
        a->ty = (gfloat)json_object_get_double_member(pl, "y");
        if (!a->placed) {
            /* a newcomer walks in from the entrance; the first layout seats everyone */
            a->x = g_hash_table_size(self->actors) > 0 && self->clock > 1.0f
                ? self->entrance_x : a->tx;
            a->y = g_hash_table_size(self->actors) > 0 && self->clock > 1.0f
                ? self->entrance_y : a->ty;
            a->placed = TRUE;
            a->fade = 1.0f;
        }

        mark = mark_for(a->activity, a->emote);
        if (mark != NULL) {
            lrg_emote_set_glyph(a->mark, mark);
            if (!lrg_world_widget_is_shown(LRG_WORLD_WIDGET(a->mark)))
                lrg_emote_pop(a->mark, 0.0f);
        } else if (lrg_world_widget_is_shown(LRG_WORLD_WIDGET(a->mark))) {
            lrg_world_widget_hide(LRG_WORLD_WIDGET(a->mark), 0.3f);
        }

        /* a bubble for what needs a sentence: a person waiting, a failure, a call */
        if (g_strcmp0(a->activity, "waiting_on_you") == 0)
            say = g_strdup(a->detail != NULL ? a->detail : "needs you");
        else if (g_strcmp0(a->activity, "error") == 0)
            say = g_strdup(a->detail != NULL ? a->detail : "something broke");
        else if (g_strcmp0(a->activity, "on_call") == 0 && a->facing != NULL &&
                 g_strcmp0(a->id, a->facing) < 0)
            /* one bubble per call: the arc already says who the other end is */
            say = g_strdup_printf("on a call with %s", a->facing);
        if (g_strcmp0(say, a->bubble_text) != 0) {
            g_free(a->bubble_text);
            a->bubble_text = g_steal_pointer(&say);
            if (a->bubble_text != NULL) {
                lrg_speech_bubble_set_text(a->bubble, a->bubble_text);
                lrg_world_widget_show(LRG_WORLD_WIDGET(a->bubble), 0.2f);
            } else {
                lrg_world_widget_hide(LRG_WORLD_WIDGET(a->bubble), 0.3f);
            }
        }
    }

    /*
     * With a world adapter, the adapter walks people (round its walls,
     * to its seats); the scene keeps what floats over them.  Spawned in
     * the actor's colour, sent to the director's seat in map pixels --
     * the Tiled adapter's own units.
     */
    if (self->adapter != NULL) {
        GHashTableIter people_iter;
        gpointer person;

        g_hash_table_iter_init(&people_iter, self->actors);
        while (g_hash_table_iter_next(&people_iter, NULL, &person)) {
            Actor *a = person;
            gpointer handle = NULL;
            g_autoptr(GError) why = NULL;

            if (!a->seen)
                continue;
            if (!g_hash_table_lookup_extended(self->handles, a->id, NULL, &handle)) {
                g_autofree gchar *hex = g_strdup_printf("#%02x%02x%02x", a->color.r,
                                                        a->color.g, a->color.b);
                guint made = lrg_world_adapter_spawn_actor(self->adapter, a->id, hex, &why);

                if (made == 0) {
                    g_warning("office: the adapter would not place %s: %s", a->id,
                              why != NULL ? why->message : "no reason given");
                    continue;
                }
                handle = GUINT_TO_POINTER(made);
                g_hash_table_insert(self->handles, g_strdup(a->id), handle);
            }
            lrg_world_adapter_move_actor(self->adapter, GPOINTER_TO_UINT(handle), a->tx, a->ty,
                                         0.0f, NULL);
        }
    }

    /* whoever is no longer in the layout walks out */
    g_hash_table_iter_init(&iter, self->actors);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Actor *a = value;

        if (!a->seen && !a->leaving) {
            gpointer handle = NULL;

            a->leaving = TRUE;
            a->tx = self->entrance_x;
            a->ty = self->entrance_y;
            if (self->adapter != NULL &&
                g_hash_table_lookup_extended(self->handles, a->id, NULL, &handle))
                lrg_world_adapter_move_actor(self->adapter, GPOINTER_TO_UINT(handle), a->tx,
                                             a->ty, 0.0f, NULL);
        }
    }
}

void
clawt_office_scene_set_color(ClawtOfficeScene *self, const gchar *id, const gchar *color)
{
    Actor *a;

    if (id == NULL)
        return;
    if (color != NULL && *color != '\0')
        g_hash_table_replace(self->colors, g_strdup(id), g_strdup(color));
    else
        g_hash_table_remove(self->colors, id);
    a = g_hash_table_lookup(self->actors, id);
    if (a != NULL)
        a->color = actor_color(self, a);
}

void
clawt_office_scene_set_look(ClawtOfficeScene *self, const gchar *id,
                            const gchar *name, const gchar *face,
                            gboolean has_avatar)
{
    Look *look;

    if (id == NULL)
        return;
    look = g_new0(Look, 1);
    look->name = g_strdup(name);
    look->face = g_strdup(face);
    look->has_avatar = has_avatar;
    g_hash_table_replace(self->looks, g_strdup(id), look);
}

void
clawt_office_scene_set_portrait(ClawtOfficeScene *self, const gchar *id,
                                const guint8 *rgba, gint size, const gchar *etag)
{
    FacePortrait *portrait;

    if (id == NULL)
        return;
    if (rgba == NULL || size < 1) {
        FacePortrait *existing = g_hash_table_lookup(self->portraits, id);

        if (existing == NULL)
            return;
        /*
         * prepare() is the only safe place for a GL call.  A clear from
         * the feed callback queues the texture and drops the pixels; it
         * does not call UnloadTexture.
         */
        if (existing->uploaded && existing->texture.id != 0) {
            Texture2D *texture = g_new(Texture2D, 1);

            *texture = existing->texture;
            existing->uploaded = FALSE;
            existing->texture.id = 0;
            g_queue_push_tail(&self->portrait_unload, texture);
        }
        g_hash_table_remove(self->portraits, id);
        return;
    }
    portrait = g_hash_table_lookup(self->portraits, id);
    if (portrait != NULL && g_strcmp0(portrait->etag, etag) == 0 && portrait->rgba != NULL)
        return;
    if (portrait == NULL) {
        portrait = g_new0(FacePortrait, 1);
        g_hash_table_insert(self->portraits, g_strdup(id), portrait);
    }
    /*
     * The old texture stays until prepare(), which is the only place a
     * GL call is safe: this can run from a feed callback between frames.
     */
    g_free(portrait->etag);
    g_free(portrait->rgba);
    portrait->etag = g_strdup(etag);
    portrait->size = size;
    portrait->rgba = g_memdup2(rgba, (gsize)size * (gsize)size * 4);
    portrait->dirty = TRUE;
}

ClawtOfficeFaceKind
clawt_office_scene_face_kind(ClawtOfficeScene *self, const gchar *id)
{
    Look *look = id != NULL ? g_hash_table_lookup(self->looks, id) : NULL;
    Actor *actor = id != NULL ? g_hash_table_lookup(self->actors, id) : NULL;
    Sheet *sheet = id != NULL ? g_hash_table_lookup(self->sheets, id) : NULL;
    const gchar *color = id != NULL ? g_hash_table_lookup(self->colors, id) : NULL;
    const gchar *name;
    ClawtOfficeFace face;
    gchar color_buf[16];
    gchar initial[8];

    if (color == NULL && actor != NULL)
        color = actor->own_color;
    name = look != NULL && look->name != NULL ? look->name
        : actor != NULL && actor->name != NULL ? actor->name : id;
    clawt_office_face_resolve(&face, id, name, color,
                              look != NULL ? look->face : NULL,
                              look != NULL && look->has_avatar,
                              sheet != NULL && (sheet->path != NULL || sheet->mesh_path != NULL),
                              color_buf, sizeof(color_buf), initial, sizeof(initial));
    return face.kind;
}

void
clawt_office_scene_set_usage(ClawtOfficeScene *self, JsonObject *usage)
{
    g_clear_pointer(&self->usage, json_object_unref);
    if (usage != NULL)
        self->usage = json_object_ref(usage);
    /* worded again at the next frame */
    self->board_built = 0;
}

void
clawt_office_scene_set_usage_placement(ClawtOfficeScene *self,
                                       ClawtOfficeUsagePlacement placement)
{
    if (placement != CLAWT_OFFICE_USAGE_CORNER && placement != CLAWT_OFFICE_USAGE_OFF)
        placement = CLAWT_OFFICE_USAGE_WORLD;
    if (placement == self->usage_placement)
        return;
    self->usage_placement = placement;
    /* a panel pinned open beside the wall is not carried into the corner */
    self->panel_pinned = FALSE;
    self->hover_quiet = FALSE;
}

ClawtOfficeUsagePlacement
clawt_office_scene_get_usage_placement(ClawtOfficeScene *self)
{
    return self->usage_placement;
}

void
clawt_office_scene_set_usage_open(ClawtOfficeScene *self, gboolean open)
{
    self->panel_pinned = open;
    self->hover_quiet = !open && (self->board_hovered || self->panel_hovered);
}

gboolean
clawt_office_scene_get_usage_open(ClawtOfficeScene *self)
{
    if (self->usage_placement == CLAWT_OFFICE_USAGE_OFF)
        return FALSE;
    if (self->usage_placement == CLAWT_OFFICE_USAGE_CORNER)
        return self->usage != NULL;
    if (self->panel_pinned)
        return TRUE;
    /* hovered open: the board, or the panel it opened, under the pointer */
    return !self->hover_quiet &&
           (self->board_hovered || (self->panel_hovered && self->panel_layout != NULL));
}

void
clawt_office_scene_set_clock(ClawtOfficeScene *self, gint64 now)
{
    self->clock_now = now;
    self->board_built = 0;
}

void
clawt_office_scene_set_character(ClawtOfficeScene *self, const gchar *id,
                                 const gchar *dir, JsonObject *character)
{
    Sheet *sheet;
    const gchar *sheet_name;
    const gchar *mesh_name;

    if (id == NULL || dir == NULL || character == NULL)
        return;
    sheet_name = json_object_get_string_member_with_default(character, "sheet", NULL);
    mesh_name = json_object_get_string_member_with_default(character, "mesh", NULL);
    sheet = g_new0(Sheet, 1);
    sheet->walk_anim = -1;
    sheet->path = sheet_name != NULL && json_object_has_member(character, "animations")
        ? g_build_filename(dir, sheet_name, NULL) : NULL;
    sheet->mesh_path = mesh_name != NULL ? g_build_filename(dir, mesh_name, NULL) : NULL;
    sheet->failed = sheet->path == NULL;
    sheet->model_failed = sheet->mesh_path == NULL;
    sheet->layout = json_object_ref(character);
    g_hash_table_replace(self->sheets, g_strdup(id), sheet);
}

void
clawt_office_scene_set_mode(ClawtOfficeScene *self, ClawtOfficeSceneMode mode)
{
    if (mode == CLAWT_OFFICE_SCENE_3D && self->map != NULL && !self->has_3d)
        return;
    self->mode = mode;
}

ClawtOfficeSceneMode
clawt_office_scene_get_mode(ClawtOfficeScene *self)
{
    return self->mode;
}

gboolean
clawt_office_scene_has_3d(ClawtOfficeScene *self)
{
    return self->has_3d;
}

void
clawt_office_scene_set_zoom(ClawtOfficeScene *self, ClawtOfficeZoom zoom)
{
    self->zoom = zoom;
    /*
     * A new zoom starts from the middle (or whoever is followed), not an
     * old drag, and the 3D camera goes back to being driven by Fit.
     * Follow is left as it was: the toolbar's zoom is not a manual move.
     */
    self->panned = FALSE;
    self->manual = FALSE;
    self->zoom_free = FALSE;
    self->frame_valid = FALSE;
    switch (zoom) {
    case CLAWT_OFFICE_ZOOM_1X: self->distance_scale = 0.6f;  break;
    case CLAWT_OFFICE_ZOOM_2X: self->distance_scale = 0.35f; break;
    /* pixel-perfect is a 2D sampling choice; in 3D it frames like Fit */
    case CLAWT_OFFICE_ZOOM_PIXEL:
    case CLAWT_OFFICE_ZOOM_FIT:
    default:                   self->distance_scale = 1.0f;  break;
    }
}

ClawtOfficeZoom
clawt_office_scene_get_zoom(ClawtOfficeScene *self)
{
    return self->zoom;
}

void
clawt_office_scene_set_ui_scale(ClawtOfficeScene *self, gint ui_scale)
{
    self->ui_scale = clawt_office_view_clamp_ui_scale(ui_scale);
}

void
clawt_office_scene_pan(ClawtOfficeScene *self, gfloat dx, gfloat dy)
{
    pan_2d(self, (gfloat)self->last_w, (gfloat)self->last_h, dx, dy);
}

void
clawt_office_scene_set_status(ClawtOfficeScene *self, const gchar *text)
{
    replace(&self->status, text);
}

guint
clawt_office_scene_n_participants(ClawtOfficeScene *self)
{
    return g_hash_table_size(self->actors);
}

void
clawt_office_scene_set_focus(ClawtOfficeScene *self, const gchar *id)
{
    gboolean have = id != NULL && *id != '\0';

    replace(&self->focus, have ? id : NULL);
    /*
     * Following somebody looks at them, not at an old drag.  Clearing
     * the follow (NULL) leaves a pan where the person put it.
     */
    if (have) {
        self->panned = FALSE;
        self->manual = FALSE;
        self->zoom_free = FALSE;
    }
}

void
clawt_office_scene_set_camera(ClawtOfficeScene *self, gfloat x, gfloat z,
                              gfloat yaw, gfloat pitch, gfloat distance)
{
    clawt_office_camera_snap_pose(self->camera, x, z, yaw, pitch, distance);
    self->manual = TRUE;
    self->camera_ready = TRUE;
    replace(&self->focus, NULL);
}

/*
 * One sample from whichever host has the pointer.  The binding table
 * already decided what the keys mean; this decides what a drag means,
 * the same way in the window, the GTK embed and lrg.
 *
 * Returns TRUE when the sample was a double-click on the floor, so the
 * host does not also open a conversation for it.
 */
gboolean
clawt_office_scene_input(ClawtOfficeScene *self, const ClawtOfficeInput *in)
{
    gboolean eat = FALSE;
    gboolean moved = FALSE;
    gboolean three_d;

    g_return_val_if_fail(self != NULL, FALSE);
    if (in == NULL)
        return FALSE;

    three_d = self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d;

    /* F and Home win the frame: Fit, from the bearing the camera has */
    if (in->reset_fit) {
        self->zoom = CLAWT_OFFICE_ZOOM_FIT;
        self->distance_scale = 1.0f;
        self->manual = FALSE;
        self->panned = FALSE;
        self->zoom_free = FALSE;
        self->frame_valid = FALSE;
        replace(&self->focus, NULL);
        clawt_office_camera_set_desired_pitch(self->camera,
                                              clawt_office_camera_pitch_fit());
        return TRUE;
    }

    if (three_d) {
        gfloat forward = 0.0f;
        gfloat strafe = 0.0f;
        gfloat turn = 0.0f;
        gfloat ax = 0.0f;
        gfloat az = 0.0f;
        gboolean anchor = FALSE;

        if (in->move_forward)
            forward += 1.0f;
        if (in->move_back)
            forward -= 1.0f;
        if (in->move_right)
            strafe += 1.0f;
        if (in->move_left)
            strafe -= 1.0f;
        if (forward != 0.0f || strafe != 0.0f) {
            clawt_office_camera_move(self->camera, forward, strafe, in->dt, in->shift);
            moved = TRUE;
        }
        if (in->turn_left)
            turn += 1.0f;
        if (in->turn_right)
            turn -= 1.0f;
        if (turn != 0.0f) {
            clawt_office_camera_turn(self->camera, turn, in->dt, in->shift);
            moved = TRUE;
        }
        if ((in->dx != 0.0f || in->dy != 0.0f) && in->view_h > 0.0f) {
            if (in->middle_down || (in->right_down && in->shift)) {
                clawt_office_camera_pan(self->camera, in->dx, in->dy, in->view_h,
                                        CLAWT_OFFICE_VIEW_3D_FOVY);
                moved = TRUE;
            } else if (in->right_down) {
                clawt_office_camera_look(self->camera, in->dx, in->dy);
                moved = TRUE;
            }
        }
        if (in->wheel != 0.0f) {
            if (in->pointer_valid && in->view_w > 0.0f && in->view_h > 0.0f)
                anchor = clawt_office_camera_floor_at(self->camera, in->x, in->y,
                                                      in->view_w, in->view_h,
                                                      CLAWT_OFFICE_VIEW_3D_FOVY,
                                                      &ax, &az);
            clawt_office_camera_dolly(self->camera, in->wheel, anchor, ax, az);
            moved = TRUE;
        }
        if (in->left_pressed && in->pointer_valid) {
            gfloat since = self->clock - self->click_time;
            gfloat cdx = in->x - self->click_x;
            gfloat cdy = in->y - self->click_y;

            if (clawt_office_camera_double_click(since, cdx, cdy)) {
                const gchar *under = clawt_office_scene_pointer(self, in->x, in->y);

                if (under == NULL && !self->board_hovered && !self->panel_hovered &&
                    in->view_w > 0.0f && in->view_h > 0.0f &&
                    clawt_office_camera_floor_at(self->camera, in->x, in->y,
                                                 in->view_w, in->view_h,
                                                 CLAWT_OFFICE_VIEW_3D_FOVY,
                                                 &ax, &az)) {
                    clawt_office_camera_set_desired_target(self->camera, ax, az);
                    moved = TRUE;
                    eat = TRUE;
                }
            }
            self->click_time = self->clock;
            self->click_x = in->x;
            self->click_y = in->y;
        }
        if (moved)
            self->manual = TRUE;
    } else {
        gfloat view_w = in->view_w > 0.0f ? in->view_w : (gfloat)MAX(self->last_w, 1);
        gfloat view_h = in->view_h > 0.0f ? in->view_h : (gfloat)MAX(self->last_h, 1);
        gfloat speed;
        gfloat dx = 0.0f;
        gfloat dy = 0.0f;

        /* W looks up the map (y down), A looks left.  Shift is three times. */
        speed = CLAWT_OFFICE_CAMERA_PAN_PX_PER_S * in->dt;
        if (in->shift)
            speed *= CLAWT_OFFICE_CAMERA_FAST_SCALE;
        if (in->move_forward)
            dy += speed;
        if (in->move_back)
            dy -= speed;
        if (in->move_left)
            dx += speed;
        if (in->move_right)
            dx -= speed;
        if (dx != 0.0f || dy != 0.0f) {
            pan_2d(self, view_w, view_h, dx, dy);
            moved = TRUE;
        }
        /* a left drag grabs the map; Q and E do nothing from above */
        if (in->left_down && (in->dx != 0.0f || in->dy != 0.0f)) {
            pan_2d(self, view_w, view_h, in->dx, in->dy);
            moved = TRUE;
        }
        if (in->wheel != 0.0f && in->pointer_valid) {
            View2D v = view_2d(self, view_w, view_h);
            gfloat next = clawt_office_camera_zoom_factor(v.zoom, in->wheel);

            clawt_office_camera_zoom_around(&v.cx, &v.cy, v.zoom, next,
                                            in->x, in->y, view_w, view_h);
            clawt_office_view_clamp_centre(view_w, view_h, self->map_w, self->map_h,
                                           next, &v.cx, &v.cy);
            self->free_zoom = next;
            self->zoom_free = TRUE;
            self->centre_x = v.cx;
            self->centre_y = v.cy;
            self->panned = TRUE;
            moved = TRUE;
        }
        if (in->left_pressed && in->pointer_valid) {
            gfloat since = self->clock - self->click_time;
            gfloat cdx = in->x - self->click_x;
            gfloat cdy = in->y - self->click_y;

            if (clawt_office_camera_double_click(since, cdx, cdy)) {
                const gchar *under = clawt_office_scene_pointer(self, in->x, in->y);

                if (under == NULL && !self->board_hovered && !self->panel_hovered) {
                    View2D v = view_2d(self, view_w, view_h);

                    self->centre_x = v.cx + (in->x - view_w * 0.5f) / v.zoom;
                    self->centre_y = v.cy + (in->y - view_h * 0.5f) / v.zoom;
                    self->panned = TRUE;
                    clawt_office_view_clamp_centre(view_w, view_h, self->map_w, self->map_h,
                                                   v.zoom, &self->centre_x, &self->centre_y);
                    moved = TRUE;
                    eat = TRUE;
                }
            }
            self->click_time = self->clock;
            self->click_x = in->x;
            self->click_y = in->y;
        }
    }

    if (moved)
        replace(&self->focus, NULL);
    return eat;
}

/* ── Time ───────────────────────────────────────────────────────── */

void
clawt_office_scene_prepare(ClawtOfficeScene *self)
{
    guint i;

    /*
     * The map is drawn once at 1:1 into a texture and that texture is
     * scaled: tiles drawn at a fractional zoom leave hairline seams where
     * the atlas's neighbouring tiles bleed through.  Never inside a
     * host's own render texture -- raylib texture modes do not nest -- so
     * update() calls it, and a host that draws inside one calls it first.
     */
    if (!IsWindowReady())
        return;

    /* characters' sheets, loaded where a GL context is current */
    {
        GHashTableIter iter;
        gpointer value;

        g_hash_table_iter_init(&iter, self->sheets);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            Sheet *sheet = value;

            if (sheet->loaded || sheet->failed)
                continue;
            sheet->texture = load_sheet_texture(sheet);
            sheet->loaded = sheet->texture.id != 0;
            sheet->failed = !sheet->loaded;
            if (sheet->loaded)
                SetTextureFilter(sheet->texture, TEXTURE_FILTER_POINT);
        }

        /*
         * Portraits, the same place: a texture loaded inside the host's
         * render texture is a nested mode raylib does not support.
         * Bilinear even in a pixel-art world -- the head is a handful of
         * pixels at Fit, and point sampling turns a face into noise.
         * Sheets stay point-sampled above; that is a different picture.
         */
        {
            Texture2D *texture;

            while ((texture = g_queue_pop_head(&self->portrait_unload)) != NULL) {
                if (texture->id != 0)
                    UnloadTexture(*texture);
                g_free(texture);
            }
        }
        g_hash_table_iter_init(&iter, self->portraits);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            FacePortrait *portrait = value;
            Image image = { 0 };

            if (!portrait->dirty || portrait->rgba == NULL)
                continue;
            if (portrait->uploaded)
                UnloadTexture(portrait->texture);
            image.data = portrait->rgba;
            image.width = portrait->size;
            image.height = portrait->size;
            image.mipmaps = 1;
            image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            portrait->texture = LoadTextureFromImage(image);
            portrait->uploaded = portrait->texture.id != 0;
            portrait->dirty = !portrait->uploaded;
            if (portrait->uploaded)
                SetTextureFilter(portrait->texture, TEXTURE_FILTER_BILINEAR);
        }

        /*
         * Its model, scaled to the shared body's height and stood on the
         * floor from its own bounding box -- a mesh made by a generator
         * comes in whatever units and origin it likes.  An animation
         * whose name says walk is the walk; the rest are left alone.
         */
        g_hash_table_iter_init(&iter, self->sheets);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            Sheet *sheet = value;
            BoundingBox box;
            gfloat height;
            gint n;

            if (sheet->model_loaded || sheet->model_failed)
                continue;
            sheet->model = LoadModel(sheet->mesh_path);
            sheet->model_loaded = sheet->model.meshCount > 0;
            sheet->model_failed = !sheet->model_loaded;
            if (!sheet->model_loaded)
                continue;
            match_skeleton_units(&sheet->model);
            box = GetModelBoundingBox(sheet->model);
            height = box.max.y - box.min.y;
            sheet->model_scale = height > 0.0001f ? 2.4f / height : 1.0f;
            sheet->model_floor = box.min.y;
            if (g_str_has_suffix(sheet->mesh_path, ".glb") ||
                g_str_has_suffix(sheet->mesh_path, ".iqm") ||
                g_str_has_suffix(sheet->mesh_path, ".m3d")) {
                sheet->animations = LoadModelAnimations(sheet->mesh_path,
                                                        &sheet->n_animations);
                for (n = 0; sheet->animations != NULL && n < sheet->n_animations; n++) {
                    g_autofree gchar *lower = g_ascii_strdown(sheet->animations[n].name, -1);

                    if (strstr(lower, "walk") != NULL || strstr(lower, "run") != NULL) {
                        sheet->walk_anim = n;
                        break;
                    }
                }
            }
        }
    }

    if (!self->has_2d || self->baked_ok)
        return;
    self->baked = LoadRenderTexture((gint)self->map_w, (gint)self->map_h);
    SetTextureFilter(self->baked.texture, TEXTURE_FILTER_BILINEAR);
    BeginTextureMode(self->baked);
    ClearBackground(BLANK);
    for (i = 0; i < self->tilemaps->len; i++)
        lrg_tilemap_draw(g_ptr_array_index(self->tilemaps, i));
    EndTextureMode();
    self->baked_ok = TRUE;
}

gfloat
clawt_office_scene_units_ratio(gfloat mesh_height, gfloat skeleton_height)
{
    gfloat ratio;

    if (!(mesh_height > 0.0f) || !(skeleton_height > 0.0f))
        return 1.0f;
    ratio = skeleton_height / mesh_height;
    if (ratio > 0.2f && ratio < 5.0f)
        return 1.0f;
    return powf(10.0f, roundf(log10f(ratio)));
}

/*
 * A skinned model whose mesh loaded in other units than its skeleton is
 * brought to the skeleton's (see clawt_office_scene_units_ratio()):
 * every vertex scaled and sent to the GPU again, before anything is
 * measured from it.
 */
static void
match_skeleton_units(Model *model)
{
    BoundingBox box;
    gfloat low = G_MAXFLOAT;
    gfloat high = -G_MAXFLOAT;
    gfloat ratio;
    guint i;
    gint m;

    if (model->skeleton.boneCount < 2 || model->skeleton.bindPose == NULL)
        return;
    for (i = 0; i < model->skeleton.boneCount; i++) {
        low = MIN(low, model->skeleton.bindPose[i].translation.y);
        high = MAX(high, model->skeleton.bindPose[i].translation.y);
    }
    box = GetModelBoundingBox(*model);
    ratio = clawt_office_scene_units_ratio(box.max.y - box.min.y, high - low);
    if (ratio == 1.0f)
        return;

    for (m = 0; m < model->meshCount; m++) {
        Mesh *mesh = &model->meshes[m];
        gint v;

        if (mesh->vertices == NULL)
            continue;
        for (v = 0; v < mesh->vertexCount * 3; v++) {
            mesh->vertices[v] *= ratio;
            if (mesh->animVertices != NULL)
                mesh->animVertices[v] *= ratio;
        }
        UpdateMeshBuffer(*mesh, 0, mesh->vertices,
                         mesh->vertexCount * 3 * (gint)sizeof(gfloat), 0);
    }
}

gboolean
clawt_office_scene_update(ClawtOfficeScene *self, gfloat delta)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    gboolean moving = FALSE;
    g_autoptr(GPtrArray) gone = g_ptr_array_new();
    guint i;

    self->clock += delta;

    clawt_office_scene_prepare(self);

    if (self->adapter != NULL) {
        lrg_world_adapter_update(self->adapter, delta);
        g_hash_table_iter_init(&iter, self->actors);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            Actor *a = value;
            gpointer handle = NULL;
            gfloat z = 0.0f;

            /* where the adapter's path-finding has them, so overlays follow */
            if (g_hash_table_lookup_extended(self->handles, a->id, NULL, &handle))
                lrg_world_adapter_get_actor_position(self->adapter, GPOINTER_TO_UINT(handle),
                                                     &a->x, &a->y, &z);
        }
    }

    g_hash_table_iter_init(&iter, self->actors);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        Actor *a = value;
        gfloat dx = a->tx - a->x;
        gfloat dy = a->ty - a->y;
        gfloat dist = sqrtf(dx * dx + dy * dy);

        if (dist > ARRIVED_PIXELS) {
            gfloat step = MIN(dist, WALK_PIXELS_PER_SECOND * delta);

            a->x += dx / dist * step;
            a->y += dy / dist * step;
            a->walk_phase += delta * 10.0f;
            moving = TRUE;
        } else {
            a->x = a->tx;
            a->y = a->ty;
            a->walk_phase = 0.0f;
            if (a->leaving) {
                a->fade -= delta * 2.0f;
                moving = TRUE;
                if (a->fade <= 0.0f)
                    g_ptr_array_add(gone, key);
            }
        }
        lrg_world_widget_update(LRG_WORLD_WIDGET(a->plate), delta);
        lrg_world_widget_update(LRG_WORLD_WIDGET(a->bubble), delta);
        lrg_world_widget_update(LRG_WORLD_WIDGET(a->mark), delta);
        if (lrg_world_widget_is_shown(LRG_WORLD_WIDGET(a->mark)))
            moving = TRUE;
    }
    for (i = 0; i < gone->len; i++) {
        gpointer handle = NULL;
        const gchar *id = g_ptr_array_index(gone, i);

        if (self->adapter != NULL &&
            g_hash_table_lookup_extended(self->handles, id, NULL, &handle)) {
            lrg_world_adapter_remove_actor(self->adapter, GPOINTER_TO_UINT(handle));
            g_hash_table_remove(self->handles, id);
        }
        g_hash_table_remove(self->actors, id);
    }

    {
        GHashTableIter beams;

        g_hash_table_iter_init(&beams, self->beams);
        while (g_hash_table_iter_next(&beams, NULL, &value)) {
            lrg_beam_update(LRG_BEAM(value), delta);
            moving = TRUE;
        }
    }

    /*
     * The camera eases here, once, after this frame's desired pose is
     * current.  The first update runs before any draw, so last_w is 0
     * and there is nothing to frame yet; the first draw snaps instead.
     * No GL: a test that updates without a window stays on this path.
     */
    if (self->last_w > 0 && self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d) {
        if (!self->manual)
            camera_refresh(self, self->last_w, self->last_h);
        if (clawt_office_camera_step(self->camera, delta))
            moving = TRUE;
    }
    return moving;
}

/* ── Drawing ────────────────────────────────────────────────────── */

/*
 * The calls in the room, as one beam per pair: a call is between two
 * people, and two arcs on top of each other would be one arc drawn twice
 * as thick.  Rebuilt per frame from who is on a call with whom.
 */
static void
sync_beams(ClawtOfficeScene *self, gboolean three_d)
{
    GHashTableIter iter;
    gpointer value;
    g_autoptr(GHashTable) live = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GPtrArray) stale = g_ptr_array_new_with_free_func(g_free);
    gpointer key;
    guint i;

    g_hash_table_iter_init(&iter, self->actors);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Actor *a = value;
        Actor *b;
        g_autofree gchar *pair = NULL;
        LrgBeam *beam;
        gfloat k = three_d ? 1.0f / self->tile : 1.0f;

        if (g_strcmp0(a->activity, "on_call") != 0 || a->facing == NULL)
            continue;
        b = g_hash_table_lookup(self->actors, a->facing);
        if (b == NULL || g_strcmp0(a->id, b->id) >= 0)
            continue;
        pair = g_strdup_printf("%s|%s", a->id, b->id);
        beam = g_hash_table_lookup(self->beams, pair);
        if (beam == NULL) {
            g_autoptr(GrlColor) gold = grl_color_new(255, 196, 64, 230);

            beam = lrg_beam_new();
            lrg_beam_set_color(beam, gold);
            lrg_beam_set_width(beam, three_d ? 0.06f : 4.0f);
            lrg_beam_set_dash(beam, three_d ? 0.4f : 10.0f, three_d ? 0.25f : 6.0f,
                              three_d ? 1.5f : 40.0f);
            g_hash_table_insert(self->beams, g_strdup(pair), beam);
        }
        if (three_d) {
            lrg_beam_set_arc(beam, 1.2f, LRG_BEAM_BEND_AXIS);
            lrg_beam_set_endpoints(beam, a->x * k, 2.2f, a->y * k, b->x * k, 2.2f, b->y * k);
        } else {
            lrg_beam_set_arc(beam, 60.0f, LRG_BEAM_BEND_PERPENDICULAR);
            lrg_beam_set_endpoints(beam, a->x, a->y - 18.0f * self->person_scale, 0.0f, b->x,
                                   b->y - 18.0f * self->person_scale, 0.0f);
        }
        g_hash_table_add(live, g_steal_pointer(&pair));
    }

    g_hash_table_iter_init(&iter, self->beams);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        if (!g_hash_table_contains(live, key))
            g_ptr_array_add(stale, g_strdup(key));
    for (i = 0; i < stale->len; i++)
        g_hash_table_remove(self->beams, g_ptr_array_index(stale, i));
}

/* The uploaded picture, or NULL until prepare() has handed it to GL. */
static FacePortrait *
actor_portrait(ClawtOfficeScene *self, Actor *a)
{
    FacePortrait *portrait;

    if (a == NULL || a->id == NULL)
        return NULL;
    portrait = g_hash_table_lookup(self->portraits, a->id);
    if (portrait == NULL || !portrait->uploaded || portrait->texture.id == 0)
        return NULL;
    return portrait;
}

/* A prop, drawn small beside the body: what the activity has in hand. */
static void
draw_prop_2d(Actor *a, gfloat x, gfloat y)
{
    if (a->prop == NULL)
        return;
    if (g_strcmp0(a->prop, "laptop") == 0 || g_strcmp0(a->prop, "screen") == 0 ||
        g_strcmp0(a->prop, "terminal") == 0) {
        DrawRectangleRec((Rectangle){ x + 6.0f, y - 2.0f, 12.0f, 8.0f }, (Color){ 40, 44, 52, 255 });
        DrawRectangleRec((Rectangle){ x + 7.0f, y - 1.0f, 10.0f, 5.0f }, (Color){ 110, 190, 255, 255 });
    } else if (g_strcmp0(a->prop, "phone") == 0) {
        DrawRectangleRec((Rectangle){ x + 7.0f, y - 14.0f, 4.0f, 7.0f }, (Color){ 30, 30, 36, 255 });
    } else if (g_strcmp0(a->prop, "folder") == 0) {
        DrawRectangleRec((Rectangle){ x + 6.0f, y - 4.0f, 10.0f, 8.0f }, (Color){ 240, 190, 70, 255 });
    } else if (g_strcmp0(a->prop, "book") == 0) {
        DrawRectangleRec((Rectangle){ x + 6.0f, y - 4.0f, 9.0f, 7.0f }, (Color){ 170, 70, 60, 255 });
    } else if (g_strcmp0(a->prop, "marker") == 0) {
        DrawLineEx((Vector2){ x + 7.0f, y - 6.0f }, (Vector2){ x + 13.0f, y - 14.0f }, 2.0f,
                   (Color){ 220, 60, 60, 255 });
    } else if (g_strcmp0(a->prop, "coffee") == 0) {
        DrawRectangleRec((Rectangle){ x + 7.0f, y - 4.0f, 6.0f, 7.0f }, (Color){ 245, 245, 240, 255 });
    }
}

/*
 * A person from above: a shadow, a body in a darker shade of their
 * colour, a head in their colour with their face's eyes.  Seated people
 * are a little lower; walking legs swing, standing ones do not.
 */
static void
draw_person_2d(ClawtOfficeScene *self, Actor *a, gfloat x, gfloat y, gboolean hovered)
{
    gfloat alpha = a->fade;
    gboolean sitting = g_strcmp0(a->pose, "sit") == 0 || g_strcmp0(a->pose, "type") == 0 ||
                       g_strcmp0(a->pose, "read") == 0;
    gfloat lift = sitting ? 0.0f : 3.0f;
    gfloat swing = sinf(a->walk_phase) * 2.5f;
    gfloat hx;
    gfloat hy;
    FacePortrait *picture;
    Color body = with_alpha(shade(a->color, 0.72f), alpha);
    Color head = with_alpha(a->color, alpha);
    Color eye = with_alpha(parse_color(clawt_face_eye_color(clawt_avatar_color(a->id, a->name,
                                                                               NULL)),
                                       (Color){ 20, 20, 30, 255 }), alpha);

    picture = actor_portrait(self, a);
    hx = x;
    hy = y - 15.0f - lift;
    DrawEllipse((gint)x, (gint)(y + 7.0f), 11.0f, 4.0f, with_alpha((Color){ 0, 0, 0, 70 }, alpha));
    if (!sitting) {
        DrawLineEx((Vector2){ x - 3.0f, y + 2.0f }, (Vector2){ x - 3.0f + swing, y + 8.0f }, 3.0f,
                   with_alpha((Color){ 50, 52, 62, 255 }, alpha));
        DrawLineEx((Vector2){ x + 3.0f, y + 2.0f }, (Vector2){ x + 3.0f - swing, y + 8.0f }, 3.0f,
                   with_alpha((Color){ 50, 52, 62, 255 }, alpha));
    }
    DrawRectangleRounded((Rectangle){ x - 7.0f, y - 8.0f - lift, 14.0f, 13.0f }, 0.5f, 6, body);
    if (hovered)
        DrawCircleLines((gint)hx, (gint)hy, BODY_RADIUS + 2.0f,
                        with_alpha((Color){ 255, 255, 255, 220 }, alpha));
    DrawCircleV((Vector2){ hx, hy }, BODY_RADIUS, head);
    /*
     * The token is already a circle (transparent corners), sized to the
     * head this view already draws, so Fit and a pixel-art map keep the
     * same face.  Until the bytes arrive the colour stands in.
     */
    if (picture != NULL) {
        gfloat side = BODY_RADIUS * 2.0f;

        DrawTexturePro(picture->texture,
                       (Rectangle){ 0.0f, 0.0f, (gfloat)picture->size, (gfloat)picture->size },
                       (Rectangle){ hx - BODY_RADIUS, hy - BODY_RADIUS, side, side },
                       (Vector2){ 0.0f, 0.0f }, 0.0f, with_alpha(WHITE, alpha));
    } else if (clawt_office_scene_face_kind(self, a->id) == CLAWT_OFFICE_FACE_BLOB) {
        ClawtOfficeFace face;
        Look *look = g_hash_table_lookup(self->looks, a->id);
        gchar color_buf[16];
        gchar initial[8];
        gint tw;

        clawt_office_face_resolve(&face, a->id, a->name, NULL,
                                  look != NULL ? look->face : NULL, FALSE, FALSE,
                                  color_buf, sizeof(color_buf), initial, sizeof(initial));
        tw = MeasureText(initial, 10);
        DrawText(initial, (gint)(hx - tw / 2.0f), (gint)(hy - 4.0f), 10,
                 with_alpha((Color){ 20, 20, 30, 255 }, alpha));
    } else {
        DrawCircleV((Vector2){ hx - 3.0f, hy - 1.0f }, 1.6f, eye);
        DrawCircleV((Vector2){ hx + 3.0f, hy - 1.0f }, 1.6f, eye);
    }
    draw_prop_2d(a, x, y - lift);

    /* a remote worker or a temporary helper says so */
    if (g_strcmp0(a->kind, "grok_bot") == 0 || g_strcmp0(a->kind, "ephemeral") == 0) {
        const gchar *badge = g_strcmp0(a->kind, "grok_bot") == 0 ? "R" : "H";

        DrawCircleV((Vector2){ x + 8.0f, y - 22.0f - lift }, 5.0f,
                    with_alpha((Color){ 90, 100, 240, 255 }, alpha));
        DrawText(badge, (gint)(x + 6.0f), (gint)(y - 26.0f - lift), 8,
                 with_alpha(RAYWHITE, alpha));
    }
}

/*
 * The row a character draws now: walking picks the direction it is
 * going, otherwise the pose (sit, type, read, phone, present), otherwise
 * idle -- and whatever the sheet lacks falls back to idle, then to its
 * first row, so a partial sheet still draws.
 */
static JsonObject *
pick_animation(JsonObject *animations, Actor *a, gboolean walking, gfloat dx, gfloat dy,
               gboolean *out_asked)
{
    const gchar *want = NULL;

    *out_asked = FALSE;
    if (walking)
        want = fabsf(dx) > fabsf(dy) ? (dx > 0.0f ? "walk_right" : "walk_left")
                                     : (dy > 0.0f ? "walk_down" : "walk_up");
    else if (a->pose != NULL && g_strcmp0(a->pose, "stand") != 0)
        want = a->pose;
    if (want != NULL && json_object_has_member(animations, want)) {
        *out_asked = TRUE;
        return json_object_get_object_member(animations, want);
    }
    if (walking && json_object_has_member(animations, "walk_down")) {
        *out_asked = TRUE;
        return json_object_get_object_member(animations, "walk_down");
    }
    if (json_object_has_member(animations, "idle"))
        return json_object_get_object_member(animations, "idle");
    {
        GList *names = json_object_get_members(animations);
        JsonObject *first = names != NULL
            ? json_object_get_object_member(animations, names->data) : NULL;

        g_list_free(names);
        return first;
    }
}

/*
 * A person drawn from their own sheet, feet at (@x, @y), about as tall
 * as the default body.  A still pose holds its first frame: the sheet
 * animates walking and what a pose names, never an idle bob.
 */
/*
 * Which frame of a character's sheet is drawn now, as a source rectangle
 * and the frame's aspect.  Shared by the 2D sprite and the 3D billboard,
 * so a character moves the same way in both views.
 */
static gboolean
character_frame(ClawtOfficeScene *self, Actor *a, Sheet *sheet, Rectangle *out_source,
                gfloat *out_aspect)
{
    JsonArray *frame = json_object_get_array_member(sheet->layout, "frame");
    JsonObject *animations = json_object_get_object_member(sheet->layout, "animations");
    gboolean walking = fabsf(a->tx - a->x) + fabsf(a->ty - a->y) > ARRIVED_PIXELS;
    gboolean asked = FALSE;
    JsonObject *anim;
    gdouble fw;
    gdouble fh;
    gint64 frames;
    gdouble fps;
    gint64 index = 0;

    if (!sheet->loaded || frame == NULL || json_array_get_length(frame) < 2 ||
        animations == NULL)
        return FALSE;
    fw = (gdouble)json_array_get_int_element(frame, 0);
    fh = (gdouble)json_array_get_int_element(frame, 1);
    anim = pick_animation(animations, a, walking, a->tx - a->x, a->ty - a->y, &asked);
    if (anim == NULL || fw <= 0.0 || fh <= 0.0)
        return FALSE;
    frames = MAX(1, json_object_get_int_member_with_default(anim, "frames", 1));
    fps = json_object_get_double_member_with_default(anim, "fps", 0.0);

    /*
     * Only the row that was asked for moves: a fallback (idle standing in
     * for a pose the sheet lacks) holds its first frame, or a sheet with
     * an animated idle would bob -- the one thing the office never does.
     */
    if (fps > 0.0 && asked && (walking || (a->pose != NULL && g_strcmp0(a->pose, "stand") != 0 &&
                                           g_strcmp0(a->pose, "sit") != 0)))
        index = (gint64)(self->clock * fps) % frames;
    *out_source = (Rectangle){ (gfloat)(index * fw),
                               (gfloat)(json_object_get_int_member_with_default(anim, "row", 0) *
                                        fh),
                               (gfloat)fw, (gfloat)fh };
    if (sheet->trimmed) {
        out_source->x += sheet->trim.x;
        out_source->y += sheet->trim.y;
        out_source->width = sheet->trim.width;
        out_source->height = sheet->trim.height;
    }
    *out_aspect = out_source->width / out_source->height;
    return TRUE;
}

/*
 * A person drawn from their own sheet, feet at (@x, @y), about as tall
 * as the default body.
 */
static gboolean
draw_character_2d(ClawtOfficeScene *self, Actor *a, Sheet *sheet, gfloat x, gfloat y)
{
    Rectangle source;
    gfloat aspect = 1.0f;
    gfloat height = 34.0f;
    gfloat width;

    if (!character_frame(self, a, sheet, &source, &aspect))
        return FALSE;
    width = height * aspect;
    DrawEllipse((gint)x, (gint)(y + 7.0f), width * 0.45f, 4.0f,
                with_alpha((Color){ 0, 0, 0, 70 }, a->fade));
    DrawTexturePro(sheet->texture, source,
                   (Rectangle){ x - width / 2.0f, y + 8.0f - height, width, height },
                   (Vector2){ 0.0f, 0.0f }, 0.0f, with_alpha(WHITE, a->fade));
    return TRUE;
}

/*
 * The same character in the 3D view: its current frame on a billboard
 * that faces the camera, standing on the floor where the body would.
 * A character is a picture until a pack ships a mesh for it, and a
 * picture that turns to face you reads better than a body that is not
 * them.
 */
/*
 * A character's own model, standing where the body would, turned the way
 * it is walking (or holding the last way it walked).  A walk animation
 * plays while it walks; standing, it holds the first frame -- no idle
 * loop, so a still office is still.
 */
static gboolean
draw_model_3d(ClawtOfficeScene *self, Actor *a, Sheet *sheet, Vector3 feet)
{
    gfloat dx = a->tx - a->x;
    gfloat dy = a->ty - a->y;
    gboolean walking = fabsf(dx) + fabsf(dy) > ARRIVED_PIXELS;
    gfloat s = sheet->model_scale;

    if (!sheet->model_loaded)
        return FALSE;
    if (walking)
        a->heading = atan2f(dx, dy) * RAD2DEG;
    if (sheet->walk_anim >= 0) {
        ModelAnimation *walk = &sheet->animations[sheet->walk_anim];
        gint frame = walking ? (gint)(self->clock * 30.0f) % MAX(walk->keyframeCount, 1) : 0;

        UpdateModelAnimation(sheet->model, *walk, (gfloat)frame);
    }
    DrawModelEx(sheet->model,
                (Vector3){ feet.x, feet.y - sheet->model_floor * s, feet.z },
                (Vector3){ 0.0f, 1.0f, 0.0f }, a->heading, (Vector3){ s, s, s },
                with_alpha(WHITE, a->fade));
    return TRUE;
}

static gboolean draw_billboard_3d(ClawtOfficeScene *self, Actor *a, Sheet *sheet,
                                  Vector3 feet);

static gboolean
draw_character_3d(ClawtOfficeScene *self, Actor *a, Sheet *sheet, Vector3 feet)
{
    if (draw_model_3d(self, a, sheet, feet))
        return TRUE;
    return draw_billboard_3d(self, a, sheet, feet);
}

static gboolean
draw_billboard_3d(ClawtOfficeScene *self, Actor *a, Sheet *sheet, Vector3 feet)
{
    Rectangle source;
    gfloat aspect = 1.0f;
    gfloat height = 2.6f;

    if (!character_frame(self, a, sheet, &source, &aspect))
        return FALSE;
    DrawBillboardRec(self->camera3d, sheet->texture, source,
                     (Vector3){ feet.x, feet.y + height / 2.0f, feet.z },
                     (Vector2){ height * aspect, height }, with_alpha(WHITE, a->fade));
    return TRUE;
}

/* The plate's text: a visitor's says which fleet it is from while hovered. */
static void
sync_plate(ClawtOfficeScene *self, Actor *a)
{
    lrg_nameplate_set_hovered(a->plate, g_strcmp0(self->hovered, a->id) == 0);
    /*
     * A visitor's plate is its name, small, as everybody's is; hovered it
     * says which fleet it is from.  Two "Ada · studio" plates at adjacent
     * visitor desks covered each other.  Set only on a change of hover.
     */
    if (a->visiting_from != NULL &&
        a->plate_full != (g_strcmp0(self->hovered, a->id) == 0)) {
        a->plate_full = !a->plate_full;
        if (a->plate_full) {
            g_autofree gchar *full = g_strdup_printf("%s \302\267 %s", a->name,
                                                     a->visiting_from);

            lrg_nameplate_set_text(a->plate, full);
        } else {
            lrg_nameplate_set_text(a->plate, a->name);
        }
    }
}

/*
 * What floats over everybody, at their screen positions (@at, one per
 * person in @people).  @scale is how large a person is drawn, for where
 * things go; the text itself is the device scale's size, so a 4K frame
 * has labels as readable as a 1080p one.
 *
 * Name plates are placed together (clawt_office_labels_arrange()):
 * adjacent desks used to draw their names over each other, "aye-a
 * elephant neyguide".  The hovered and the followed are pinned where they
 * are; everybody else keeps their place if it is free, else stacks a row
 * or two down, else waits for a hover.  Marks and bubbles are drawn after
 * every plate, over them: they are the louder things to say.
 */
static void
draw_overlays(ClawtOfficeScene *self, GPtrArray *people, const Vector2 *at, gfloat scale,
              gint height, gfloat plate_zoom)
{
    gfloat ui = (gfloat)self->ui_scale;
    /*
     * Past 1x the plates grow with the map, so a fractional Fit does not
     * leave the names at the size they had when the map was stuck at 1.
     * At 1x and below they stay at the ui scale: a fitted map that is
     * smaller than the view would otherwise shrink them to dots.  3D
     * passes 0 and keeps the ui scale; its camera is not this zoom.
     */
    gfloat plate = plate_zoom > ui ? plate_zoom : ui;
    gfloat above = 30.0f * scale;
    gfloat below = MAX(12.0f * scale, 10.0f * ui);
    g_autofree ClawtOfficeLabel *labels = g_new0(ClawtOfficeLabel, MAX(people->len, 1));
    guint i;

    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);
        GrlRectangle box = { 0.0f, 0.0f, 0.0f, 0.0f };

        sync_plate(self, a);
        lrg_world_widget_set_scale(LRG_WORLD_WIDGET(a->plate), plate);
        lrg_world_widget_set_scale(LRG_WORLD_WIDGET(a->mark), plate);
        lrg_world_widget_set_scale(LRG_WORLD_WIDGET(a->bubble), plate);
        lrg_world_widget_get_box_at(LRG_WORLD_WIDGET(a->plate), at[i].x, at[i].y + below, &box);
        labels[i].x = box.x;
        labels[i].y = box.y;
        labels[i].width = box.width;
        labels[i].height = box.height;
        labels[i].pinned = g_strcmp0(self->hovered, a->id) == 0 ||
                           g_strcmp0(self->focus, a->id) == 0;
    }
    clawt_office_labels_arrange(labels, people->len, LABEL_GAP * ui, LABEL_STEPS,
                                (gfloat)height);

    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);

        if (labels[i].shown)
            lrg_world_widget_draw_at(LRG_WORLD_WIDGET(a->plate), at[i].x,
                                     at[i].y + below + labels[i].dy);
    }
    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);

        if (lrg_world_widget_is_shown(LRG_WORLD_WIDGET(a->mark)) ||
            lrg_world_widget_get_alpha(LRG_WORLD_WIDGET(a->mark)) > 0.01f)
            lrg_world_widget_draw_at(LRG_WORLD_WIDGET(a->mark), at[i].x, at[i].y - above);
        if (lrg_world_widget_get_alpha(LRG_WORLD_WIDGET(a->bubble)) > 0.01f)
            lrg_world_widget_draw_at(LRG_WORLD_WIDGET(a->bubble), at[i].x,
                                     at[i].y - above - 14.0f * scale);
    }
}

/*
 * Text for raylib's default font, which has ASCII and nothing else: the
 * dashes and quotes libclawt's labels use become their ASCII cousins,
 * anything else a '?', rather than a box per character.
 */
static gchar *
ascii_for_font(const gchar *text)
{
    GString *out = g_string_new(NULL);
    const gchar *p;

    for (p = text != NULL ? text : ""; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);

        if (c < 0x80)
            g_string_append_c(out, (gchar)c);
        else if (c == 0x2014 || c == 0x2013 || c == 0x2212)
            g_string_append_c(out, '-');
        else if (c == 0x2018 || c == 0x2019)
            g_string_append_c(out, '\'');
        else if (c == 0x201c || c == 0x201d)
            g_string_append_c(out, '"');
        else if (c == 0x00b7 || c == 0x2022)
            g_string_append_c(out, '.');
        else if (c == 0x2026)
            g_string_append(out, "...");
        else
            g_string_append_c(out, '?');
    }
    return g_string_free(out, FALSE);
}

/* A tone's colour, as the clients paint clawt_allowance_level_tone(). */
static Color
tone_color(const gchar *tone)
{
    if (g_strcmp0(tone, "bad") == 0)
        return (Color){ 224, 27, 36, 255 };
    if (g_strcmp0(tone, "warn") == 0)
        return (Color){ 229, 165, 10, 255 };
    if (g_strcmp0(tone, "good") == 0)
        return (Color){ 46, 194, 126, 255 };
    return (Color){ 120, 174, 237, 255 };
}

/* ── The usage board ────────────────────────────────────────────── */

/*
 * The board's text width, for the layout in libclawt: raylib's default
 * font, after the same ASCII folding the text is drawn with, so what is
 * measured is what is drawn.
 */
static gfloat
measure_for_font(const gchar *text, gfloat size, gpointer user_data)
{
    g_autofree gchar *ascii = ascii_for_font(text);

    (void)user_data;
    return (gfloat)MeasureText(ascii, (gint)size);
}

static Color
ink_color(ClawtOfficeBoardInk ink)
{
    switch (ink) {
    case CLAWT_OFFICE_BOARD_INK_TITLE:    return RAYWHITE;
    case CLAWT_OFFICE_BOARD_INK_LABEL:    return (Color){ 190, 195, 210, 255 };
    case CLAWT_OFFICE_BOARD_INK_VALUE:    return RAYWHITE;
    case CLAWT_OFFICE_BOARD_INK_ON_BADGE: return (Color){ 18, 20, 26, 255 };
    case CLAWT_OFFICE_BOARD_INK_MUTED:
    default:                              return (Color){ 150, 156, 172, 255 };
    }
}

/*
 * A layout from libclawt, item by item.  The layout has already fitted
 * everything -- zone, view, columns -- so this only paints.
 */
static void
draw_board_layout(ClawtOfficeBoardLayout *layout, gfloat ui, guchar backing_alpha)
{
    guint i;

    for (i = 0; layout != NULL && i < layout->items->len; i++) {
        ClawtOfficeBoardItem *item = &g_array_index(layout->items, ClawtOfficeBoardItem, i);
        Rectangle box = { item->x, item->y, item->width, item->height };

        switch (item->kind) {
        case CLAWT_OFFICE_BOARD_ITEM_BACKING:
            DrawRectangleRec(box, (Color){ 18, 20, 26, backing_alpha });
            DrawRectangleLinesEx(box, MAX(1.0f, ui), (Color){ 70, 76, 92, 255 });
            break;
        case CLAWT_OFFICE_BOARD_ITEM_TRACK:
            DrawRectangleRec(box, (Color){ 60, 64, 76, 255 });
            break;
        case CLAWT_OFFICE_BOARD_ITEM_FILL:
        case CLAWT_OFFICE_BOARD_ITEM_BADGE:
            DrawRectangleRec(box, tone_color(item->tone));
            break;
        case CLAWT_OFFICE_BOARD_ITEM_TEXT: {
            g_autofree gchar *ascii = ascii_for_font(item->text);

            DrawText(ascii, (gint)item->x, (gint)item->y, (gint)item->size,
                     ink_color(item->ink));
            break;
        }
        default:
            break;
        }
    }
}

/* usage.providers, worded by libclawt -- again whenever the clock moves on a minute */
static ClawtOfficeBoard *
board_model(ClawtOfficeScene *self)
{
    gint64 now = self->clock_now > 0 ? self->clock_now : g_get_real_time() / G_USEC_PER_SEC;

    if (self->board == NULL || self->board_built == 0 || now / 60 != self->board_built / 60) {
        g_clear_pointer(&self->board, clawt_office_board_free);
        self->board = clawt_office_board_new(self->usage, now);
        self->board_built = MAX(now, 1);
    }
    return self->board;
}

/*
 * The board on the wall, fitted to its zone (@zone, screen pixels) by
 * clawt_office_board_layout_world(): words where they fit, a gauge per
 * login where they do not, one mark when even that is too much.  Drawn
 * after the floor and before the people, so they stand in front of it.
 */
static void
draw_world_board(ClawtOfficeScene *self, Rectangle zone)
{
    g_clear_pointer(&self->board_layout, clawt_office_board_layout_free);
    if (!self->has_board || self->usage == NULL ||
        self->usage_placement != CLAWT_OFFICE_USAGE_WORLD)
        return;
    self->board_layout = clawt_office_board_layout_world(board_model(self), zone.x, zone.y,
                                                         zone.width, zone.height,
                                                         self->ui_scale, measure_for_font,
                                                         NULL);
    /* a little of the wall shows through: it is a screen on it */
    draw_board_layout(self->board_layout, (gfloat)self->ui_scale, 232);
}

/* The 2D map's usage_board zone, on screen. */
static Rectangle
board_zone_2d(ClawtOfficeScene *self)
{
    Vector2 a = GetWorldToScreen2D((Vector2){ self->board_rect.x, self->board_rect.y },
                                   self->camera2d);
    Vector2 b = GetWorldToScreen2D((Vector2){ self->board_rect.x + self->board_rect.width,
                                              self->board_rect.y + self->board_rect.height },
                                   self->camera2d);

    return (Rectangle){ a.x, a.y, b.x - a.x, b.y - a.y };
}

/*
 * The 3D board's corners for a camera looking along (@forward_x,
 * @forward_z): a screen standing on the zone's 3D anchor, as wide as the
 * zone, square to the camera.  The same corners the default framing
 * keeps in view; see place_camera_3d().
 */
static gboolean
board_corners_3d(ClawtOfficeScene *self, gfloat forward_x, gfloat forward_z,
                 gfloat corners[12])
{
    gfloat w = self->board_rect.width / self->tile;
    gfloat h = w * self->board_rect.height / MAX(self->board_rect.width, 1.0f);
    gfloat anchor[3];

    anchor[0] = self->board_3d.x;
    anchor[1] = self->board_3d.y;
    anchor[2] = self->board_3d.z;
    return clawt_office_view_board_corners_3d(anchor, w, h, forward_x, forward_z, corners);
}

/*
 * The 3D board on screen: the rectangle its corners cover, projected by
 * raylib with the camera this frame was drawn with.  FALSE when any
 * corner is behind the camera.
 */
static gboolean
board_zone_3d(ClawtOfficeScene *self, gint width, gint height, Rectangle *out)
{
    Vector3 eye = self->camera3d.position;
    Vector3 forward = { self->camera3d.target.x - eye.x, self->camera3d.target.y - eye.y,
                        self->camera3d.target.z - eye.z };
    gfloat corners[12];
    gfloat lo_x = G_MAXFLOAT;
    gfloat lo_y = G_MAXFLOAT;
    gfloat hi_x = -G_MAXFLOAT;
    gfloat hi_y = -G_MAXFLOAT;
    gint i;

    if (!board_corners_3d(self, forward.x, forward.z, corners))
        return FALSE;

    for (i = 0; i < 4; i++) {
        Vector3 corner = { corners[i * 3 + 0], corners[i * 3 + 1], corners[i * 3 + 2] };
        Vector2 at;

        if ((corner.x - eye.x) * forward.x + (corner.y - eye.y) * forward.y +
            (corner.z - eye.z) * forward.z <= 0.0f)
            return FALSE;
        at = GetWorldToScreenEx(corner, self->camera3d, width, height);
        lo_x = MIN(lo_x, at.x);
        lo_y = MIN(lo_y, at.y);
        hi_x = MAX(hi_x, at.x);
        hi_y = MAX(hi_y, at.y);
    }
    *out = (Rectangle){ lo_x, lo_y, hi_x - lo_x, hi_y - lo_y };
    return out->width >= 1.0f && out->height >= 1.0f;
}

/*
 * The details panel, over everything, when it is open: beside the board
 * on the wall, or docked in the corner.  Laid out by
 * clawt_office_board_layout_panel(), which keeps it inside the view.
 */
static void
draw_panel(ClawtOfficeScene *self, gint width, gint height)
{
    ClawtOfficeBoardLayout *board = self->board_layout;
    gboolean docked = self->usage_placement == CLAWT_OFFICE_USAGE_CORNER;
    gboolean open = clawt_office_scene_get_usage_open(self);

    g_clear_pointer(&self->panel_layout, clawt_office_board_layout_free);
    if (self->usage == NULL || !open)
        return;
    /* beside the wall needs the wall drawn this frame */
    if (!docked && (board == NULL || board->detail == CLAWT_OFFICE_BOARD_DETAIL_NONE))
        return;
    self->panel_layout = clawt_office_board_layout_panel(
        board_model(self), docked,
        board != NULL ? board->x : 0.0f, board != NULL ? board->y : 0.0f,
        board != NULL ? board->width : 0.0f, board != NULL ? board->height : 0.0f,
        (gfloat)width, (gfloat)height, self->ui_scale, measure_for_font, NULL);
    /* opaque: when it has to sit over the board, the board must not show through */
    draw_board_layout(self->panel_layout, (gfloat)self->ui_scale, 255);
}

/* People drawn top to bottom, so a nearer one stands in front. */
static gint
by_y(gconstpointer left, gconstpointer right)
{
    const Actor *a = *(Actor *const *)left;
    const Actor *b = *(Actor *const *)right;

    return (a->y > b->y) - (a->y < b->y);
}

static GPtrArray *
sorted_actors(ClawtOfficeScene *self)
{
    GPtrArray *out = g_ptr_array_new();
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, self->actors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (((Actor *)value)->placed)
            g_ptr_array_add(out, value);
    g_ptr_array_sort(out, by_y);
    return out;
}

/*
 * Where the 2D camera looks, so the map does not sit a fraction of a
 * screen pixel off.
 *
 * A whole zoom repeats map pixels.  The target stays on a map pixel, and
 * each repeat starts on a screen pixel.  A fractional zoom cannot land
 * every map pixel on a screen pixel; landing the target on one (target
 * times zoom is a whole number) keeps the whole map from crawling, which
 * is the shimmer a floored Fit used to avoid by never drawing that zoom.
 * The offset is already a whole pixel.
 */
static Vector2
snap_camera_target(gfloat cx, gfloat cy, gfloat zoom)
{
    if (!(zoom > 0.0f))
        zoom = 1.0f;
    if (zoom >= 1.0f && zoom == floorf(zoom))
        return (Vector2){ floorf(cx), floorf(cy) };
    return (Vector2){ roundf(cx * zoom) / zoom, roundf(cy * zoom) / zoom };
}

/*
 * The 2D view for this size: Fit (or a free wheel zoom), centred on the
 * map, on whoever is followed, or where a drag or W/A/S/D left it.
 * Input uses this rather than camera2d, which the first frame has not
 * filled in yet.
 */
static View2D
view_2d(ClawtOfficeScene *self, gfloat width, gfloat height)
{
    View2D v;
    Actor *followed = NULL;
    gfloat zoom;

    if (self->zoom_free)
        zoom = self->free_zoom;
    else
        zoom = clawt_office_view_map_zoom(self->zoom, self->ui_scale, width, height,
                                          self->map_w, self->map_h);
    if (!(zoom > 0.0f))
        zoom = 1.0f;
    v.zoom = zoom;
    v.cx = self->map_w / 2.0f;
    v.cy = self->map_h / 2.0f;
    if (self->focus != NULL)
        followed = g_hash_table_lookup(self->actors, self->focus);
    if (followed != NULL && followed->placed && !self->panned) {
        v.cx = followed->x;
        v.cy = followed->y;
    } else if (self->panned) {
        v.cx = self->centre_x;
        v.cy = self->centre_y;
    }
    clawt_office_view_clamp_centre(width, height, self->map_w, self->map_h, zoom,
                                   &v.cx, &v.cy);
    return v;
}

/* The map moves with the pointer: its centre the other way, in map pixels. */
static void
pan_2d(ClawtOfficeScene *self, gfloat view_w, gfloat view_h, gfloat dx, gfloat dy)
{
    View2D v;

    if (self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d)
        return;
    if (!(view_w > 0.0f))
        view_w = (gfloat)MAX(self->last_w, 1);
    if (!(view_h > 0.0f))
        view_h = (gfloat)MAX(self->last_h, 1);
    v = view_2d(self, view_w, view_h);
    self->centre_x = v.cx - dx / v.zoom;
    self->centre_y = v.cy - dy / v.zoom;
    self->panned = TRUE;
    clawt_office_view_clamp_centre(view_w, view_h, self->map_w, self->map_h, v.zoom,
                                   &self->centre_x, &self->centre_y);
}

static void
draw_2d(ClawtOfficeScene *self, gint width, gint height)
{
    g_autoptr(GPtrArray) people = sorted_actors(self);
    g_autofree Vector2 *at = NULL;
    /*
     * From this frame's size, not a cached scale.  The GTK embed draws
     * at the size of the latest resize message, the window at the
     * render size, and lrg at the viewport's size, so Fit follows every
     * resize in all three.
     */
    View2D v = view_2d(self, (gfloat)width, (gfloat)height);
    gfloat zoom = v.zoom;
    gfloat cx = v.cx;
    gfloat cy = v.cy;
    guint i;
    self->camera2d.offset = (Vector2){ floorf(width / 2.0f), floorf(height / 2.0f) };
    self->camera2d.target = snap_camera_target(cx, cy, zoom);
    self->camera2d.rotation = 0.0f;
    self->camera2d.zoom = zoom;
    /* people stay legible in a small view: never under ~a 16 logical px head on screen */
    self->person_scale = CLAMP(MAX(PERSON_SCALE, 0.95f * self->ui_scale / MAX(zoom, 0.01f)),
                               PERSON_SCALE, 4.0f * self->ui_scale);

    sync_beams(self, FALSE);
    BeginMode2D(self->camera2d);
    if (self->baked_ok)
        DrawTexturePro(self->baked.texture,
                       (Rectangle){ 0.0f, 0.0f, self->map_w, -self->map_h },
                       (Rectangle){ 0.0f, 0.0f, self->map_w, self->map_h },
                       (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
    else
        for (i = 0; i < self->tilemaps->len; i++)
            lrg_tilemap_draw(g_ptr_array_index(self->tilemaps, i));
    EndMode2D();

    /*
     * The usage board, on its wall: in screen space so its text is the
     * pixel font at a whole size, but between the floor and the people,
     * so somebody standing at it is drawn in front of it.
     */
    if (self->has_board)
        draw_world_board(self, board_zone_2d(self));

    BeginMode2D(self->camera2d);
    {
        GHashTableIter iter;
        gpointer beam;

        g_hash_table_iter_init(&iter, self->beams);
        while (g_hash_table_iter_next(&iter, NULL, &beam))
            lrg_beam_draw_2d(LRG_BEAM(beam));
    }
    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);

        /* people a little larger than a tile's worth, so a fitted map still shows faces */
        rlPushMatrix();
        rlTranslatef(a->x, a->y, 0.0f);
        rlScalef(self->person_scale, self->person_scale, 1.0f);
        {
            Sheet *sheet = g_hash_table_lookup(self->sheets, a->id);

            if (sheet == NULL || !draw_character_2d(self, a, sheet, 0.0f, 0.0f))
                draw_person_2d(self, a, 0.0f, 0.0f, g_strcmp0(self->hovered, a->id) == 0);
        }
        rlPopMatrix();
    }
    EndMode2D();

    /* text in screen space, so it stays legible at any zoom */
    at = g_new0(Vector2, MAX(people->len, 1));
    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);

        at[i] = GetWorldToScreen2D((Vector2){ a->x, a->y }, self->camera2d);
    }
    draw_overlays(self, people, at, self->person_scale * zoom, height, zoom);
}

/*
 * How high above the floor the default 3D framing keeps in view: the
 * top of a standing figure's head, where its name label is placed.
 */
#define FRAME_HEAD_HEIGHT (2.6f)

/*
 * The whole office at this bearing, for the 3D view's Fit: the floor's
 * corners, the same at head height, and the usage board's corners.
 * Remembered for the bearing and the view's shape it was worked out for,
 * since neither moves on most frames.
 */
static void
frame_office_3d(ClawtOfficeScene *self, gfloat yaw, gfloat aspect, gfloat *cx, gfloat *cz,
                gfloat *reach)
{
    gfloat points[3 * 12];
    guint n = 0;
    gint i;

    if (self->frame_valid && self->frame_yaw == yaw && self->frame_aspect == aspect) {
        *cx = self->frame_x;
        *cz = self->frame_z;
        *reach = self->frame_reach;
        return;
    }

    for (i = 0; i < 8; i++) {
        points[n * 3 + 0] = (i & 1) ? self->map_w / self->tile : 0.0f;
        points[n * 3 + 1] = (i & 4) ? FRAME_HEAD_HEIGHT : 0.0f;
        points[n * 3 + 2] = (i & 2) ? self->map_h / self->tile : 0.0f;
        n++;
    }

    /*
     * The board as it will be drawn: square to a camera at this bearing,
     * which looks along -(cos, sin) across the floor whatever its distance.
     */
    if (self->has_board &&
        board_corners_3d(self, -cosf(yaw), -sinf(yaw), points + n * 3))
        n += 4;

    if (!clawt_office_view_frame_3d(points, n, yaw, CLAWT_OFFICE_VIEW_3D_LIFT,
                                    CLAWT_OFFICE_VIEW_3D_FOVY, aspect,
                                    CLAWT_OFFICE_VIEW_3D_MARGIN, cx, cz, reach)) {
        /* nothing to frame: the middle of the map, as it always was */
        *cx = self->map_w / self->tile / 2.0f;
        *cz = self->map_h / self->tile / 2.0f;
        *reach = MAX(*cx, *cz) * 1.2f;
    }

    self->frame_valid = TRUE;
    self->frame_yaw = yaw;
    self->frame_aspect = aspect;
    self->frame_x = *cx;
    self->frame_z = *cz;
    self->frame_reach = *reach;
}

/* reach across the floor, times the slant, times the zoom's scale. */
static gfloat
fit_distance(gfloat reach, gfloat scale)
{
    return reach * sqrtf(1.0f + CLAWT_OFFICE_VIEW_3D_LIFT * CLAWT_OFFICE_VIEW_3D_LIFT) * scale;
}

/*
 * While nobody has taken the camera, Fit (or follow) writes the desired
 * pose.  The yaw is the one already wanted, so a resize re-frames without
 * spinning back to the home bearing.  The spring is stepped in update(),
 * not here, so a draw cannot move the camera twice.
 */
static void
camera_refresh(ClawtOfficeScene *self, gint width, gint height)
{
    Actor *followed = NULL;
    gfloat aspect;
    gfloat cx;
    gfloat cz;
    gfloat reach;
    gfloat yaw;

    if (self->manual || width <= 0 || height <= 0)
        return;
    if (self->mode != CLAWT_OFFICE_SCENE_3D || !self->has_3d)
        return;
    if (self->focus != NULL)
        followed = g_hash_table_lookup(self->actors, self->focus);
    if (followed != NULL && followed->placed) {
        clawt_office_camera_set_desired_target(self->camera, followed->x / self->tile,
                                               followed->y / self->tile);
        clawt_office_camera_set_desired_pitch(self->camera, clawt_office_camera_pitch_fit());
        clawt_office_camera_set_desired_distance(self->camera,
                                                 fit_distance(7.0f, self->distance_scale));
        return;
    }
    aspect = (gfloat)width / (gfloat)MAX(height, 1);
    yaw = clawt_office_camera_get_desired_yaw(self->camera);
    frame_office_3d(self, yaw, aspect, &cx, &cz, &reach);
    clawt_office_camera_set_desired_target(self->camera, cx, cz);
    clawt_office_camera_set_desired_pitch(self->camera, clawt_office_camera_pitch_fit());
    clawt_office_camera_set_desired_distance(self->camera,
                                             fit_distance(reach, self->distance_scale));
}

/*
 * The god's-eye camera: a point on the floor, a yaw, a pitch and a
 * distance.  At Fit the distance is the framed horizontal reach times
 * the slant, so the eye sits where the old fixed-pitch camera did.
 * 1x, 2x and the wheel change that distance.  A manual move stops Fit
 * and follow from writing over it.
 */
static void
place_camera_3d(ClawtOfficeScene *self, gint width, gint height)
{
    gfloat aspect = (gfloat)width / (gfloat)MAX(height, 1);
    gfloat ex = 0.0f;
    gfloat ey = 0.0f;
    gfloat ez = 0.0f;
    gfloat tx = 0.0f;
    gfloat tz = 0.0f;
    Vector3 eye;

    if (!self->manual)
        camera_refresh(self, width, height);
    /* the first draw has no eased history: show the framed pose at once */
    if (!self->camera_ready) {
        clawt_office_camera_snap(self->camera);
        self->camera_ready = TRUE;
    }
    clawt_office_camera_eye(self->camera, &ex, &ey, &ez);
    clawt_office_camera_target(self->camera, &tx, &tz);
    eye = (Vector3){ ex, ey, ez };

    self->camera3d.position = eye;
    self->camera3d.target = (Vector3){ tx, 0.0f, tz };
    self->camera3d.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    self->camera3d.fovy = CLAWT_OFFICE_VIEW_3D_FOVY;
    self->camera3d.projection = CAMERA_PERSPECTIVE;
    lrg_camera3d_set_position_xyz(self->lcamera, ex, ey, ez);
    lrg_camera3d_set_target_xyz(self->lcamera, tx, 0.0f, tz);
    lrg_camera3d_set_up_xyz(self->lcamera, 0.0f, 1.0f, 0.0f);
    lrg_camera3d_set_fovy(self->lcamera, CLAWT_OFFICE_VIEW_3D_FOVY);
    lrg_camera3d_set_aspect(self->lcamera, aspect);
}

static void
draw_3d(ClawtOfficeScene *self, gint width, gint height)
{
    g_autoptr(GPtrArray) people = sorted_actors(self);
    g_autofree Vector2 *at = NULL;
    gfloat k = 1.0f / self->tile;
    guint i;

    place_camera_3d(self, width, height);
    lrg_world3d_renderer_render(self->renderer, self->world3d, self->lcamera, NULL,
                                width, height);

    /* the board, as a screen standing on its anchor, before the people */
    if (self->has_board) {
        Rectangle zone;

        if (board_zone_3d(self, width, height, &zone))
            draw_world_board(self, zone);
        else
            g_clear_pointer(&self->board_layout, clawt_office_board_layout_free);
    }

    sync_beams(self, TRUE);
    BeginMode3D(self->camera3d);
    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);
        gboolean sitting = g_strcmp0(a->pose, "sit") == 0 || g_strcmp0(a->pose, "type") == 0 ||
                           g_strcmp0(a->pose, "read") == 0;
        gfloat base = sitting ? 0.2f : 0.0f;
        Vector3 feet = { a->x * k, base, a->y * k };
        Color body = with_alpha(shade(a->color, 0.72f), a->fade);
        Color head = with_alpha(a->color, a->fade);

        /* their own character when they have one, facing the camera */
        {
            Sheet *sheet = g_hash_table_lookup(self->sheets, a->id);

            if (sheet != NULL && draw_character_3d(self, a, sheet, feet))
                continue;
        }
        /* a figure about two tiles tall, so it reads from a god's-eye camera */
        DrawCapsule((Vector3){ feet.x, base + 0.45f, feet.z },
                    (Vector3){ feet.x, base + 1.25f, feet.z }, 0.42f, 10, 8, body);
        {
            FacePortrait *picture = actor_portrait(self, a);

            /*
             * A disc facing the camera, where the sphere was.  The body
             * stays the capsule; the name, the hover ring and the click
             * still use the figure, not the texture.
             */
            if (picture != NULL)
                DrawBillboard(self->camera3d, picture->texture,
                              (Vector3){ feet.x, base + 2.0f, feet.z }, 1.0f,
                              with_alpha(WHITE, a->fade));
            else {
                gfloat yaw = clawt_office_camera_get_yaw(self->camera);

                DrawSphere((Vector3){ feet.x, base + 2.0f, feet.z }, 0.5f, head);
                DrawSphere((Vector3){ feet.x + 0.2f * cosf(yaw + G_PI),
                                      base + 2.08f,
                                      feet.z + 0.2f * sinf(yaw + G_PI) }, 0.08f,
                           with_alpha((Color){ 20, 20, 30, 255 }, a->fade));
            }
        }
        if (g_strcmp0(self->hovered, a->id) == 0)
            DrawCircle3D((Vector3){ feet.x, 0.02f, feet.z }, 0.45f,
                         (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, RAYWHITE);
    }
    {
        GHashTableIter iter;
        gpointer beam;

        g_hash_table_iter_init(&iter, self->beams);
        while (g_hash_table_iter_next(&iter, NULL, &beam))
            lrg_beam_draw_3d(LRG_BEAM(beam));
    }
    EndMode3D();

    at = g_new0(Vector2, MAX(people->len, 1));
    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);

        at[i] = GetWorldToScreenEx((Vector3){ a->x * k, 2.6f, a->y * k },
                                   self->camera3d, width, height);
    }
    draw_overlays(self, people, at, (gfloat)self->ui_scale, height, 0.0f);
}

/*
 * A world adapter draws the world and its actors; the scene adds what
 * floats over them where it can place it.  The Tiled adapter's camera
 * says where an actor is on screen; an adapter with no camera of this
 * kind (a game's own world) draws its own people and nothing is floated
 * over them -- the roster says the same.
 */
static void
draw_adapter(ClawtOfficeScene *self, gint width, gint height)
{
    g_autoptr(GPtrArray) people = NULL;
    g_autofree Vector2 *at = NULL;
    LrgCamera2D *camera;
    guint i;

    if (LRG_IS_TILED_WORLD(self->adapter))
        lrg_tiled_world_set_viewport(LRG_TILED_WORLD(self->adapter), width, height);
    lrg_world_adapter_draw(self->adapter);
    if (!LRG_IS_TILED_WORLD(self->adapter))
        return;

    camera = lrg_tiled_world_get_camera(LRG_TILED_WORLD(self->adapter));

    /*
     * The adapter draws its people with its world, so the board can only
     * go over both; it still never leaves its zone, and the name labels
     * are drawn over it.
     */
    if (self->has_board) {
        GrlVector3 a = { self->board_rect.x, self->board_rect.y, 0.0f };
        GrlVector3 b = { self->board_rect.x + self->board_rect.width,
                         self->board_rect.y + self->board_rect.height, 0.0f };
        GrlVector2 sa = { 0.0f, 0.0f };
        GrlVector2 sb = { 0.0f, 0.0f };

        lrg_camera_world_to_screen(LRG_CAMERA(camera), &a, &sa);
        lrg_camera_world_to_screen(LRG_CAMERA(camera), &b, &sb);
        draw_world_board(self, (Rectangle){ sa.x, sa.y, sb.x - sa.x, sb.y - sa.y });
    }

    people = sorted_actors(self);
    at = g_new0(Vector2, MAX(people->len, 1));
    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);
        GrlVector3 world = { a->x, a->y, 0.0f };
        GrlVector2 screen = { 0.0f, 0.0f };

        lrg_camera_world_to_screen(LRG_CAMERA(camera), &world, &screen);
        at[i] = (Vector2){ screen.x, screen.y };
    }
    draw_overlays(self, people, at,
                  MAX(lrg_camera2d_get_zoom(camera) * PERSON_SCALE, 0.8f * self->ui_scale),
                  height, 0.0f);
}

void
clawt_office_scene_draw(ClawtOfficeScene *self, gint width, gint height)
{
    self->last_w = width;
    self->last_h = height;
    ClearBackground((Color){ 24, 26, 32, 255 });

    /* the wall board is laid out afresh by whichever view draws it */
    g_clear_pointer(&self->board_layout, clawt_office_board_layout_free);
    if (self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d)
        draw_3d(self, width, height);
    else if (self->adapter != NULL)
        draw_adapter(self, width, height);
    else if (self->has_2d)
        draw_2d(self, width, height);

    if (self->status != NULL) {
        gint ui = self->ui_scale;
        gint size = 16 * ui;
        gint text_w = MeasureText(self->status, size);

        DrawRectangleRounded((Rectangle){ 10.0f * ui, 10.0f * ui, (gfloat)text_w + 20.0f * ui,
                                          28.0f * ui }, 0.4f, 6, (Color){ 0, 0, 0, 150 });
        DrawText(self->status, 20 * ui, 16 * ui, size, RAYWHITE);
    }

    /* the details panel last: over the people, their labels and the status line */
    draw_panel(self, width, height);
}

/* Whether @x, @y is on what @layout drew. */
static gboolean
on_layout(ClawtOfficeBoardLayout *layout, gfloat x, gfloat y)
{
    return layout != NULL && layout->detail != CLAWT_OFFICE_BOARD_DETAIL_NONE &&
           x >= layout->x && y >= layout->y && x < layout->x + layout->width &&
           y < layout->y + layout->height;
}

const gchar *
clawt_office_scene_pointer(ClawtOfficeScene *self, gfloat x, gfloat y)
{
    GHashTableIter iter;
    gpointer value;
    Actor *best = NULL;
    gfloat best_d = 0.0f;

    /*
     * The board and its panel first, against what was drawn last frame.
     * A panel closed with Escape while the pointer is on the board stays
     * closed until the pointer has left it, or it would open again at
     * once.  Nobody is hovered through the panel.
     */
    self->board_hovered = on_layout(self->board_layout, x, y);
    self->panel_hovered = on_layout(self->panel_layout, x, y);
    if (!self->board_hovered && !self->panel_hovered)
        self->hover_quiet = FALSE;
    if (self->panel_hovered) {
        replace(&self->hovered, NULL);
        return NULL;
    }

    g_hash_table_iter_init(&iter, self->actors);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Actor *a = value;
        Vector2 at;
        gfloat d;
        gfloat reach;

        if (!a->placed || a->leaving)
            continue;
        if (self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d) {
            at = GetWorldToScreenEx((Vector3){ a->x / self->tile, 0.8f, a->y / self->tile },
                                    self->camera3d, MAX(self->last_w, 1), MAX(self->last_h, 1));
            reach = 22.0f * self->ui_scale;
        } else {
            at = GetWorldToScreen2D((Vector2){ a->x, a->y - 10.0f * self->person_scale },
                                    self->camera2d);
            reach = 16.0f * self->person_scale * self->camera2d.zoom;
        }
        d = sqrtf((at.x - x) * (at.x - x) + (at.y - y) * (at.y - y));
        if (d <= reach && (best == NULL || d < best_d)) {
            best = a;
            best_d = d;
        }
    }
    replace(&self->hovered, best != NULL ? best->id : NULL);
    return self->hovered;
}

gboolean
clawt_office_scene_click(ClawtOfficeScene *self, gfloat x, gfloat y)
{
    gboolean on_board;
    gboolean on_panel;

    clawt_office_scene_pointer(self, x, y);
    if (self->usage_placement == CLAWT_OFFICE_USAGE_OFF || self->usage == NULL)
        return FALSE;
    on_board = self->usage_placement == CLAWT_OFFICE_USAGE_WORLD && self->board_hovered;
    on_panel = self->panel_hovered;

    /* the panel's own clicks go nowhere: it is a sign, not a button */
    if (on_panel)
        return TRUE;
    /* the board pins its panel open, and a second click closes it */
    if (on_board) {
        clawt_office_scene_set_usage_open(self, !self->panel_pinned);
        return TRUE;
    }
    /* a click away from a pinned panel closes it, and does nothing else */
    if (self->usage_placement == CLAWT_OFFICE_USAGE_WORLD && self->panel_pinned) {
        clawt_office_scene_set_usage_open(self, FALSE);
        return TRUE;
    }
    return FALSE;
}

gboolean
clawt_office_scene_dismiss(ClawtOfficeScene *self)
{
    if (self->usage_placement != CLAWT_OFFICE_USAGE_WORLD ||
        !clawt_office_scene_get_usage_open(self))
        return FALSE;
    self->panel_pinned = FALSE;
    /* and not straight back open under a pointer still on the board or panel */
    self->hover_quiet = self->board_hovered || self->panel_hovered;
    return TRUE;
}

/* One layout as JSON, for a test or a bug report. */
static void
add_layout(JsonBuilder *b, const gchar *name, ClawtOfficeBoardLayout *layout)
{
    static const gchar *const details[] = { "none", "mark", "gauges", "rows", "panel" };
    static const gchar *const kinds[] = { "backing", "track", "fill", "badge", "text" };
    guint i;

    json_builder_set_member_name(b, name);
    if (layout == NULL) {
        json_builder_add_null_value(b);
        return;
    }
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "detail");
    json_builder_add_string_value(b, details[layout->detail]);
    json_builder_set_member_name(b, "rect");
    json_builder_begin_array(b);
    json_builder_add_double_value(b, layout->x);
    json_builder_add_double_value(b, layout->y);
    json_builder_add_double_value(b, layout->width);
    json_builder_add_double_value(b, layout->height);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "hidden");
    json_builder_add_int_value(b, layout->hidden);
    json_builder_set_member_name(b, "items");
    json_builder_begin_array(b);
    for (i = 0; i < layout->items->len; i++) {
        ClawtOfficeBoardItem *item = &g_array_index(layout->items, ClawtOfficeBoardItem, i);

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "kind");
        json_builder_add_string_value(b, kinds[item->kind]);
        json_builder_set_member_name(b, "rect");
        json_builder_begin_array(b);
        json_builder_add_double_value(b, item->x);
        json_builder_add_double_value(b, item->y);
        json_builder_add_double_value(b, item->width);
        json_builder_add_double_value(b, item->height);
        json_builder_end_array(b);
        if (item->text != NULL) {
            static const gchar *const inks[] = { "title", "label", "value", "muted",
                                                 "on_badge" };

            json_builder_set_member_name(b, "text");
            json_builder_add_string_value(b, item->text);
            json_builder_set_member_name(b, "ink");
            json_builder_add_string_value(b, inks[item->ink]);
            json_builder_set_member_name(b, "size");
            json_builder_add_double_value(b, item->size);
            json_builder_set_member_name(b, "room");
            json_builder_add_double_value(b, item->room);
        }
        if (item->tone != NULL) {
            json_builder_set_member_name(b, "tone");
            json_builder_add_string_value(b, item->tone);
        }
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
}

JsonObject *
clawt_office_scene_describe_usage(ClawtOfficeScene *self)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonNode) root = NULL;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "view");
    json_builder_begin_array(b);
    json_builder_add_int_value(b, self->last_w);
    json_builder_add_int_value(b, self->last_h);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "placement");
    json_builder_add_string_value(b, clawt_office_usage_placement_nick(self->usage_placement));
    json_builder_set_member_name(b, "mode");
    json_builder_add_string_value(b, self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d
                                     ? "3d" : "2d");
    json_builder_set_member_name(b, "logins");
    json_builder_add_int_value(b, self->board != NULL ? self->board->logins->len : 0);
    json_builder_set_member_name(b, "windows");
    json_builder_add_int_value(b, self->board != NULL
                                  ? clawt_office_board_n_windows(self->board) : 0);
    /*
     * The zone on screen: as the 2D view places it, or the rectangle the
     * 3D board's corners covered with the last frame's camera.
     */
    if (self->has_board && self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d) {
        Rectangle zone;

        if (board_zone_3d(self, self->last_w, self->last_h, &zone)) {
            json_builder_set_member_name(b, "zone");
            json_builder_begin_array(b);
            json_builder_add_double_value(b, zone.x);
            json_builder_add_double_value(b, zone.y);
            json_builder_add_double_value(b, zone.width);
            json_builder_add_double_value(b, zone.height);
            json_builder_end_array(b);
        }
    } else if (self->has_board && self->adapter == NULL) {
        Rectangle zone = board_zone_2d(self);

        json_builder_set_member_name(b, "zone");
        json_builder_begin_array(b);
        json_builder_add_double_value(b, zone.x);
        json_builder_add_double_value(b, zone.y);
        json_builder_add_double_value(b, zone.width);
        json_builder_add_double_value(b, zone.height);
        json_builder_end_array(b);
    }
    add_layout(b, "board", self->board_layout);
    add_layout(b, "panel", self->panel_layout);
    json_builder_end_object(b);
    root = json_builder_get_root(b);
    return json_object_ref(json_node_get_object(root));
}

/* ── Simulate mode ──────────────────────────────────────────────── */

typedef struct {
    gfloat x;
    gfloat y;
} Seat;

/* Every anchor of every zone tagged @tag, in order. */
static GArray *
seats_tagged(JsonObject *world, const gchar *tag)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(Seat));
    JsonArray *zones = json_object_has_member(world, "zones")
        ? json_object_get_array_member(world, "zones") : NULL;
    guint i;
    guint t;
    guint s;

    for (i = 0; zones != NULL && i < json_array_get_length(zones); i++) {
        JsonObject *zone = json_array_get_object_element(zones, i);
        JsonArray *tags = json_object_has_member(zone, "tags")
            ? json_object_get_array_member(zone, "tags") : NULL;
        JsonArray *anchors = json_object_has_member(zone, "anchors_2d")
            ? json_object_get_array_member(zone, "anchors_2d") : NULL;
        gboolean match = FALSE;

        for (t = 0; tags != NULL && t < json_array_get_length(tags); t++)
            if (g_str_has_prefix(json_array_get_string_element(tags, t), tag))
                match = TRUE;
        /* anchors_2d is a list of [x, y] points (clawt_office_world_to_json()) */
        for (s = 0; match && anchors != NULL && s < json_array_get_length(anchors); s++) {
            JsonArray *point = json_array_get_array_element(anchors, s);
            Seat seat;

            if (point == NULL || json_array_get_length(point) < 2)
                continue;
            seat.x = (gfloat)json_array_get_double_element(point, 0);
            seat.y = (gfloat)json_array_get_double_element(point, 1);
            g_array_append_val(out, seat);
        }
    }
    return out;
}

static void
add_placement(JsonBuilder *b, const gchar *id, const gchar *activity, const gchar *pose,
              const gchar *prop, const gchar *emote, const gchar *facing, Seat seat)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_string_value(b, id);
    json_builder_set_member_name(b, "activity");
    json_builder_add_string_value(b, activity);
    json_builder_set_member_name(b, "pose");
    json_builder_add_string_value(b, pose);
    if (prop != NULL) {
        json_builder_set_member_name(b, "prop");
        json_builder_add_string_value(b, prop);
    }
    if (emote != NULL) {
        json_builder_set_member_name(b, "emote");
        json_builder_add_string_value(b, emote);
    }
    if (facing != NULL) {
        json_builder_set_member_name(b, "facing");
        json_builder_add_string_value(b, facing);
    }
    json_builder_set_member_name(b, "x");
    json_builder_add_double_value(b, seat.x);
    json_builder_set_member_name(b, "y");
    json_builder_add_double_value(b, seat.y);
    json_builder_end_object(b);
}

JsonObject *
clawt_office_simulate_layout(JsonObject *world, JsonObject *layout, GRand *rng)
{
    JsonArray *people = json_object_has_member(layout, "participants")
        ? json_object_get_array_member(layout, "participants") : NULL;
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(GArray) desks = seats_tagged(world, "desk");
    g_autoptr(GArray) booths = seats_tagged(world, "phone_booth");
    g_autoptr(GArray) meeting = seats_tagged(world, "meeting");
    g_autoptr(GArray) lounge = seats_tagged(world, "lounge");
    g_autoptr(GArray) bugs = seats_tagged(world, "bug_corner");
    g_autoptr(GArray) library = seats_tagged(world, "library");
    g_autoptr(JsonNode) root = NULL;
    guint n = people != NULL ? json_array_get_length(people) : 0;
    guint i;
    guint at_desk = 0;
    guint at_meeting = 0;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "placements");
    json_builder_begin_array(b);
    for (i = 0; i < n; i++) {
        JsonObject *p = json_array_get_object_element(people, i);
        const gchar *id = member(p, "id");
        guint roll = (guint)g_rand_int_range(rng, 0, 10);
        Seat seat = { 0.0f, 0.0f };

        if (id == NULL)
            continue;
        /* the first two on a call with each other, in the booths if there are two */
        if (i < 2 && n >= 2 && booths->len >= 2) {
            JsonObject *other = json_array_get_object_element(people, 1 - i);

            seat = g_array_index(booths, Seat, i);
            add_placement(b, id, "on_call", "phone", "phone", NULL, member(other, "id"), seat);
        } else if (i >= 2 && i < 5 && n >= 5 && meeting->len > at_meeting) {
            seat = g_array_index(meeting, Seat, at_meeting++);
            add_placement(b, id, "in_meeting", "sit", NULL, NULL, NULL, seat);
        } else if (roll == 0 && bugs->len > 0) {
            seat = g_array_index(bugs, Seat, 0);
            add_placement(b, id, "error", "stand", NULL, "alert", NULL, seat);
        } else if (roll == 1 && desks->len > at_desk) {
            seat = g_array_index(desks, Seat, at_desk++);
            add_placement(b, id, "waiting_on_you", "stand", NULL, "question", NULL, seat);
        } else if (roll <= 3 && library->len > 0) {
            seat = g_array_index(library, Seat, i % library->len);
            add_placement(b, id, "working", "read", "book", NULL, NULL, seat);
        } else if (roll <= 7 && desks->len > at_desk) {
            seat = g_array_index(desks, Seat, at_desk++);
            add_placement(b, id, "working", "type", "laptop", NULL, NULL, seat);
        } else if (lounge->len > 0) {
            seat = g_array_index(lounge, Seat, i % lounge->len);
            add_placement(b, id, "idle", "sit", "coffee", NULL, NULL, seat);
        }
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "participants");
    json_builder_add_value(b, people != NULL
        ? json_node_copy(json_object_get_member(layout, "participants"))
        : json_node_new(JSON_NODE_ARRAY));
    json_builder_end_object(b);
    root = json_builder_get_root(b);
    return json_object_ref(json_node_get_object(root));
}
