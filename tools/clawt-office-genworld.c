/*
 * clawt-office-genworld.c - Generates the office world packs clawtilla ships
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every asset in data/office/worlds/ is drawn by this program -- tiles
 * with cairo, the 3D level from libregnum primitives -- so the packs are
 * ours to license (CC0-1.0) and nothing is downloaded.  The output is
 * deterministic: run it twice and the files, and their sha256 in
 * office.yaml, are the same.  `make office-worlds` runs it; the output is
 * committed so a build never needs cairo.
 *
 * A pack is described as a list of rooms on a tile grid.  Each room has a
 * floor, a wall style, the zone it is (its tags) and furniture; the zone's
 * anchors are the seats the furniture makes, so a room's capacity is how
 * many seats it actually has.  From that one description come:
 *
 *   tiles.png      the tileset (32x32 tiles)
 *   map.tmj        a Tiled JSON map: floor and furniture layers, and an
 *                  object layer naming each zone
 *   office.rlevel  a libregnum level of primitives, one unit per tile
 *   office.yaml    the manifest: zones with 2D and 3D anchors, assets
 *                  with sha256
 *
 * Usage: clawt-office-genworld <output-dir>     (writes one pack per world)
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <cairo.h>
#include <math.h>
#include <string.h>

#define TILE (32)

/* ── Tiles ──────────────────────────────────────────────────────── */

typedef enum {
    T_EMPTY = 0,
    T_CARPET, T_WOOD, T_TILE, T_WALL, T_DESK, T_CHAIR, T_SOFA, T_PLANT,
    T_TABLE, T_WHITEBOARD, T_SHELF, T_SERVER, T_BOOTH, T_DOOR, T_GLASS,
    T_GRASS, T_PATH, T_WATER, T_TREE, T_FLOWERS, T_BENCH, T_LAMP, T_RUG_RED,
    T_SCREEN, T_MONITOR, T_COFFEE, T_LAB, T_STONE, T_HEDGE, T_PODIUM,
    T_COUNT
} Tile;

/*
 * Each tile's name, by its index: written into the manifest as `tiles:`
 * so a program restyling a pack (clawtilla office world restyle) knows
 * a desk from a sofa without a copy of this table.  Beside the enum, in
 * its order, so the two are one edit.
 */
static const gchar *const tile_names[T_COUNT] = {
    "empty",
    "carpet", "wood", "tile", "wall", "desk", "chair", "sofa", "plant",
    "table", "whiteboard", "shelf", "server", "booth", "door", "glass",
    "grass", "path", "water", "tree", "flowers", "bench", "lamp", "red rug",
    "screen", "monitor", "coffee machine", "lab bench", "stone", "hedge", "podium"
};

static void
rgb(cairo_t *cr, guint hex)
{
    cairo_set_source_rgb(cr, ((hex >> 16) & 0xff) / 255.0, ((hex >> 8) & 0xff) / 255.0,
                         (hex & 0xff) / 255.0);
}

static void
rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, guint hex)
{
    rgb(cr, hex);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

static void
rounded(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r, guint hex)
{
    rgb(cr, hex);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void
circle(cairo_t *cr, gdouble x, gdouble y, gdouble r, guint hex)
{
    rgb(cr, hex);
    cairo_arc(cr, x, y, r, 0, 2 * G_PI);
    cairo_fill(cr);
}

/* One tile at (0,0); flat, friendly, readable at a glance. */
static void
draw_tile(cairo_t *cr, Tile t)
{
    gint i;

    switch (t) {
    case T_EMPTY:
        break;
    case T_CARPET:
        rect(cr, 0, 0, TILE, TILE, 0x56607a);
        for (i = 0; i < 4; i++)
            rect(cr, i * 8 + 2, (i % 2) * 16 + 4, 3, 3, 0x5e6984);
        break;
    case T_WOOD:
        rect(cr, 0, 0, TILE, TILE, 0xb88655);
        for (i = 0; i < 4; i++)
            rect(cr, 0, i * 8 + 7, TILE, 1, 0x9e6f44);
        rect(cr, 14, 0, 1, 8, 0x9e6f44);
        rect(cr, 6, 16, 1, 8, 0x9e6f44);
        break;
    case T_TILE:
        rect(cr, 0, 0, TILE, TILE, 0xd9dde3);
        rect(cr, 0, 15, TILE, 1, 0xc3c8cf);
        rect(cr, 15, 0, 1, TILE, 0xc3c8cf);
        break;
    case T_WALL:
        rect(cr, 0, 0, TILE, TILE, 0x3b4252);
        rect(cr, 0, 0, TILE, 6, 0x4c566a);
        break;
    case T_DESK:
        rect(cr, 0, 0, TILE, TILE, 0x56607a);
        rounded(cr, 2, 4, 28, 18, 3, 0xd8c3a5);
        rect(cr, 9, 7, 14, 9, 0x2e3440);
        rect(cr, 10, 8, 12, 7, 0x88c0d0);
        break;
    case T_CHAIR:
        rect(cr, 0, 0, TILE, TILE, 0x56607a);
        circle(cr, 16, 16, 8, 0x434c5e);
        circle(cr, 16, 16, 5, 0x5e81ac);
        break;
    case T_SOFA:
        rect(cr, 0, 0, TILE, TILE, 0xb88655);
        rounded(cr, 1, 6, 30, 20, 5, 0xbf616a);
        rounded(cr, 4, 10, 24, 12, 4, 0xd08770);
        break;
    case T_PLANT:
        circle(cr, 16, 22, 6, 0x8f6a4a);
        circle(cr, 12, 13, 7, 0x5f9e5f);
        circle(cr, 20, 12, 7, 0x6fb36f);
        circle(cr, 16, 8, 6, 0x7cc47c);
        break;
    case T_TABLE:
        rect(cr, 0, 0, TILE, TILE, 0xd9dde3);
        rounded(cr, 1, 1, 30, 30, 4, 0x8f6a4a);
        rounded(cr, 3, 3, 26, 26, 3, 0xa97c52);
        break;
    case T_WHITEBOARD:
        rect(cr, 0, 0, TILE, TILE, 0x3b4252);
        rect(cr, 2, 3, 28, 20, 0xeceff4);
        rect(cr, 5, 8, 12, 2, 0x5e81ac);
        rect(cr, 5, 13, 18, 2, 0xbf616a);
        rect(cr, 5, 18, 8, 2, 0xa3be8c);
        break;
    case T_SHELF:
        rect(cr, 0, 0, TILE, TILE, 0x6b4f36);
        for (i = 0; i < 3; i++) {
            rect(cr, 2, 3 + i * 10, 28, 8, 0x4a3727);
            rect(cr, 3, 4 + i * 10, 4, 6, 0xbf616a);
            rect(cr, 8, 4 + i * 10, 3, 6, 0xebcb8b);
            rect(cr, 12, 4 + i * 10, 5, 6, 0x5e81ac);
            rect(cr, 18, 4 + i * 10, 3, 6, 0xa3be8c);
            rect(cr, 22, 4 + i * 10, 5, 6, 0xb48ead);
        }
        break;
    case T_SERVER:
        rect(cr, 2, 0, 28, TILE, 0x2e3440);
        for (i = 0; i < 5; i++) {
            rect(cr, 4, 2 + i * 6, 24, 4, 0x3b4252);
            circle(cr, 24, 4 + i * 6, 1.2, (i % 2) ? 0xa3be8c : 0x88c0d0);
        }
        break;
    case T_BOOTH:
        rect(cr, 0, 0, TILE, TILE, 0x88c0d0);
        rect(cr, 3, 3, 26, 26, 0xe5f3f8);
        rounded(cr, 11, 10, 10, 12, 2, 0x2e3440);
        break;
    case T_DOOR:
        rect(cr, 0, 0, TILE, TILE, 0x8f6a4a);
        rect(cr, 3, 3, 26, 26, 0xa3be8c);
        rect(cr, 3, 14, 26, 2, 0x8f6a4a);
        break;
    case T_GLASS:
        rect(cr, 0, 0, TILE, TILE, 0x56607a);
        rect(cr, 0, 12, TILE, 8, 0xa7d3e3);
        rect(cr, 0, 12, TILE, 1, 0xe5f3f8);
        break;
    case T_GRASS:
        rect(cr, 0, 0, TILE, TILE, 0x7fb069);
        for (i = 0; i < 5; i++)
            rect(cr, (i * 7) % 28 + 2, (i * 11) % 26 + 3, 2, 3, 0x6c9a59);
        break;
    case T_PATH:
        rect(cr, 0, 0, TILE, TILE, 0xd8c39a);
        circle(cr, 8, 10, 2, 0xc8b28a);
        circle(cr, 22, 22, 2.5, 0xc8b28a);
        break;
    case T_WATER:
        rect(cr, 0, 0, TILE, TILE, 0x5e9fc4);
        rect(cr, 4, 10, 10, 2, 0x8ec4de);
        rect(cr, 16, 20, 12, 2, 0x8ec4de);
        break;
    case T_TREE:
        rect(cr, 0, 0, TILE, TILE, 0x7fb069);
        circle(cr, 16, 24, 3, 0x6b4f36);
        circle(cr, 16, 13, 11, 0x3f7d4a);
        circle(cr, 12, 10, 5, 0x4f9a5a);
        break;
    case T_FLOWERS:
        rect(cr, 0, 0, TILE, TILE, 0x7fb069);
        circle(cr, 8, 9, 3, 0xbf616a);
        circle(cr, 22, 12, 3, 0xebcb8b);
        circle(cr, 14, 22, 3, 0xb48ead);
        circle(cr, 25, 25, 2.5, 0xeceff4);
        break;
    case T_BENCH:
        rect(cr, 0, 0, TILE, TILE, 0xd8c39a);
        rounded(cr, 2, 10, 28, 10, 2, 0x8f6a4a);
        rect(cr, 2, 14, 28, 1, 0x6b4f36);
        break;
    case T_LAMP:
        rect(cr, 0, 0, TILE, TILE, 0xd8c39a);
        circle(cr, 16, 16, 7, 0xebcb8b);
        circle(cr, 16, 16, 3, 0xfff4cc);
        break;
    case T_RUG_RED:
        rect(cr, 0, 0, TILE, TILE, 0x9c3e46);
        rect(cr, 3, 3, 26, 26, 0xbf616a);
        rect(cr, 13, 8, 6, 11, 0xeceff4);
        rect(cr, 13, 21, 6, 4, 0xeceff4);
        break;
    case T_SCREEN:
        rect(cr, 0, 0, TILE, TILE, 0x3b4252);
        rect(cr, 2, 4, 28, 22, 0x2e3440);
        rect(cr, 5, 8, 18, 3, 0xa3be8c);
        rect(cr, 5, 14, 11, 3, 0xebcb8b);
        rect(cr, 5, 20, 6, 3, 0xbf616a);
        break;
    case T_MONITOR:
        rect(cr, 0, 0, TILE, TILE, 0x56607a);
        rounded(cr, 2, 4, 28, 18, 3, 0x4c566a);
        rect(cr, 5, 6, 10, 8, 0xa3be8c);
        rect(cr, 17, 6, 10, 8, 0x88c0d0);
        break;
    case T_COFFEE:
        rect(cr, 0, 0, TILE, TILE, 0xd9dde3);
        rounded(cr, 6, 3, 20, 26, 3, 0x2e3440);
        circle(cr, 16, 20, 4, 0x8f6a4a);
        rect(cr, 10, 7, 12, 4, 0xbf616a);
        break;
    case T_LAB:
        rect(cr, 0, 0, TILE, TILE, 0xd9dde3);
        rounded(cr, 1, 5, 30, 20, 2, 0xeceff4);
        circle(cr, 9, 14, 4, 0x88c0d0);
        circle(cr, 22, 14, 4, 0xa3be8c);
        break;
    case T_STONE:
        rect(cr, 0, 0, TILE, TILE, 0x9aa3ad);
        rect(cr, 0, 15, TILE, 2, 0x828b95);
        rect(cr, 15, 0, 2, 15, 0x828b95);
        rect(cr, 6, 17, 2, 15, 0x828b95);
        break;
    case T_HEDGE:
        rect(cr, 0, 0, TILE, TILE, 0x7fb069);
        rounded(cr, 1, 4, 30, 24, 8, 0x3f7d4a);
        break;
    case T_PODIUM:
        rect(cr, 0, 0, TILE, TILE, 0xbf616a);
        rounded(cr, 8, 6, 16, 20, 2, 0x6b4f36);
        break;
    case T_COUNT:
        break;
    }
}

static gboolean
write_tileset(const gchar *path)
{
    gint cols = 8;
    gint rows = (T_COUNT + cols - 1) / cols;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          cols * TILE, rows * TILE);
    cairo_t *cr = cairo_create(surface);
    gint t;
    cairo_status_t status;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
    for (t = 1; t < T_COUNT; t++) {
        cairo_save(cr);
        cairo_translate(cr, ((t - 1) % cols) * TILE, ((t - 1) / cols) * TILE);
        cairo_rectangle(cr, 0, 0, TILE, TILE);
        cairo_clip(cr);
        draw_tile(cr, (Tile)t);
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    status = cairo_surface_write_to_png(surface, path);
    cairo_surface_destroy(surface);
    return status == CAIRO_STATUS_SUCCESS;
}

/* ── Worlds as rooms ────────────────────────────────────────────── */

typedef enum {
    F_NONE, F_DESKS, F_SOFAS, F_MEETING, F_HALL, F_SHELVES, F_LAB, F_WHITEBOARD,
    F_SERVERS, F_BOOTH, F_BUGS, F_ENTRANCE, F_SCREEN, F_TERMINALS, F_COFFEE,
    F_OFFICE, F_BENCHES, F_VISITOR
} Furnish;

typedef struct {
    const gchar *id;
    const gchar *name;
    const gchar *tags;     /* comma separated */
    gint x, y, w, h;       /* in tiles, walls included */
    Tile floor;
    Tile wall;             /* T_EMPTY for none */
    Furnish furnish;
    guint color;           /* 3D floor colour */
} Room;

typedef struct {
    const gchar *id;
    const gchar *name;
    const gchar *description;
    gint width, height;
    Tile ground;
    guint ground_color;
    const Room *rooms;
    gsize n_rooms;
    gboolean town;
} World;

static const Room office_rooms[] = {
    { "chief", "Chief's office", "chief_office", 1, 1, 9, 7, T_WOOD, T_GLASS, F_OFFICE, 0xb88655 },
    { "leads", "Leads' offices", "lead_office", 1, 8, 9, 7, T_WOOD, T_GLASS, F_DESKS, 0xb88655 },
    { "floor", "Open floor", "desk", 11, 1, 22, 12, T_CARPET, T_EMPTY, F_DESKS, 0x56607a },
    { "huddle", "Huddle room", "meeting:4", 34, 1, 9, 6, T_WOOD, T_GLASS, F_MEETING, 0xb88655 },
    { "boardroom", "Boardroom", "meeting:10", 34, 8, 9, 8, T_WOOD, T_GLASS, F_MEETING, 0xb88655 },
    { "library", "Library", "library", 1, 16, 9, 8, T_WOOD, T_WALL, F_SHELVES, 0xa97c52 },
    { "lab", "Lab", "lab", 11, 14, 8, 6, T_TILE, T_WALL, F_LAB, 0xd9dde3 },
    { "planning", "Planning wall", "whiteboard", 20, 14, 6, 6, T_CARPET, T_EMPTY, F_WHITEBOARD, 0x56607a },
    { "ops", "Ops desk", "terminal", 27, 14, 6, 6, T_CARPET, T_EMPTY, F_TERMINALS, 0x56607a },
    { "servers", "Server room", "server_room", 34, 17, 9, 6, T_TILE, T_WALL, F_SERVERS, 0x9aa3ad },
    { "lounge", "Lounge", "lounge", 11, 21, 12, 8, T_WOOD, T_EMPTY, F_SOFAS, 0xb88655 },
    { "kitchen", "Kitchen", "breakroom", 24, 21, 7, 8, T_TILE, T_WALL, F_COFFEE, 0xd9dde3 },
    { "booths", "Phone booths", "phone_booth", 1, 25, 9, 4, T_CARPET, T_EMPTY, F_BOOTH, 0x56607a },
    { "bugs", "Bug corner", "bug_corner", 32, 24, 5, 5, T_RUG_RED, T_EMPTY, F_BUGS, 0xbf616a },
    { "hall", "Town hall", "town_hall", 11, 30, 22, 9, T_CARPET, T_WALL, F_HALL, 0x4c566a },
    { "visitors", "Visitor desk", "visitor", 34, 30, 9, 4, T_TILE, T_EMPTY, F_VISITOR, 0xd9dde3 },
    { "board", "Usage board", "usage_board", 38, 24, 5, 5, T_CARPET, T_EMPTY, F_SCREEN, 0x56607a },
    { "lobby", "Lobby", "entrance", 34, 35, 9, 4, T_TILE, T_EMPTY, F_ENTRANCE, 0xd9dde3 },
};

static const Room town_rooms[] = {
    { "manor", "The chief's manor", "chief_office", 2, 2, 10, 8, T_WOOD, T_WALL, F_OFFICE, 0xb88655 },
    { "guildhall", "Guild hall", "lead_office", 14, 2, 10, 8, T_WOOD, T_WALL, F_DESKS, 0xb88655 },
    { "workshop-east", "East workshop", "desk", 26, 2, 12, 9, T_WOOD, T_WALL, F_DESKS, 0xa97c52 },
    { "workshop-west", "West workshop", "desk", 40, 2, 12, 9, T_WOOD, T_WALL, F_DESKS, 0xa97c52 },
    { "library", "Library", "library", 2, 13, 10, 8, T_WOOD, T_WALL, F_SHELVES, 0xa97c52 },
    { "smithy", "Smithy", "lab", 14, 13, 8, 7, T_STONE, T_WALL, F_LAB, 0x9aa3ad },
    { "noticeboard", "Notice board", "whiteboard", 24, 14, 5, 4, T_PATH, T_EMPTY, F_WHITEBOARD, 0xd8c39a },
    { "observatory", "Observatory", "terminal", 31, 13, 7, 7, T_STONE, T_WALL, F_TERMINALS, 0x9aa3ad },
    { "teahouse", "Tea house", "meeting:4", 40, 13, 7, 6, T_WOOD, T_WALL, F_MEETING, 0xb88655 },
    { "townhall", "Town hall", "town_hall", 14, 23, 20, 10, T_STONE, T_WALL, F_HALL, 0x9aa3ad },
    { "council", "Council chamber", "meeting:10", 40, 21, 10, 8, T_WOOD, T_WALL, F_MEETING, 0xb88655 },
    { "cafe", "Café garden", "lounge", 2, 24, 10, 8, T_PATH, T_EMPTY, F_SOFAS, 0xd8c39a },
    { "bakery", "Bakery", "breakroom", 2, 34, 8, 6, T_WOOD, T_WALL, F_COFFEE, 0xb88655 },
    { "phonebox", "Phone box", "phone_booth", 36, 24, 3, 3, T_PATH, T_EMPTY, F_BOOTH, 0xd8c39a },
    { "repair", "Repair shed", "bug_corner", 12, 35, 6, 5, T_STONE, T_WALL, F_BUGS, 0x9aa3ad },
    { "inn", "The inn", "visitor", 40, 31, 10, 8, T_WOOD, T_WALL, F_VISITOR, 0xb88655 },
    { "square", "Market square", "usage_board", 22, 35, 9, 5, T_STONE, T_EMPTY, F_SCREEN, 0x9aa3ad },
    { "gate", "Town gate", "entrance", 34, 36, 4, 4, T_PATH, T_EMPTY, F_ENTRANCE, 0xd8c39a },
};

static const World worlds[] = {
    { "modern-office", "Modern Office",
      "A glassy open-plan office: desks, a boardroom and a huddle room, a library, "
      "a lab, a lounge and kitchen, phone booths, a bug corner and a town hall.",
      44, 40, T_CARPET, 0x4c566a, office_rooms, G_N_ELEMENTS(office_rooms), FALSE },
    { "cozy-town", "Cozy Town",
      "A little town: workshops for the teams, a manor for the chief, a guild hall "
      "for the leads, a tea house and council chamber, a café garden, a library, a "
      "smithy, an inn for visitors and a town hall on the square.",
      54, 42, T_GRASS, 0x7fb069, town_rooms, G_N_ELEMENTS(town_rooms), TRUE },
};

/* ── Laying a world out ─────────────────────────────────────────── */

typedef struct {
    gint      w, h;
    guint16  *floor;
    guint16  *top;
    GArray   *anchors;  /* per room: GArray of gdouble x,y (tile units) */
} Grid;

static void
set(guint16 *layer, gint w, gint x, gint y, Tile t)
{
    layer[y * w + x] = (guint16)t;
}

static void
seat(GArray *anchors, gdouble x, gdouble y)
{
    gdouble pair[2];

    pair[0] = x + 0.5;
    pair[1] = y + 0.5;
    g_array_append_vals(anchors, pair, 2);
}

/* Furniture into the top layer, and the seats it makes into @anchors. */
static void
furnish(Grid *g, const Room *r, GArray *anchors)
{
    gint ix = r->x + 1, iy = r->y + 1;
    gint iw = r->w - 2, ih = r->h - 2;
    gint x, y;

    switch (r->furnish) {
    case F_NONE:
        break;
    case F_DESKS:
        for (y = iy + 1; y + 1 < iy + ih; y += 3)
            for (x = ix + 1; x < ix + iw - 1; x += 3) {
                set(g->top, g->w, x, y, T_DESK);
                set(g->top, g->w, x, y + 1, T_CHAIR);
                seat(anchors, x, y + 1);
            }
        break;
    case F_OFFICE:
        set(g->top, g->w, ix + iw / 2, iy + 1, T_DESK);
        set(g->top, g->w, ix + iw / 2, iy + 2, T_CHAIR);
        seat(anchors, ix + iw / 2, iy + 2);
        set(g->top, g->w, ix, iy, T_PLANT);
        set(g->top, g->w, ix + iw - 1, iy, T_SHELF);
        set(g->top, g->w, ix + 1, iy + ih - 2, T_SOFA);
        seat(anchors, ix + 1, iy + ih - 2);
        break;
    case F_MEETING:
    case F_HALL:
        /* a long table with chairs either side; the hall adds a podium */
        for (x = ix + 2; x < ix + iw - 2; x++) {
            set(g->top, g->w, x, iy + ih / 2, T_TABLE);
            set(g->top, g->w, x, iy + ih / 2 - 1, T_CHAIR);
            set(g->top, g->w, x, iy + ih / 2 + 1, T_CHAIR);
            seat(anchors, x, iy + ih / 2 - 1);
            seat(anchors, x, iy + ih / 2 + 1);
        }
        if (r->furnish == F_HALL) {
            set(g->top, g->w, ix + iw / 2, iy, T_PODIUM);
            for (x = ix + 1; x < ix + iw - 1; x += 2) {
                set(g->top, g->w, x, iy + ih - 1, T_BENCH);
                seat(anchors, x, iy + ih - 1);
            }
        }
        break;
    case F_SHELVES:
        for (x = ix; x < ix + iw; x++)
            set(g->top, g->w, x, iy, T_SHELF);
        for (y = iy + 2; y < iy + ih; y += 2)
            for (x = ix + 1; x < ix + iw - 1; x += 3) {
                set(g->top, g->w, x, y, T_TABLE);
                seat(anchors, x + 1, y);
            }
        break;
    case F_LAB:
        for (x = ix; x < ix + iw; x += 2) {
            set(g->top, g->w, x, iy, T_LAB);
            seat(anchors, x, iy + 1);
        }
        break;
    case F_WHITEBOARD:
        for (x = ix; x < ix + iw; x++)
            set(g->top, g->w, x, iy, T_WHITEBOARD);
        for (x = ix; x < ix + iw; x++)
            seat(anchors, x, iy + 1);
        break;
    case F_SERVERS:
        for (y = iy; y < iy + ih; y += 2)
            for (x = ix; x < ix + iw; x += 2)
                set(g->top, g->w, x, y, T_SERVER);
        seat(anchors, ix + 1, iy + ih - 1);
        break;
    case F_BOOTH:
        for (x = ix; x < ix + iw; x += 2) {
            set(g->top, g->w, x, iy, T_BOOTH);
            seat(anchors, x, iy);
        }
        break;
    case F_BUGS:
        set(g->top, g->w, ix, iy, T_PLANT);
        for (x = ix; x < ix + iw; x++)
            seat(anchors, x, iy + ih - 1);
        break;
    case F_ENTRANCE:
        set(g->top, g->w, ix + iw / 2, iy + ih - 1, T_DOOR);
        for (x = ix; x < ix + iw; x++)
            seat(anchors, x, iy + ih / 2);
        break;
    case F_SCREEN:
        for (x = ix; x < ix + iw; x++)
            set(g->top, g->w, x, iy, T_SCREEN);
        seat(anchors, ix + iw / 2, iy + 2);
        break;
    case F_TERMINALS:
        for (x = ix; x < ix + iw; x += 2) {
            set(g->top, g->w, x, iy, T_MONITOR);
            set(g->top, g->w, x, iy + 1, T_CHAIR);
            seat(anchors, x, iy + 1);
        }
        break;
    case F_COFFEE:
        set(g->top, g->w, ix, iy, T_COFFEE);
        for (x = ix + 1; x < ix + iw; x += 2) {
            set(g->top, g->w, x, iy + ih / 2, T_TABLE);
            seat(anchors, x, iy + ih / 2 + 1);
        }
        break;
    case F_SOFAS:
        for (y = iy; y < iy + ih; y += 3)
            for (x = ix; x < ix + iw - 1; x += 4) {
                set(g->top, g->w, x, y, T_SOFA);
                set(g->top, g->w, x + 1, y, T_SOFA);
                seat(anchors, x, y);
                seat(anchors, x + 1, y);
                set(g->top, g->w, x, y + 1, T_TABLE);
            }
        set(g->top, g->w, ix + iw - 1, iy, T_PLANT);
        break;
    case F_BENCHES:
        break;
    case F_VISITOR:
        for (x = ix; x < ix + iw; x += 3) {
            set(g->top, g->w, x, iy, T_DESK);
            seat(anchors, x, iy + 1);
        }
        break;
    }
}

static Grid *
lay_out(const World *world)
{
    Grid *g = g_new0(Grid, 1);
    gsize i;
    gint x, y;

    g->w = world->width;
    g->h = world->height;
    g->floor = g_new0(guint16, g->w * g->h);
    g->top = g_new0(guint16, g->w * g->h);
    g->anchors = g_array_new(FALSE, TRUE, sizeof(GArray *));

    for (i = 0; i < (gsize)g->w * g->h; i++)
        g->floor[i] = (guint16)world->ground;

    /* the town's paths and greenery, drawn before the buildings */
    if (world->town) {
        for (x = 0; x < g->w; x++)
            for (y = 21; y < 23; y++)
                set(g->floor, g->w, x, y, T_PATH);
        for (y = 0; y < g->h; y++)
            for (x = 12; x < 14; x++)
                set(g->floor, g->w, x, y, T_PATH);
        for (y = 0; y < g->h; y++)
            for (x = 38; x < 40; x++)
                set(g->floor, g->w, x, y, T_PATH);
        for (x = 0; x < g->w; x += 5)
            set(g->top, g->w, x, 0, T_TREE);
        for (y = 33; y < 40; y++)
            for (x = 46; x < 52; x++)
                set(g->floor, g->w, x, y, (x + y) % 7 == 0 ? T_FLOWERS : T_WATER);
        set(g->top, g->w, 12, 22, T_LAMP);
        set(g->top, g->w, 39, 22, T_LAMP);
    }

    for (i = 0; i < world->n_rooms; i++) {
        const Room *r = &world->rooms[i];
        GArray *anchors = g_array_new(FALSE, FALSE, sizeof(gdouble));

        for (y = r->y; y < r->y + r->h; y++)
            for (x = r->x; x < r->x + r->w; x++) {
                gboolean edge = x == r->x || y == r->y || x == r->x + r->w - 1 ||
                                y == r->y + r->h - 1;

                set(g->floor, g->w, x, y, r->floor);
                if (edge && r->wall != T_EMPTY)
                    set(g->top, g->w, x, y, r->wall);
            }
        /* a doorway in the bottom wall, two tiles wide */
        if (r->wall != T_EMPTY) {
            set(g->top, g->w, r->x + r->w / 2, r->y + r->h - 1, T_EMPTY);
            set(g->top, g->w, r->x + r->w / 2 - 1, r->y + r->h - 1, T_EMPTY);
        }
        furnish(g, r, anchors);
        g_array_append_val(g->anchors, anchors);
    }
    return g;
}

static void
grid_free(Grid *g)
{
    guint i;

    for (i = 0; i < g->anchors->len; i++)
        g_array_unref(g_array_index(g->anchors, GArray *, i));
    g_array_unref(g->anchors);
    g_free(g->floor);
    g_free(g->top);
    g_free(g);
}

/* ── Writers ────────────────────────────────────────────────────── */

static void
add_layer(JsonBuilder *b, const gchar *name, gint id, guint16 *data, gint w, gint h)
{
    gint i;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, id);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, name);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "tilelayer");
    json_builder_set_member_name(b, "x");
    json_builder_add_int_value(b, 0);
    json_builder_set_member_name(b, "y");
    json_builder_add_int_value(b, 0);
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, w);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, h);
    json_builder_set_member_name(b, "opacity");
    json_builder_add_int_value(b, 1);
    json_builder_set_member_name(b, "visible");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "data");
    json_builder_begin_array(b);
    for (i = 0; i < w * h; i++)
        json_builder_add_int_value(b, data[i]);   /* firstgid 1: tile t is gid t */
    json_builder_end_array(b);
    json_builder_end_object(b);
}

static gboolean
write_tmj(const World *world, Grid *g, const gchar *path, GError **error)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    gsize i;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "map");
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, "1.10");
    json_builder_set_member_name(b, "orientation");
    json_builder_add_string_value(b, "orthogonal");
    json_builder_set_member_name(b, "renderorder");
    json_builder_add_string_value(b, "right-down");
    json_builder_set_member_name(b, "infinite");
    json_builder_add_boolean_value(b, FALSE);
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, g->w);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, g->h);
    json_builder_set_member_name(b, "tilewidth");
    json_builder_add_int_value(b, TILE);
    json_builder_set_member_name(b, "tileheight");
    json_builder_add_int_value(b, TILE);
    json_builder_set_member_name(b, "nextlayerid");
    json_builder_add_int_value(b, 4);
    json_builder_set_member_name(b, "nextobjectid");
    json_builder_add_int_value(b, (gint)world->n_rooms + 1);

    json_builder_set_member_name(b, "tilesets");
    json_builder_begin_array(b);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "firstgid");
    json_builder_add_int_value(b, 1);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "tiles");
    json_builder_set_member_name(b, "image");
    json_builder_add_string_value(b, "tiles.png");
    json_builder_set_member_name(b, "imagewidth");
    json_builder_add_int_value(b, 8 * TILE);
    json_builder_set_member_name(b, "imageheight");
    json_builder_add_int_value(b, ((T_COUNT + 7) / 8) * TILE);
    json_builder_set_member_name(b, "tilewidth");
    json_builder_add_int_value(b, TILE);
    json_builder_set_member_name(b, "tileheight");
    json_builder_add_int_value(b, TILE);
    json_builder_set_member_name(b, "tilecount");
    json_builder_add_int_value(b, T_COUNT - 1);
    json_builder_set_member_name(b, "columns");
    json_builder_add_int_value(b, 8);
    json_builder_set_member_name(b, "margin");
    json_builder_add_int_value(b, 0);
    json_builder_set_member_name(b, "spacing");
    json_builder_add_int_value(b, 0);
    json_builder_end_object(b);
    json_builder_end_array(b);

    json_builder_set_member_name(b, "layers");
    json_builder_begin_array(b);
    add_layer(b, "floor", 1, g->floor, g->w, g->h);
    add_layer(b, "furniture", 2, g->top, g->w, g->h);

    /* the zones, as an object layer a Tiled user can see and edit */
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, 3);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "zones");
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "objectgroup");
    json_builder_set_member_name(b, "visible");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "opacity");
    json_builder_add_int_value(b, 1);
    json_builder_set_member_name(b, "x");
    json_builder_add_int_value(b, 0);
    json_builder_set_member_name(b, "y");
    json_builder_add_int_value(b, 0);
    json_builder_set_member_name(b, "objects");
    json_builder_begin_array(b);
    for (i = 0; i < world->n_rooms; i++) {
        const Room *r = &world->rooms[i];

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id");
        json_builder_add_int_value(b, (gint)i + 1);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, r->id);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, r->tags);
        json_builder_set_member_name(b, "x");
        json_builder_add_int_value(b, r->x * TILE);
        json_builder_set_member_name(b, "y");
        json_builder_add_int_value(b, r->y * TILE);
        json_builder_set_member_name(b, "width");
        json_builder_add_int_value(b, r->w * TILE);
        json_builder_set_member_name(b, "height");
        json_builder_add_int_value(b, r->h * TILE);
        json_builder_set_member_name(b, "rotation");
        json_builder_add_int_value(b, 0);
        json_builder_set_member_name(b, "visible");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    json_builder_end_array(b);
    json_builder_end_object(b);

    root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    return json_generator_to_file(gen, path, error);
}

static void
color3(GString *out, guint hex)
{
    g_string_append_printf(out, "[%.3f, %.3f, %.3f, 1.0]",
                           ((hex >> 16) & 0xff) / 255.0, ((hex >> 8) & 0xff) / 255.0,
                           (hex & 0xff) / 255.0);
}

static guint node_serial;

static void
node(GString *out, const gchar *name, gdouble x, gdouble y, gdouble z,
     gdouble sx, gdouble sy, gdouble sz, const gchar *primitive, guint hex)
{
    node_serial++;
    g_string_append_printf(out,
        "  - name: %s\n"
        "    guid: 0ff1ce00-0000-4000-8000-%012x\n"
        "    transform:\n"
        "      location: [%.2f, %.2f, %.2f]\n"
        "      scale: [%.2f, %.2f, %.2f]\n"
        "    visual:\n"
        "      kind: primitive\n"
        "      primitive: %s\n"
        "      material:\n"
        "        color: ",
        name, node_serial, x, y, z, sx, sy, sz, primitive);
    color3(out, hex);
    g_string_append(out, "\n        roughness: 0.8\n");
}

/* One primitive per piece of furniture: the 3D level mirrors the 2D map. */
static gboolean
write_rlevel(const World *world, Grid *g, const gchar *path, GError **error)
{
    GString *out = g_string_new(NULL);
    gint x, y;
    gsize i;
    gboolean ok;

    node_serial = 0;
    g_string_append_printf(out,
        "# %s.rlevel - generated by tools/clawt-office-genworld.c; CC0-1.0\n"
        "type: level\nname: %s\ndefault_view: 3d\nnodes:\n", world->id, world->id);
    node_serial++;
    g_string_append_printf(out,
        "  - name: Sun\n    guid: 0ff1ce00-0000-4000-8000-%012x\n"
        "    transform:\n      location: [%.1f, 30.0, %.1f]\n      rotation: [-0.9, 0.6, 0.0]\n"
        "    visual:\n      kind: light\n      material:\n        color: [1.0, 0.96, 0.88, 1.0]\n"
        "      params:\n        light_type: directional\n        intensity: 1.4\n",
        node_serial, g->w / 2.0, g->h / 2.0);
    node(out, "Ground", g->w / 2.0, 0.0, g->h / 2.0, g->w, 1.0, g->h, "plane",
         world->ground_color);

    for (i = 0; i < world->n_rooms; i++) {
        const Room *r = &world->rooms[i];
        g_autofree gchar *name = g_strdup_printf("Floor-%s", r->id);

        node(out, name, r->x + r->w / 2.0, 0.01, r->y + r->h / 2.0, r->w, 1.0, r->h,
             "plane", r->color);
    }

    for (y = 0; y < g->h; y++)
        for (x = 0; x < g->w; x++) {
            Tile t = (Tile)g->top[y * g->w + x];
            g_autofree gchar *name = g_strdup_printf("T%d-%d", x, y);
            gdouble cx = x + 0.5, cz = y + 0.5;

            switch (t) {
            case T_WALL:
                node(out, name, cx, 0.6, cz, 1.0, 1.2, 1.0, "cube", 0x4c566a);
                break;
            case T_GLASS:
                node(out, name, cx, 0.6, cz, 1.0, 1.2, 1.0, "cube", 0xa7d3e3);
                break;
            case T_DESK:
            case T_TABLE:
            case T_LAB:
                node(out, name, cx, 0.38, cz, 0.95, 0.06, 0.8, "cube",
                     t == T_LAB ? 0xeceff4 : 0xd8c3a5);
                break;
            case T_CHAIR:
                node(out, name, cx, 0.22, cz, 0.45, 0.44, 0.45, "cylinder", 0x5e81ac);
                break;
            case T_SOFA:
                node(out, name, cx, 0.25, cz, 0.95, 0.5, 0.7, "cube", 0xbf616a);
                break;
            case T_PLANT:
            case T_TREE:
                node(out, name, cx, 0.6, cz, 0.6, 1.2, 0.6, "sphere",
                     t == T_TREE ? 0x3f7d4a : 0x6fb36f);
                break;
            case T_SHELF:
                node(out, name, cx, 0.9, cz, 1.0, 1.8, 0.4, "cube", 0x6b4f36);
                break;
            case T_WHITEBOARD:
            case T_SCREEN:
                node(out, name, cx, 1.0, cz, 1.0, 1.0, 0.08, "cube",
                     t == T_SCREEN ? 0x2e3440 : 0xeceff4);
                break;
            case T_SERVER:
                node(out, name, cx, 0.9, cz, 0.8, 1.8, 0.8, "cube", 0x2e3440);
                break;
            case T_BOOTH:
                /*
                 * An open shell -- back, two sides and a roof, open to the
                 * room -- not a solid block: the seat is inside it, and a
                 * block hid whoever was on the phone from every camera.
                 */
                {
                    g_autofree gchar *back = g_strconcat(name, "-back", NULL);
                    g_autofree gchar *left = g_strconcat(name, "-left", NULL);
                    g_autofree gchar *right = g_strconcat(name, "-right", NULL);
                    g_autofree gchar *roof = g_strconcat(name, "-roof", NULL);

                    /* taller than a person (the figures stand about 2.5) */
                    node(out, back, cx, 1.4, cz - 0.42, 0.9, 2.8, 0.06, "cube", 0x88c0d0);
                    node(out, left, cx - 0.42, 1.4, cz, 0.06, 2.8, 0.9, "cube", 0x88c0d0);
                    node(out, right, cx + 0.42, 1.4, cz, 0.06, 2.8, 0.9, "cube", 0x88c0d0);
                    node(out, roof, cx, 2.83, cz, 0.9, 0.06, 0.9, "cube", 0x6f9fb0);
                }
                break;
            case T_MONITOR:
                node(out, name, cx, 0.6, cz, 0.7, 0.4, 0.08, "cube", 0x4c566a);
                break;
            case T_COFFEE:
                node(out, name, cx, 0.5, cz, 0.6, 1.0, 0.6, "cube", 0x2e3440);
                break;
            case T_BENCH:
                node(out, name, cx, 0.2, cz, 0.95, 0.15, 0.4, "cube", 0x8f6a4a);
                break;
            case T_LAMP:
                node(out, name, cx, 1.2, cz, 0.2, 2.4, 0.2, "cylinder", 0xebcb8b);
                break;
            case T_PODIUM:
                node(out, name, cx, 0.5, cz, 0.6, 1.0, 0.5, "cube", 0x6b4f36);
                break;
            case T_DOOR:
                node(out, name, cx, 0.02, cz, 1.0, 1.0, 1.0, "plane", 0xa3be8c);
                break;
            case T_EMPTY:
            case T_CARPET:
            case T_WOOD:
            case T_TILE:
            case T_GRASS:
            case T_PATH:
            case T_WATER:
            case T_FLOWERS:
            case T_RUG_RED:
            case T_STONE:
            case T_HEDGE:
            case T_COUNT:
                break;
            }
        }

    ok = g_file_set_contents(path, out->str, -1, error);
    g_string_free(out, TRUE);
    return ok;
}

static gchar *
sha256_of(const gchar *path)
{
    g_autofree gchar *data = NULL;
    gsize length = 0;

    if (!g_file_get_contents(path, &data, &length, NULL))
        return g_strdup("missing");
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)data, length);
}

static gboolean
write_manifest(const World *world, Grid *g, const gchar *dir, GError **error)
{
    GString *out = g_string_new(NULL);
    const gchar *files[] = { "tiles.png", "map.tmj", "office.rlevel" };
    gsize i;
    gboolean ok;

    g_string_append_printf(out,
        "# %s - generated by tools/clawt-office-genworld.c (`make office-worlds`).\n"
        "# Every asset here was drawn by that program, so the whole pack is CC0.\n"
        "name: %s\ndescription: \"%s\"\nlicense: CC0-1.0\n"
        "renderers:\n  2d:\n    map: map.tmj\n  3d:\n    level: office.rlevel\n"
        "zones:\n", world->id, world->name, world->description);

    for (i = 0; i < world->n_rooms; i++) {
        const Room *r = &world->rooms[i];
        GArray *anchors = g_array_index(g->anchors, GArray *, i);
        g_auto(GStrv) tags = g_strsplit(r->tags, ",", -1);
        guint a;
        gsize t;

        g_string_append_printf(out, "  - id: %s\n    name: \"%s\"\n    tags: [", r->id, r->name);
        for (t = 0; tags[t] != NULL; t++)
            g_string_append_printf(out, "%s\"%s\"", t > 0 ? ", " : "", tags[t]);
        g_string_append_printf(out, "]\n    capacity: %u\n    rect_2d: [%d, %d, %d, %d]\n",
                               anchors->len / 2, r->x * TILE, r->y * TILE,
                               r->w * TILE, r->h * TILE);
        g_string_append(out, "    anchors_2d:\n");
        for (a = 0; a + 1 < anchors->len; a += 2)
            g_string_append_printf(out, "      - [%.0f, %.0f]\n",
                                   g_array_index(anchors, gdouble, a) * TILE,
                                   g_array_index(anchors, gdouble, a + 1) * TILE);
        g_string_append(out, "    anchors_3d:\n");
        for (a = 0; a + 1 < anchors->len; a += 2)
            g_string_append_printf(out, "      - [%.2f, 0.0, %.2f]\n",
                                   g_array_index(anchors, gdouble, a),
                                   g_array_index(anchors, gdouble, a + 1));
        if (anchors->len == 0)
            g_string_append_printf(out, "      - [%.2f, 0.0, %.2f]\n",
                                   r->x + r->w / 2.0, r->y + r->h / 2.0);
    }

    /* tile names by gid (firstgid 1), for a restyle to know what is what */
    g_string_append(out, "tiles:\n");
    {
        gint t;

        for (t = 1; t < T_COUNT; t++)
            g_string_append_printf(out, "  - \"%s\"\n", tile_names[t]);
    }

    g_string_append(out, "assets:\n");
    for (i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree gchar *path = g_build_filename(dir, files[i], NULL);
        g_autofree gchar *sum = sha256_of(path);

        g_string_append_printf(out,
            "  - { path: %s, sha256: %s, source: \"tools/clawt-office-genworld.c\" }\n",
            files[i], sum);
    }

    {
        g_autofree gchar *path = g_build_filename(dir, "office.yaml", NULL);

        ok = g_file_set_contents(path, out->str, -1, error);
    }
    g_string_free(out, TRUE);
    return ok;
}

gint
main(gint argc, gchar *argv[])
{
    gsize w;

    if (argc != 2 || g_strcmp0(argv[1], "-h") == 0 || g_strcmp0(argv[1], "--help") == 0) {
        g_print("Usage: clawt-office-genworld <output-dir>\n\n"
                "Writes the office world packs clawtilla ships (modern-office,\n"
                "cozy-town), one directory each, every asset drawn here: CC0.\n\n"
                "Example:\n  clawt-office-genworld data/office/worlds\n");
        return argc == 2 ? 0 : 1;
    }
    if (g_strcmp0(argv[1], "--license") == 0) {
        g_print("clawt-office-genworld is AGPL-3.0-or-later; its output is CC0-1.0.\n");
        return 0;
    }

    for (w = 0; w < G_N_ELEMENTS(worlds); w++) {
        const World *world = &worlds[w];
        g_autofree gchar *dir = g_build_filename(argv[1], world->id, NULL);
        g_autofree gchar *tiles = g_build_filename(dir, "tiles.png", NULL);
        g_autofree gchar *map = g_build_filename(dir, "map.tmj", NULL);
        g_autofree gchar *level = g_build_filename(dir, "office.rlevel", NULL);
        g_autoptr(GError) error = NULL;
        Grid *g;

        g_mkdir_with_parents(dir, 0755);
        g = lay_out(world);
        if (!write_tileset(tiles) || !write_tmj(world, g, map, &error) ||
            !write_rlevel(world, g, level, &error) ||
            !write_manifest(world, g, dir, &error)) {
            g_printerr("clawt-office-genworld: %s: %s\n", world->id,
                       error != NULL ? error->message : "could not write the tileset");
            grid_free(g);
            return 1;
        }
        grid_free(g);
        g_print("wrote %s\n", dir);
    }
    return 0;
}
