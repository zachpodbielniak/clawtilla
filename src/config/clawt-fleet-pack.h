/*
 * clawt-fleet-pack.h - A fleet as a directory: exported, read, taken in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * For moving a fleet to another machine, or starting a new one from a
 * fleet that works: one operator's migration and template tool, not a
 * marketplace (since 0.42.0).  A pack is a directory:
 *
 *   FLEET.yaml               the manifest: agents, teams, routines,
 *                            triggers and projects as they are configured,
 *                            and what was left out and must be filled in
 *   agents/<id>/<FILE>.org   each agent's persona files
 *   skills/<name>/SKILL.md   the library's own skills, with their tests/
 *
 * What never travels: a secret's value or reference, anything that names
 * this machine's paths (an agent's `workspace`), the daemon's own section,
 * and every credential file.  Each one left out is listed in the
 * manifest's `to_fill`, so the new fleet says what it is missing rather
 * than failing on it later.
 *
 * Taking one in never widens what agents may do: an item that turns on a
 * dangerous setting (one the schema marks so -- host control, desktop
 * control, sudo, ...) is refused unless the import is told
 * `allow_dangerous`; agents arrive held (`runtime.autostart: false`);
 * skills arrive disabled and markdown-only, as any import does.
 */

#pragma once

#include <json-glib/json-glib.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "skill/clawt-skill-library.h"

G_BEGIN_DECLS

/**
 * CLAWT_FLEET_PACK_MANIFEST:
 *
 * The manifest's file name inside a pack.
 */
#define CLAWT_FLEET_PACK_MANIFEST "FLEET.yaml"

/**
 * clawt_fleet_pack_export:
 * @config: the fleet
 * @skills: (nullable): its skill library, for the skills to include
 * @destination: a directory that does not exist yet, or is empty
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the pack.  The answer counts what went in and lists `to_fill`.
 *
 * Returns: (transfer full) (nullable): `{agents, teams, routines,
 *   triggers, projects, skills, files, to_fill}`, or %NULL with @error
 */
JsonNode *clawt_fleet_pack_export(ClawtConfig        *config,
                                  ClawtSkillLibrary  *skills,
                                  const gchar        *destination,
                                  GError            **error);

/**
 * clawt_fleet_pack_preview:
 * @config: the fleet it would go into
 * @skills: (nullable): its skill library
 * @source: a pack directory
 * @error: (out) (optional): return location for a #GError
 *
 * What taking @source in would do, without doing it.
 *
 * Returns: (transfer full) (nullable): `{items: [{kind, id, exists,
 *   dangerous: [keys]}], to_fill: [...]}`, or %NULL with @error
 */
JsonNode *clawt_fleet_pack_preview(ClawtConfig        *config,
                                   ClawtSkillLibrary  *skills,
                                   const gchar        *source,
                                   GError            **error);

/**
 * clawt_fleet_pack_merge:
 * @config: the fleet it goes into
 * @source: a pack directory
 * @resolutions: (nullable): `"<kind>:<id>"` to `skip`, `replace` or
 *   `rename:<new id>`; an item that exists is skipped unless told
 *   otherwise, one that does not is added
 * @allow_dangerous: whether items that turn on a dangerous setting may
 *   come in at all
 * @out_taken: (out) (transfer full) (optional) (element-type utf8): each
 *   item taken in, as `"<kind>:<id>"` with its id as it will be
 * @error: (out) (optional): return location for a #GError
 *
 * The configuration text @config would have with the pack's items merged
 * in -- validated by loading it, but not written anywhere: saving it and
 * reloading is the caller's, so it goes through the same path as any
 * other change.  Agents taken in have `runtime.autostart: false`.
 *
 * Returns: (transfer full) (nullable): the new text, or %NULL with @error
 */
gchar *clawt_fleet_pack_merge(ClawtConfig  *config,
                              const gchar  *source,
                              JsonObject   *resolutions,
                              gboolean      allow_dangerous,
                              GPtrArray   **out_taken,
                              GError      **error);

/**
 * clawt_fleet_pack_install_files:
 * @config: the fleet, already holding the merged items
 * @skills: (nullable): its skill library
 * @source: the pack directory
 * @taken: (element-type utf8): what clawt_fleet_pack_merge() took in
 * @resolutions: (nullable): the same resolutions the merge was given: a
 *   renamed agent's files are found under its old name, and a skill that
 *   exists is replaced only when told `replace`
 * @error: (out) (optional): return location for a #GError
 *
 * Puts the pack's persona files into the taken agents' workspaces --
 * never over a file already there -- and imports its skills through the
 * library (disabled, markdown only), with their tests.
 *
 * Returns: %TRUE unless something could not be written
 */
gboolean clawt_fleet_pack_install_files(ClawtConfig        *config,
                                        ClawtSkillLibrary  *skills,
                                        const gchar        *source,
                                        GPtrArray          *taken,
                                        JsonObject         *resolutions,
                                        GError            **error);

G_END_DECLS
