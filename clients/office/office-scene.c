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
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WALK_PIXELS_PER_SECOND (150.0f)
#define ARRIVED_PIXELS         (0.6f)
#define BODY_RADIUS            (9.0f)
#define PERSON_SCALE           (1.7f)

/* ── People ─────────────────────────────────────────────────────── */

typedef struct {
    gchar           *id;
    gchar           *name;
    gchar           *kind;
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
    gfloat                orbit;      /* radians round the map's centre */
    gfloat                distance;   /* 3D camera distance, in tiles */
    GHashTable           *actors;     /* id -> Actor */
    GHashTable           *colors;     /* id -> "#rrggbb" */
    GHashTable           *beams;      /* "a|b" -> LrgBeam */
    gchar                *status;
    gchar                *hovered;
    Camera2D              camera2d;
    RenderTexture2D       baked;      /* the map at 1:1, drawn once */
    gboolean              baked_ok;
    Camera3D              camera3d;
    gint                  last_w;
    gint                  last_h;
    gfloat                clock;
};

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
    self->tile = 32.0f;
    self->orbit = 0.65f;
    self->distance = 1.0f;
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

    if (!self->has_2d && !self->has_3d) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "the world has neither a map nor a level this build can draw");
        return FALSE;
    }
    if (self->map_w <= 0.0f) {
        self->map_w = 44 * 32;
        self->map_h = 40 * 32;
    }

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

    /* whoever is no longer in the layout walks out */
    g_hash_table_iter_init(&iter, self->actors);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Actor *a = value;

        if (!a->seen && !a->leaving) {
            a->leaving = TRUE;
            a->tx = self->entrance_x;
            a->ty = self->entrance_y;
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
clawt_office_scene_orbit(ClawtOfficeScene *self, gfloat dx, gfloat zoom)
{
    self->orbit += dx;
    if (zoom != 0.0f)
        self->distance = CLAMP(self->distance * (zoom > 0.0f ? 0.9f : 1.1f), 0.35f, 2.0f);
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
    if (!self->has_2d || self->baked_ok || !IsWindowReady())
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
    for (i = 0; i < gone->len; i++)
        g_hash_table_remove(self->actors, g_ptr_array_index(gone, i));

    {
        GHashTableIter beams;

        g_hash_table_iter_init(&beams, self->beams);
        while (g_hash_table_iter_next(&beams, NULL, &value)) {
            lrg_beam_update(LRG_BEAM(value), delta);
            moving = TRUE;
        }
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
            lrg_beam_set_endpoints(beam, a->x, a->y - 18.0f * PERSON_SCALE, 0.0f, b->x, b->y - 18.0f * PERSON_SCALE, 0.0f);
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
draw_person_2d(Actor *a, gfloat x, gfloat y, gboolean hovered)
{
    gfloat alpha = a->fade;
    gboolean sitting = g_strcmp0(a->pose, "sit") == 0 || g_strcmp0(a->pose, "type") == 0 ||
                       g_strcmp0(a->pose, "read") == 0;
    gfloat lift = sitting ? 0.0f : 3.0f;
    gfloat swing = sinf(a->walk_phase) * 2.5f;
    Color body = with_alpha(shade(a->color, 0.72f), alpha);
    Color head = with_alpha(a->color, alpha);
    Color eye = with_alpha(parse_color(clawt_face_eye_color(clawt_avatar_color(a->id, a->name,
                                                                               NULL)),
                                       (Color){ 20, 20, 30, 255 }), alpha);

    DrawEllipse((gint)x, (gint)(y + 7.0f), 11.0f, 4.0f, with_alpha((Color){ 0, 0, 0, 70 }, alpha));
    if (!sitting) {
        DrawLineEx((Vector2){ x - 3.0f, y + 2.0f }, (Vector2){ x - 3.0f + swing, y + 8.0f }, 3.0f,
                   with_alpha((Color){ 50, 52, 62, 255 }, alpha));
        DrawLineEx((Vector2){ x + 3.0f, y + 2.0f }, (Vector2){ x + 3.0f - swing, y + 8.0f }, 3.0f,
                   with_alpha((Color){ 50, 52, 62, 255 }, alpha));
    }
    DrawRectangleRounded((Rectangle){ x - 7.0f, y - 8.0f - lift, 14.0f, 13.0f }, 0.5f, 6, body);
    if (hovered)
        DrawCircleLines((gint)x, (gint)(y - 15.0f - lift), BODY_RADIUS + 2.0f,
                        with_alpha((Color){ 255, 255, 255, 220 }, alpha));
    DrawCircleV((Vector2){ x, y - 15.0f - lift }, BODY_RADIUS, head);
    DrawCircleV((Vector2){ x - 3.0f, y - 16.0f - lift }, 1.6f, eye);
    DrawCircleV((Vector2){ x + 3.0f, y - 16.0f - lift }, 1.6f, eye);
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

/* The overlays over one person, at its screen position. */
static void
draw_overlays(ClawtOfficeScene *self, Actor *a, Vector2 at, gfloat scale)
{
    gfloat above = 30.0f * scale;

    (void)self;
    lrg_nameplate_set_hovered(a->plate, g_strcmp0(self->hovered, a->id) == 0);
    lrg_world_widget_draw_at(LRG_WORLD_WIDGET(a->plate), at.x, at.y + 12.0f * scale);
    if (lrg_world_widget_is_shown(LRG_WORLD_WIDGET(a->mark)) ||
        lrg_world_widget_get_alpha(LRG_WORLD_WIDGET(a->mark)) > 0.01f)
        lrg_world_widget_draw_at(LRG_WORLD_WIDGET(a->mark), at.x, at.y - above);
    if (lrg_world_widget_get_alpha(LRG_WORLD_WIDGET(a->bubble)) > 0.01f)
        lrg_world_widget_draw_at(LRG_WORLD_WIDGET(a->bubble), at.x,
                                 at.y - above - 14.0f * scale);
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

static void
draw_2d(ClawtOfficeScene *self, gint width, gint height)
{
    g_autoptr(GPtrArray) people = sorted_actors(self);
    gfloat zoom = MIN((gfloat)width / self->map_w, (gfloat)height / self->map_h) * 0.98f;
    guint i;

    self->camera2d.offset = (Vector2){ width / 2.0f, height / 2.0f };
    self->camera2d.target = (Vector2){ self->map_w / 2.0f, self->map_h / 2.0f };
    self->camera2d.rotation = 0.0f;
    self->camera2d.zoom = zoom;

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
        rlScalef(PERSON_SCALE, PERSON_SCALE, 1.0f);
        draw_person_2d(a, 0.0f, 0.0f, g_strcmp0(self->hovered, a->id) == 0);
        rlPopMatrix();
    }
    EndMode2D();

    /* text in screen space, so it stays legible at any zoom */
    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);
        Vector2 at = GetWorldToScreen2D((Vector2){ a->x, a->y }, self->camera2d);

        draw_overlays(self, a, at, PERSON_SCALE * zoom);
    }
}

/* The orbiting god's-eye camera, aimed at the middle of the map. */
static void
place_camera_3d(ClawtOfficeScene *self, gint width, gint height)
{
    gfloat cx = self->map_w / self->tile / 2.0f;
    gfloat cz = self->map_h / self->tile / 2.0f;
    gfloat reach = MAX(cx, cz) * 1.2f * self->distance;
    Vector3 eye = { cx + cosf(self->orbit) * reach, reach * 0.95f,
                    cz + sinf(self->orbit) * reach };

    self->camera3d.position = eye;
    self->camera3d.target = (Vector3){ cx, 0.0f, cz };
    self->camera3d.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    self->camera3d.fovy = 45.0f;
    self->camera3d.projection = CAMERA_PERSPECTIVE;
    lrg_camera3d_set_position_xyz(self->lcamera, eye.x, eye.y, eye.z);
    lrg_camera3d_set_target_xyz(self->lcamera, cx, 0.0f, cz);
    lrg_camera3d_set_up_xyz(self->lcamera, 0.0f, 1.0f, 0.0f);
    lrg_camera3d_set_fovy(self->lcamera, 45.0f);
    lrg_camera3d_set_aspect(self->lcamera, (gfloat)width / (gfloat)MAX(height, 1));
}

static void
draw_3d(ClawtOfficeScene *self, gint width, gint height)
{
    g_autoptr(GPtrArray) people = sorted_actors(self);
    gfloat k = 1.0f / self->tile;
    guint i;

    place_camera_3d(self, width, height);
    lrg_world3d_renderer_render(self->renderer, self->world3d, self->lcamera, NULL,
                                width, height);

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

        /* a figure about two tiles tall, so it reads from a god's-eye camera */
        DrawCapsule((Vector3){ feet.x, base + 0.45f, feet.z },
                    (Vector3){ feet.x, base + 1.25f, feet.z }, 0.42f, 10, 8, body);
        DrawSphere((Vector3){ feet.x, base + 2.0f, feet.z }, 0.5f, head);
        DrawSphere((Vector3){ feet.x + 0.2f * cosf(self->orbit + G_PI),
                              base + 2.08f,
                              feet.z + 0.2f * sinf(self->orbit + G_PI) }, 0.08f,
                   with_alpha((Color){ 20, 20, 30, 255 }, a->fade));
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

    for (i = 0; i < people->len; i++) {
        Actor *a = g_ptr_array_index(people, i);
        Vector2 at = GetWorldToScreenEx((Vector3){ a->x * k, 2.6f, a->y * k },
                                        self->camera3d, width, height);

        draw_overlays(self, a, at, 1.0f);
    }
}

void
clawt_office_scene_draw(ClawtOfficeScene *self, gint width, gint height)
{
    self->last_w = width;
    self->last_h = height;
    ClearBackground((Color){ 24, 26, 32, 255 });

    if (self->mode == CLAWT_OFFICE_SCENE_3D && self->has_3d)
        draw_3d(self, width, height);
    else if (self->has_2d)
        draw_2d(self, width, height);

    if (self->status != NULL) {
        gint size = 16;
        gint text_w = MeasureText(self->status, size);

        DrawRectangleRounded((Rectangle){ 10.0f, 10.0f, (gfloat)text_w + 20.0f, 28.0f }, 0.4f,
                             6, (Color){ 0, 0, 0, 150 });
        DrawText(self->status, 20, 16, size, RAYWHITE);
    }
}

const gchar *
clawt_office_scene_pointer(ClawtOfficeScene *self, gfloat x, gfloat y)
{
    GHashTableIter iter;
    gpointer value;
    Actor *best = NULL;
    gfloat best_d = 0.0f;

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
            reach = 22.0f;
        } else {
            at = GetWorldToScreen2D((Vector2){ a->x, a->y - 10.0f * PERSON_SCALE },
                                    self->camera2d);
            reach = 16.0f * PERSON_SCALE * self->camera2d.zoom;
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
