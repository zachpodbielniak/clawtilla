/*
 * clawt-office-restyle.h - A world pack redrawn in another style: the pure half
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_OFFICE_RESTYLE_H
#define CLAWT_OFFICE_RESTYLE_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * clawt_office_restyle_kinds:
 * @manifest_yaml: the base pack's `office.yaml`, which names its tiles
 *   (`tiles:`, by gid from 1)
 * @tmj_json: the base pack's 2D map
 * @rlevel_yaml: (nullable): the base pack's 3D level
 * @error: (out) (optional): where a refusal is written
 *
 * The tile kinds a pack actually uses, each once: `{gid, name, floor,
 * model}`.  `floor` says it is on the map's floor layer (drawn edge to
 * edge, no transparency); `model` that the 3D level draws it as one node
 * a model can stand in for, and that it is furniture rather than the
 * building (walls, glass and doors are redrawn as tiles only).  A pack whose manifest names no tiles is
 * refused: without names there is nothing to ask a service to draw.
 *
 * Returns: (transfer full) (nullable): the kinds
 */
JsonArray *clawt_office_restyle_kinds(const gchar  *manifest_yaml,
                                      const gchar  *tmj_json,
                                      const gchar  *rlevel_yaml,
                                      GError      **error);

/**
 * clawt_office_restyle_prompt:
 * @kind: one element of clawt_office_restyle_kinds()
 * @style: the style asked for
 * @three_d: a prompt for a model rather than a tile
 *
 * Returns: (transfer full): what the service is asked to draw
 */
gchar *clawt_office_restyle_prompt(JsonObject  *kind,
                                   const gchar *style,
                                   gboolean     three_d);

/**
 * clawt_office_restyle_tiles:
 * @base_png: the base pack's tile sheet
 * @tile: one tile's size in pixels
 * @columns: tiles a row, as the map's tileset says
 * @drawn: (element-type guint GBytes): gid -> a new PNG for that tile
 * @error: (out) (optional): where a failure is written
 *
 * The base sheet with each drawn tile painted over its own slot -- the
 * same index, so the map that names it needs no change.  A drawing of
 * another size is scaled to the slot.  Needs cairo (POPPLER=1).
 *
 * Returns: (transfer full) (nullable): the new sheet as a PNG
 */
GBytes *clawt_office_restyle_tiles(GBytes      *base_png,
                                   guint        tile,
                                   guint        columns,
                                   GHashTable  *drawn,
                                   GError     **error);

/**
 * clawt_office_restyle_level:
 * @rlevel_yaml: the base pack's 3D level
 * @tmj_json: the base pack's 2D map, which says what each node is
 * @models: gid (as a decimal string) -> a model's path in the new pack
 * @error: (out) (optional): where a failure is written
 *
 * The level with every node `T<x>-<y>` whose map cell is a kind with a
 * model drawn as that model instead: `kind: mesh-asset`, the path, the
 * node's place on the floor and unit scale.  Every other node is left
 * as it was.
 *
 * Returns: (transfer full) (nullable): the new level's YAML
 */
gchar *clawt_office_restyle_level(const gchar  *rlevel_yaml,
                                  const gchar  *tmj_json,
                                  JsonObject   *models,
                                  GError      **error);

/**
 * clawt_office_restyle_manifest:
 * @manifest_yaml: the base pack's `office.yaml`
 * @name: the new pack's name
 * @style: the style it was drawn in, for its description
 * @assets: `{path, sha256, source}` for every file of the new pack
 * @error: (out) (optional): where a failure is written
 *
 * The new pack's manifest: the base's zones, anchors, renderers and
 * tiles; its own name, description and assets; and a licence that says
 * where the new art came from, because a pack without one is refused.
 *
 * Returns: (transfer full) (nullable): the YAML
 */
gchar *clawt_office_restyle_manifest(const gchar  *manifest_yaml,
                                     const gchar  *name,
                                     const gchar  *style,
                                     JsonArray    *assets,
                                     GError      **error);

G_END_DECLS

#endif /* CLAWT_OFFICE_RESTYLE_H */
