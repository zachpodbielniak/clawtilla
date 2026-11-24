/*
 * clawt-office-restyle.c - A world pack redrawn in another style: the pure half
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `clawtilla office world restyle BASE NAME --style "..."` makes a new
 * pack with the base's layout -- the same map, zones and anchors -- and
 * new art: each tile kind the base uses redrawn by PixelLab into its own
 * slot of the tile sheet, and each piece of furniture the 3D level draws
 * as one node replaced by a model Meshy made.  Nothing here touches the
 * network or the daemon: what kinds there are, the sheet, the level and
 * the manifest are worked out from the base pack's text, so each is
 * tested on its own.  The paid half is core/daemon-office-restyle.c.
 *
 * What a node is comes from the map, not the level: the shipped levels
 * name a tile's node `T<x>-<y>`, its cell, and the map says what is in
 * that cell.  The names come from the manifest's `tiles:` list, which
 * tools/clawt-office-genworld.c writes beside the enum they come from.
 */

#include "clawtilla.h"

#include <yaml-glib.h>
#include <stdlib.h>
#include <string.h>

#ifdef CLAWT_HAVE_POPPLER
#include <cairo.h>
#endif

/* Tiled keeps flip flags in a gid's top bits. */
#define GID_MASK (0x1FFFFFFFu)

/* ── Reading the base ────────────────────────────────────────────── */

static JsonNode *
yaml_to_json(
	const gchar  *text,
	GError      **error
){
	g_autoptr(YamlParser) parser = yaml_parser_new();
	YamlNode *root;

	if (text == NULL || !yaml_parser_load_from_data(parser, text, -1, error))
		return NULL;
	root = yaml_parser_get_root(parser);
	if (root == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the document is empty");
		return NULL;
	}
	return yaml_node_to_json_node(root);
}

static gchar *
json_to_yaml(
	JsonNode  *node,
	GError   **error
){
	g_autoptr(YamlGenerator) generator = yaml_generator_new();
	g_autoptr(YamlNode) root = yaml_node_from_json_node(node);

	yaml_generator_set_root(generator, root);
	return yaml_generator_to_data(generator, NULL, error);
}

/* The map's tile layers in order (floor first), with its size. */
typedef struct {
	gint      width;
	gint      height;
	JsonArray *floor;      /* borrowed */
	JsonArray *furniture;  /* borrowed, or NULL */
} Map;

static gboolean
read_map(
	JsonNode  *tmj,
	Map       *map,
	GError   **error
){
	JsonObject *root = JSON_NODE_HOLDS_OBJECT(tmj) ? json_node_get_object(tmj) : NULL;
	JsonArray *layers;
	guint i;

	memset(map, 0, sizeof(*map));
	if (root == NULL || !json_object_has_member(root, "layers")) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the map has no layers");
		return FALSE;
	}

	map->width = (gint)json_object_get_int_member_with_default(root, "width", 0);
	map->height = (gint)json_object_get_int_member_with_default(root, "height", 0);
	layers = json_object_get_array_member(root, "layers");

	for (i = 0; i < json_array_get_length(layers); i++) {
		JsonObject *layer = json_array_get_object_element(layers, i);

		if (g_strcmp0(json_object_get_string_member_with_default(layer, "type", ""),
		              "tilelayer") != 0 ||
		    !json_object_has_member(layer, "data"))
			continue;
		if (map->floor == NULL)
			map->floor = json_object_get_array_member(layer, "data");
		else if (map->furniture == NULL)
			map->furniture = json_object_get_array_member(layer, "data");
	}

	if (map->floor == NULL || map->width <= 0 || map->height <= 0 ||
	    json_array_get_length(map->floor) < (guint)(map->width * map->height)) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the map's tile layers do not match its size");
		return FALSE;
	}
	return TRUE;
}

static guint
gid_at(
	JsonArray *layer,
	gint       index
){
	if (layer == NULL || index < 0 || (guint)index >= json_array_get_length(layer))
		return 0;
	return (guint)json_array_get_int_element(layer, (guint)index) & GID_MASK;
}

/* `T<x>-<y>` exactly: the cell, or FALSE for any other name. */
static gboolean
parse_cell(
	const gchar *name,
	gint        *x,
	gint        *y
){
	gchar *end = NULL;
	glong a;
	glong b;

	if (name == NULL || name[0] != 'T' || !g_ascii_isdigit(name[1]))
		return FALSE;
	a = strtol(name + 1, &end, 10);
	if (end == NULL || *end != '-' || !g_ascii_isdigit(end[1]))
		return FALSE;
	b = strtol(end + 1, &end, 10);
	if (end == NULL || *end != '\0')
		return FALSE;
	*x = (gint)a;
	*y = (gint)b;
	return TRUE;
}

/* The gid drawn at a cell: the furniture on it, else its floor. */
static guint
gid_for_cell(
	const Map *map,
	gint       x,
	gint       y
){
	gint index = y * map->width + x;
	guint top = gid_at(map->furniture, index);

	return top != 0 ? top : gid_at(map->floor, index);
}

/* The level's nodes array, or NULL. */
static JsonArray *
level_nodes(
	JsonNode *level
){
	JsonObject *root = JSON_NODE_HOLDS_OBJECT(level) ? json_node_get_object(level) : NULL;

	if (root == NULL || !json_object_has_member(root, "nodes") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "nodes")))
		return NULL;
	return json_object_get_array_member(root, "nodes");
}

/*
 * Kinds that are the building rather than what is in it.  The level
 * draws each wall cell as its own node, so a model would be repeated
 * cell by cell into a wall of separate objects; these keep their
 * primitives and are redrawn only as tiles.
 */
static gboolean
is_structure(
	const gchar *name
){
	static const gchar *const structure[] = { "wall", "glass", "door", NULL };
	guint i;

	for (i = 0; structure[i] != NULL; i++)
		if (g_strcmp0(name, structure[i]) == 0)
			return TRUE;
	return FALSE;
}

JsonArray *
clawt_office_restyle_kinds(
	const gchar  *manifest_yaml,
	const gchar  *tmj_json,
	const gchar  *rlevel_yaml,
	GError      **error
){
	g_autoptr(JsonNode) manifest = yaml_to_json(manifest_yaml, error);
	g_autoptr(JsonNode) tmj = NULL;
	g_autoptr(JsonNode) level = NULL;
	g_autoptr(GHashTable) floor = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GHashTable) top = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GHashTable) modelled = g_hash_table_new(g_direct_hash, g_direct_equal);
	JsonArray *names;
	JsonArray *out;
	JsonArray *nodes;
	Map map;
	guint i;

	if (manifest == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_OBJECT(manifest) ||
	    !json_object_has_member(json_node_get_object(manifest), "tiles")) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the pack's manifest names no tiles (`tiles:`), so there "
		                    "is nothing to say what to draw; a pack made by "
		                    "`make office-worlds` since 0.77.0 has them");
		return NULL;
	}
	names = json_object_get_array_member(json_node_get_object(manifest), "tiles");

	tmj = json_from_string(tmj_json, error);
	if (tmj == NULL || !read_map(tmj, &map, error))
		return NULL;

	for (i = 0; i < (guint)(map.width * map.height); i++) {
		guint f = gid_at(map.floor, (gint)i);
		guint t = gid_at(map.furniture, (gint)i);

		if (f != 0)
			g_hash_table_add(floor, GUINT_TO_POINTER(f));
		if (t != 0)
			g_hash_table_add(top, GUINT_TO_POINTER(t));
	}

	/* which kinds the level draws as one node a model can stand in for */
	if (rlevel_yaml != NULL) {
		level = yaml_to_json(rlevel_yaml, NULL);
		nodes = level != NULL ? level_nodes(level) : NULL;
		for (i = 0; nodes != NULL && i < json_array_get_length(nodes); i++) {
			JsonObject *node = json_array_get_object_element(nodes, i);
			gint x = 0;
			gint y = 0;

			if (node != NULL &&
			    parse_cell(json_object_get_string_member_with_default(node, "name",
			                                                          NULL), &x, &y) &&
			    x < map.width && y < map.height)
				g_hash_table_add(modelled,
				                 GUINT_TO_POINTER(gid_at(map.furniture,
				                                         y * map.width + x)));
		}
	}

	out = json_array_new();
	for (i = 0; i < json_array_get_length(names); i++) {
		guint gid = i + 1;
		gboolean on_floor = g_hash_table_contains(floor, GUINT_TO_POINTER(gid));
		gboolean on_top = g_hash_table_contains(top, GUINT_TO_POINTER(gid));
		JsonObject *kind;

		if (!on_floor && !on_top)
			continue;

		kind = json_object_new();
		json_object_set_int_member(kind, "gid", gid);
		json_object_set_string_member(kind, "name",
		                              json_array_get_string_element(names, i));
		json_object_set_boolean_member(kind, "floor", on_floor && !on_top);
		json_object_set_boolean_member(kind, "model",
		                               on_top && g_hash_table_contains(modelled,
		                                   GUINT_TO_POINTER(gid)) &&
		                               !is_structure(json_array_get_string_element(names,
		                                                                           i)));
		json_array_add_object_element(out, kind);
	}

	return out;
}

gchar *
clawt_office_restyle_prompt(
	JsonObject  *kind,
	const gchar *style,
	gboolean     three_d
){
	const gchar *name = json_object_get_string_member_with_default(kind, "name", "thing");
	const gchar *look = style != NULL && *style != '\0' ? style : "cosy and friendly";

	if (three_d)
		return g_strdup_printf("A %s for an office, as one piece of furniture, "
		                       "%s, simple game-ready shapes", name, look);
	if (json_object_get_boolean_member_with_default(kind, "floor", FALSE))
		return g_strdup_printf("A seamless %s floor tile seen from directly above, "
		                       "for a top-down pixel-art office game, %s", name, look);
	return g_strdup_printf("A %s seen from above, for a top-down pixel-art office "
	                       "game, %s", name, look);
}

/* ── The sheet ───────────────────────────────────────────────────── */

#ifdef CLAWT_HAVE_POPPLER
typedef struct {
	const guchar *data;
	gsize         length;
	gsize         at;
} Reader;

static cairo_status_t
read_bytes(
	void          *closure,
	unsigned char *out,
	unsigned int   length
){
	Reader *reader = closure;

	if (reader->at + length > reader->length)
		return CAIRO_STATUS_READ_ERROR;
	memcpy(out, reader->data + reader->at, length);
	reader->at += length;
	return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
write_bytes(
	void                *closure,
	const unsigned char *data,
	unsigned int         length
){
	g_byte_array_append(closure, data, length);
	return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *
surface_from(
	GBytes *png
){
	Reader reader = { NULL, 0, 0 };
	cairo_surface_t *surface;

	reader.data = g_bytes_get_data(png, &reader.length);
	surface = cairo_image_surface_create_from_png_stream(read_bytes, &reader);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	return surface;
}
#endif

GBytes *
clawt_office_restyle_tiles(
	GBytes      *base_png,
	guint        tile,
	guint        columns,
	GHashTable  *drawn,
	GError     **error
){
#ifdef CLAWT_HAVE_POPPLER
	cairo_surface_t *base;
	cairo_surface_t *sheet;
	cairo_t *cr;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	GByteArray *out;
	gint w;
	gint h;

	g_return_val_if_fail(base_png != NULL && drawn != NULL, NULL);

	base = surface_from(base_png);
	if (base == NULL || tile == 0 || columns == 0) {
		if (base != NULL)
			cairo_surface_destroy(base);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the base pack's tile sheet is not a PNG");
		return NULL;
	}

	/* a copy as large as the base, so its other slots are kept as they are */
	w = cairo_image_surface_get_width(base);
	h = cairo_image_surface_get_height(base);
	sheet = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cr = cairo_create(sheet);
	cairo_set_source_surface(cr, base, 0, 0);
	cairo_paint(cr);
	cairo_surface_destroy(base);

	g_hash_table_iter_init(&iter, drawn);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		guint gid = GPOINTER_TO_UINT(key);
		cairo_surface_t *art = surface_from(value);
		gdouble x = (gdouble)(((gid - 1) % columns) * tile);
		gdouble y = (gdouble)(((gid - 1) / columns) * tile);

		if (gid == 0 || art == NULL) {
			if (art != NULL)
				cairo_surface_destroy(art);
			continue;
		}

		/* the slot cleared, then the drawing scaled into it */
		cairo_save(cr);
		cairo_rectangle(cr, x, y, tile, tile);
		cairo_clip(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_translate(cr, x, y);
		cairo_scale(cr, (gdouble)tile / cairo_image_surface_get_width(art),
		            (gdouble)tile / cairo_image_surface_get_height(art));
		cairo_set_source_surface(cr, art, 0, 0);
		/* pixel art stays pixels when it is scaled */
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_surface_destroy(art);
	}

	cairo_destroy(cr);
	out = g_byte_array_new();
	if (cairo_surface_write_to_png_stream(sheet, write_bytes, out) !=
	    CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(sheet);
		g_byte_array_unref(out);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		                    "the tile sheet could not be written");
		return NULL;
	}
	cairo_surface_destroy(sheet);
	return g_byte_array_free_to_bytes(out);
#else
	(void)base_png;
	(void)tile;
	(void)columns;
	(void)drawn;
	g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
	                    "this daemon was built without cairo (POPPLER=1), so it "
	                    "cannot redraw a tile sheet");
	return NULL;
#endif
}

/* ── The level ───────────────────────────────────────────────────── */

gchar *
clawt_office_restyle_level(
	const gchar  *rlevel_yaml,
	const gchar  *tmj_json,
	JsonObject   *models,
	GError      **error
){
	g_autoptr(JsonNode) level = yaml_to_json(rlevel_yaml, error);
	g_autoptr(JsonNode) tmj = NULL;
	JsonArray *nodes;
	Map map;
	guint i;

	if (level == NULL)
		return NULL;
	tmj = json_from_string(tmj_json, error);
	if (tmj == NULL || !read_map(tmj, &map, error))
		return NULL;

	nodes = level_nodes(level);
	for (i = 0; nodes != NULL && i < json_array_get_length(nodes); i++) {
		JsonObject *node = json_array_get_object_element(nodes, i);
		g_autofree gchar *gid = NULL;
		const gchar *asset;
		JsonObject *visual;
		JsonObject *transform;
		gint x = 0;
		gint y = 0;

		if (node == NULL ||
		    !parse_cell(json_object_get_string_member_with_default(node, "name", NULL),
		                &x, &y) ||
		    x >= map.width || y >= map.height)
			continue;

		gid = g_strdup_printf("%u", gid_for_cell(&map, x, y));
		asset = json_object_get_string_member_with_default(models, gid, NULL);
		if (asset == NULL)
			continue;

		visual = json_object_new();
		json_object_set_string_member(visual, "kind", "mesh-asset");
		json_object_set_string_member(visual, "asset", asset);
		json_object_set_object_member(node, "visual", visual);

		/* on the floor, at its own size: the model was made to scale */
		if (json_object_has_member(node, "transform")) {
			JsonArray *location;
			JsonArray *scale = json_array_new();

			transform = json_object_get_object_member(node, "transform");
			if (json_object_has_member(transform, "location")) {
				location = json_object_get_array_member(transform, "location");
				if (json_array_get_length(location) == 3) {
					JsonArray *grounded = json_array_new();

					json_array_add_double_element(grounded,
						json_array_get_double_element(location, 0));
					json_array_add_double_element(grounded, 0.0);
					json_array_add_double_element(grounded,
						json_array_get_double_element(location, 2));
					json_object_set_array_member(transform, "location", grounded);
				}
			}
			json_array_add_double_element(scale, 1.0);
			json_array_add_double_element(scale, 1.0);
			json_array_add_double_element(scale, 1.0);
			json_object_set_array_member(transform, "scale", scale);
		}
	}

	return json_to_yaml(level, error);
}

/* ── The manifest ────────────────────────────────────────────────── */

gchar *
clawt_office_restyle_manifest(
	const gchar  *manifest_yaml,
	const gchar  *name,
	const gchar  *style,
	JsonArray    *assets,
	GError      **error
){
	g_autoptr(JsonNode) manifest = yaml_to_json(manifest_yaml, error);
	g_autofree gchar *description = NULL;
	JsonObject *root;

	if (manifest == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_OBJECT(manifest)) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the base manifest is not a mapping");
		return NULL;
	}
	root = json_node_get_object(manifest);

	description = g_strdup_printf("%s, redrawn: %s",
	                              json_object_get_string_member_with_default(root, "name",
	                                                                         "a pack"),
	                              style != NULL ? style : "");
	json_object_set_string_member(root, "name", name);
	json_object_set_string_member(root, "description", description);
	/* never the base's CC0: the new art is the services', on their terms */
	json_object_set_string_member(root, "license",
	                              "LicenseRef-generated: art drawn by PixelLab and Meshy "
	                              "for clawtilla, on their terms (see provenance.yaml); "
	                              "layout from the base pack");
	json_object_set_array_member(root, "assets", json_array_ref(assets));

	return json_to_yaml(manifest, error);
}
