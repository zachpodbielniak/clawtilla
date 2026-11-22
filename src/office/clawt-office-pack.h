/*
 * clawt-office-pack.h - Changing a world pack on disk, safely
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#ifndef CLAWT_OFFICE_PACK_H
#define CLAWT_OFFICE_PACK_H

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_OFFICE_PACK_MAX_TEXT:
 *
 * The most clawt_office_pack_write_file() writes in one call: a manifest,
 * a choreography or a map is text, and anything larger is an asset, which
 * goes through clawt_office_pack_import_asset() instead.
 */
#define CLAWT_OFFICE_PACK_MAX_TEXT (4 * 1024 * 1024)

/**
 * CLAWT_OFFICE_PACK_MAX_ASSET:
 *
 * The largest file clawt_office_pack_import_asset() copies into a pack.
 */
#define CLAWT_OFFICE_PACK_MAX_ASSET (64 * 1024 * 1024)

/**
 * clawt_office_pack_path_is_safe:
 * @relative: (nullable): a path a caller wants inside a pack
 *
 * Whether @relative names something inside a pack: not empty, not
 * absolute, no `..` component, no backslash, and no hidden component.
 * The same rule the loader applies to a manifest's paths, so a file
 * written here is one a manifest may name.
 *
 * Returns: %TRUE when it is safe
 */
gboolean clawt_office_pack_path_is_safe(const gchar *relative);

/**
 * clawt_office_pack_fork:
 * @source: a pack's directory
 * @destination: where the copy goes; must not exist
 * @error: return location for a #GError
 *
 * Copies a pack -- regular files and directories only, never a symbolic
 * link, which could point anywhere -- so a shipped pack can be changed
 * by changing a copy under `<state_dir>/office/worlds/`, which the world
 * search finds first.
 *
 * Returns: %TRUE when the copy is whole
 */
gboolean clawt_office_pack_fork(const gchar  *source,
                                const gchar  *destination,
                                GError      **error);

/**
 * clawt_office_pack_write_file:
 * @pack: the pack's directory
 * @relative: the file, inside the pack
 * @contents: (array length=length): the new contents
 * @length: their length
 * @error: return location for a #GError
 *
 * Writes one file of a pack.  `office.yaml` and `choreography.yaml` are
 * checked before they stay: the pack is loaded (and the choreography
 * parsed) with the new text, and on a refusal the old file is put back
 * and the refusal returned, so a pack that loaded before a write still
 * loads after a refused one.
 *
 * Returns: %TRUE when the file was written and, for a manifest or a
 *   choreography, accepted
 */
gboolean clawt_office_pack_write_file(const gchar  *pack,
                                      const gchar  *relative,
                                      const gchar  *contents,
                                      gsize         length,
                                      GError      **error);

/**
 * clawt_office_pack_import_asset:
 * @pack: the pack's directory
 * @source_path: the file to copy in
 * @relative: where it goes inside the pack
 * @license: (nullable): the asset's licence, when it is not the pack's
 * @provenance: where it came from: a URL, an author, or a generator, its
 *   model and its terms
 * @out_sha256: (out) (optional) (transfer full): the file's sha256, hex
 * @error: return location for a #GError
 *
 * Copies @source_path into the pack and records it in the manifest's
 * `assets` list with its sha256, licence and provenance, replacing an
 * entry for the same path.  An asset without provenance is refused: a
 * pack is portable between fleets, and "where did this picture come
 * from" has to stay answerable after it moves.
 *
 * Returns: %TRUE when the file is in the pack and recorded
 */
gboolean clawt_office_pack_import_asset(const gchar  *pack,
                                        const gchar  *source_path,
                                        const gchar  *relative,
                                        const gchar  *license,
                                        const gchar  *provenance,
                                        gchar       **out_sha256,
                                        GError      **error);

/**
 * clawt_office_character_import_asset:
 * @characters: the characters directory, `<state_dir>/office/characters`
 * @agent_id: whose character this file is part of
 * @source_path: the file to copy in
 * @relative: where it goes inside the character, e.g. `sprites/walk.png`
 * @license: (nullable): its licence
 * @provenance: where it came from: the generator, its model and terms
 * @out_sha256: (out) (optional) (transfer full): the file's sha256, hex
 * @error: return location for a #GError
 *
 * clawt_office_pack_import_asset() for an agent's character: the same
 * rules, recorded in the character's own `character.yaml`, which is made
 * with the first file.
 *
 * Returns: %TRUE when the file is in the character and recorded
 */
gboolean clawt_office_character_import_asset(const gchar  *characters,
                                             const gchar  *agent_id,
                                             const gchar  *source_path,
                                             const gchar  *relative,
                                             const gchar  *license,
                                             const gchar  *provenance,
                                             gchar       **out_sha256,
                                             GError      **error);

/**
 * ClawtOfficeAnimation:
 * @name: what it is for: `idle`, `walk_down`, `walk_up`, `walk_left`,
 *   `walk_right`, `sit`, `type`, `read`, `phone`, `present`, or a pack's own
 * @row: its row in the sheet, from 0
 * @frames: how many frames, left to right
 * @fps: frames a second; 0 holds the first frame
 *
 * One row of a character's sprite sheet.
 */
typedef struct {
    gchar   *name;
    guint    row;
    guint    frames;
    gdouble  fps;
} ClawtOfficeAnimation;

/**
 * ClawtOfficeCharacter:
 * @agent: whose character it is
 * @sheet: (nullable): the sprite sheet, relative to the character's
 *   directory, or %NULL while it has none
 * @sheet_sha256: (nullable): its checksum, from the manifest's `assets`
 * @frame_width: one frame's width in pixels
 * @frame_height: one frame's height in pixels
 * @animations: (element-type ClawtOfficeAnimation): its rows
 *
 * An agent's character as the custodial team made it
 * (`<state_dir>/office/characters/<agent>/character.yaml`).
 */
typedef struct {
    gchar     *agent;
    gchar     *sheet;
    gchar     *sheet_sha256;
    guint      frame_width;
    guint      frame_height;
    GPtrArray *animations;
} ClawtOfficeCharacter;

GType clawt_office_character_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_OFFICE_CHARACTER (clawt_office_character_get_type())

/**
 * clawt_office_character_copy:
 * @character: a character
 *
 * Returns: (transfer full): a deep copy
 */
ClawtOfficeCharacter *clawt_office_character_copy(const ClawtOfficeCharacter *character);

/**
 * clawt_office_character_free:
 * @character: (nullable): a character
 */
void clawt_office_character_free(ClawtOfficeCharacter *character);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtOfficeCharacter, clawt_office_character_free)

/**
 * clawt_office_character_load:
 * @dir: a character's directory
 * @error: return location for a #GError
 *
 * Reads `character.yaml`.  A character with no `sheet` yet loads, with
 * @sheet %NULL: its assets are still being made.
 *
 * Returns: (transfer full) (nullable): the character
 */
ClawtOfficeCharacter *clawt_office_character_load(const gchar *dir, GError **error);

/**
 * clawt_office_character_to_json:
 * @character: a character
 *
 * Returns: (transfer full): `{agent, sheet, sheet_sha256, frame: [w, h],
 *   animations: {name: {row, frames, fps}}}`
 */
JsonNode *clawt_office_character_to_json(const ClawtOfficeCharacter *character);

/**
 * clawt_office_character_set_sheet:
 * @characters: the characters directory, `<state_dir>/office/characters`
 * @agent_id: whose character
 * @sheet: the sprite sheet, relative to the character's directory; it must
 *   have been imported (clawt_office_character_import_asset()) and be a PNG
 * @frame_width: one frame's width, 8 to 4096 pixels
 * @frame_height: one frame's height, 8 to 4096 pixels
 * @animations: `{name: {row, frames, fps}}`, at least one
 * @error: return location for a #GError
 *
 * Says how the sheet is laid out, checked against the image: every
 * animation's row and frames must fit inside it, so a renderer never
 * reads past the sheet.  The manifest's assets are kept.
 *
 * Returns: %TRUE when the manifest was written
 */
gboolean clawt_office_character_set_sheet(const gchar  *characters,
                                          const gchar  *agent_id,
                                          const gchar  *sheet,
                                          guint         frame_width,
                                          guint         frame_height,
                                          JsonObject   *animations,
                                          GError      **error);

/**
 * clawt_office_png_size:
 * @path: a file
 * @width: (out): its width
 * @height: (out): its height
 *
 * Reads a PNG's size from its header, without decoding it.
 *
 * Returns: %TRUE when @path is a PNG
 */
gboolean clawt_office_png_size(const gchar *path, guint *width, guint *height);

/**
 * clawt_office_file_sha256:
 * @path: a file
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): its sha256, hex
 */
gchar *clawt_office_file_sha256(const gchar *path, GError **error);

G_END_DECLS

#endif /* CLAWT_OFFICE_PACK_H */
