/*
 * clawt-office-world.h - Office world packs and the choreography that fills them
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A world pack is a directory with an `office.yaml`: what the place is
 * called, its licence, the 2D map and/or 3D level that draw it, and its
 * *zones* -- named places carrying semantic tags (`desk`, `lounge`,
 * `meeting:8`, `town_hall`, `bug_corner`...).  Nothing in clawtilla
 * knows a pack's geometry; everything asks for "a zone tagged X", so a
 * modern office, a cozy town and a game world are the same to it.
 *
 * The choreography says which zone, pose and prop each activity gets:
 * working at the library station goes to a `library` zone and reads; on a
 * call takes out a phone.  A default is built in; a pack and the person
 * can each override any rule.
 *
 * Everything here is data checked on load: a pack without a licence is
 * refused (it could not be shared), a path that leaves the pack is
 * refused (it could not be synced to another fleet), and an unknown zone
 * tag is a warning rather than a refusal.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "office/clawt-office-presence.h"

G_BEGIN_DECLS

/**
 * ClawtOfficeZone:
 * @id: the zone's id within the pack
 * @name: (nullable): how to show it
 * @tags: (array zero-terminated=1): its tags, e.g. `meeting:8`, `desk`
 * @team: (nullable): the team it belongs to, for `team_area` and desks
 * @capacity: how many fit, 0 for unlimited
 * @anchors_2d: (element-type gdouble): x,y pairs in map pixels
 * @anchors_3d: (element-type gdouble): x,y,z triples in level units
 * @has_rect_2d: whether the pack gave the zone's area on the map
 * @rect_2d: x, y, width and height in map pixels, when @has_rect_2d --
 *   what a zone that draws something (the `usage_board`) is fitted to
 *
 * One place in a world.
 */
typedef struct {
    gchar   *id;
    gchar   *name;
    GStrv    tags;
    gchar   *team;
    guint    capacity;
    GArray  *anchors_2d;
    GArray  *anchors_3d;
    gboolean has_rect_2d;
    gdouble  rect_2d[4];
} ClawtOfficeZone;

/**
 * ClawtOfficeAsset:
 * @path: relative to the pack
 * @sha256: (nullable): its checksum, hex
 * @license: (nullable): the asset's own licence when it differs
 * @source: (nullable): where it came from: a URL, a generator and model
 *
 * One file the pack ships, with its provenance.
 */
typedef struct {
    gchar *path;
    gchar *sha256;
    gchar *license;
    gchar *source;
} ClawtOfficeAsset;

#define CLAWT_TYPE_OFFICE_WORLD (clawt_office_world_get_type())
G_DECLARE_FINAL_TYPE(ClawtOfficeWorld, clawt_office_world, CLAWT, OFFICE_WORLD, GObject)

/**
 * clawt_office_world_load:
 * @directory: the pack's directory
 * @error: return location for why it was refused
 *
 * Reads `office.yaml` from @directory.  Refused when it is missing or
 * not YAML, has no `license`, names no renderer, or names a file outside
 * the pack.
 *
 * Returns: (transfer full) (nullable): the world
 */
ClawtOfficeWorld *clawt_office_world_load(const gchar *directory, GError **error);

/**
 * clawt_office_world_get_id:
 * @self: a world
 *
 * Returns: (transfer none): the pack's directory name
 */
const gchar *clawt_office_world_get_id(ClawtOfficeWorld *self);

/**
 * clawt_office_world_get_name:
 * @self: a world
 *
 * Returns: (transfer none): its display name
 */
const gchar *clawt_office_world_get_name(ClawtOfficeWorld *self);

/**
 * clawt_office_world_get_directory:
 * @self: a world
 *
 * Returns: (transfer none): where it was loaded from
 */
const gchar *clawt_office_world_get_directory(ClawtOfficeWorld *self);

/**
 * clawt_office_world_get_license:
 * @self: a world
 *
 * Returns: (transfer none): its licence, e.g. `CC0-1.0`
 */
const gchar *clawt_office_world_get_license(ClawtOfficeWorld *self);

/**
 * clawt_office_world_get_renderer:
 * @self: a world
 * @dimension: `2d` or `3d`
 *
 * Returns: (transfer full) (nullable): the absolute path of the map or
 *   level for @dimension, or %NULL when the pack has none
 */
gchar *clawt_office_world_get_renderer(ClawtOfficeWorld *self, const gchar *dimension);

/**
 * clawt_office_world_get_zones:
 * @self: a world
 *
 * Returns: (transfer none) (element-type ClawtOfficeZone): every zone
 */
GPtrArray *clawt_office_world_get_zones(ClawtOfficeWorld *self);

/**
 * clawt_office_world_get_assets:
 * @self: a world
 *
 * Returns: (transfer none) (element-type ClawtOfficeAsset): its assets
 */
GPtrArray *clawt_office_world_get_assets(ClawtOfficeWorld *self);

/**
 * clawt_office_world_get_warnings:
 * @self: a world
 *
 * Returns: (transfer none) (element-type utf8): what was accepted but
 *   looked wrong, one line each
 */
GPtrArray *clawt_office_world_get_warnings(ClawtOfficeWorld *self);

/**
 * clawt_office_world_get_adapter:
 * @self: a world
 * @out_source: (out) (optional) (transfer none) (nullable): what the
 *   adapter is asked to load: a file in the pack, or the adapter's own
 *   name for a place in its world
 *
 * The world adapter the pack is drawn with (`renderers.adapter.module`),
 * looked up by clawtilla-office in `office.adapter_dirs` or among the
 * built-in ones.  See docs/office-adapters.org.
 *
 * Returns: (transfer none) (nullable): the adapter's name, or %NULL
 */
const gchar *clawt_office_world_get_adapter(ClawtOfficeWorld *self,
                                            const gchar     **out_source);

/**
 * clawt_office_world_get_choreography_yaml:
 * @self: a world
 *
 * Returns: (transfer none) (nullable): the pack's own choreography
 *   overrides, as YAML, or %NULL when it has none
 */
const gchar *clawt_office_world_get_choreography_yaml(ClawtOfficeWorld *self);

/**
 * clawt_office_world_find_zone:
 * @self: a world
 * @tag: a tag; `meeting` matches `meeting:8`
 * @team: (nullable): prefer a zone of this team
 * @need: how many have to fit, 0 for any
 *
 * The zone a renderer sends somebody to: one carrying @tag, preferring
 * @team's, then the smallest that fits @need.
 *
 * Returns: (transfer none) (nullable): the zone, or %NULL when none has @tag
 */
const ClawtOfficeZone *clawt_office_world_find_zone(ClawtOfficeWorld *self,
                                                    const gchar      *tag,
                                                    const gchar      *team,
                                                    guint             need);

/**
 * clawt_office_world_verify_assets:
 * @self: a world
 * @error: return location for the first asset that does not match
 *
 * Checks every listed asset exists inside the pack and matches its
 * `sha256` -- the check a pack synced from another fleet must pass.
 *
 * Returns: %TRUE when every asset is present and matches
 */
gboolean clawt_office_world_verify_assets(ClawtOfficeWorld *self, GError **error);

/**
 * clawt_office_world_to_json:
 * @self: a world
 *
 * Returns: (transfer full): the pack as `office.world` sends it
 */
JsonNode *clawt_office_world_to_json(ClawtOfficeWorld *self);

/**
 * clawt_office_worlds_discover:
 * @directories: (array zero-terminated=1): where to look, in order
 * @refused: (nullable) (element-type utf8): where to add why a pack was
 *   not loaded
 *
 * Every pack under @directories.  A pack id found twice keeps the first.
 *
 * Returns: (transfer full) (element-type ClawtOfficeWorld): the worlds
 */
GPtrArray *clawt_office_worlds_discover(const gchar *const *directories,
                                        GPtrArray          *refused);

/**
 * clawt_office_zone_tag_is_known:
 * @tag: a tag, with or without its `:argument`
 *
 * Returns: %TRUE for a tag something in clawtilla sends people to
 */
gboolean clawt_office_zone_tag_is_known(const gchar *tag);

/**
 * clawt_office_zone_tag_count:
 *
 * Returns: how many known zone tags there are
 */
guint clawt_office_zone_tag_count(void);

/**
 * clawt_office_zone_tag_nth:
 * @n: an index
 * @out_description: (out) (optional) (transfer none): what it is for
 *
 * Returns: (transfer none) (nullable): the tag, %NULL past the end
 */
const gchar *clawt_office_zone_tag_nth(guint n, const gchar **out_description);

/* ── Choreography ───────────────────────────────────────────────── */

/**
 * ClawtOfficeCue:
 * @zone: the zone tag to go to
 * @pose: `stand`, `sit`, `walk`, `phone`, `present`, `read`, `type`
 * @prop: (nullable): what to hold: `phone`, `folder`, `book`, `laptop`
 * @emote: (nullable): what floats overhead: `question`, `alert`, `zzz`
 *
 * What one activity looks like.
 */
typedef struct {
    gchar *zone;
    gchar *pose;
    gchar *prop;
    gchar *emote;
} ClawtOfficeCue;

#define CLAWT_TYPE_OFFICE_CHOREOGRAPHY (clawt_office_choreography_get_type())
G_DECLARE_FINAL_TYPE(ClawtOfficeChoreography, clawt_office_choreography,
                     CLAWT, OFFICE_CHOREOGRAPHY, GObject)

/**
 * clawt_office_choreography_new_default:
 *
 * Returns: (transfer full): the built-in rules
 */
ClawtOfficeChoreography *clawt_office_choreography_new_default(void);

/**
 * clawt_office_choreography_default_yaml:
 *
 * Returns: (transfer none): the built-in rules as YAML, the shape an
 *   override is written in
 */
const gchar *clawt_office_choreography_default_yaml(void);

/**
 * clawt_office_choreography_override:
 * @self: rules
 * @yaml: a `choreography:` document
 * @error: return location for why it was refused
 *
 * Replaces the rules @yaml names, leaving the rest.  Applied in order:
 * the built-in rules, then the pack's, then the person's.
 *
 * Returns: %TRUE when @yaml was applied
 */
gboolean clawt_office_choreography_override(ClawtOfficeChoreography *self,
                                            const gchar             *yaml,
                                            GError                 **error);

/**
 * clawt_office_choreography_cue:
 * @self: rules
 * @activity: an activity
 * @station: its station when working
 *
 * Returns: (transfer none): what @activity looks like; never %NULL
 */
const ClawtOfficeCue *clawt_office_choreography_cue(ClawtOfficeChoreography *self,
                                                    ClawtOfficeActivity      activity,
                                                    ClawtOfficeStation       station);

/**
 * clawt_office_choreography_to_json:
 * @self: rules
 *
 * Returns: (transfer full): every rule, as `office.world` carries them
 */
JsonNode *clawt_office_choreography_to_json(ClawtOfficeChoreography *self);

G_END_DECLS
