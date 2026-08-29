/*
 * clawt-skill-library.h - Every skill the fleet has, once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A skill lives in exactly one place -- `skills.dir` -- and is *linked*
 * into whichever workspaces need it.  One copy is the whole point: a
 * skill edited once is edited for every agent that has it, and a skill
 * reviewed once has been reviewed for all of them.  Copying per agent
 * would mean twelve divergent versions of a procedure and no way to tell
 * which of them somebody had actually read.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "skill/clawt-skill.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_SKILL_LIBRARY (clawt_skill_library_get_type())

G_DECLARE_FINAL_TYPE(ClawtSkillLibrary, clawt_skill_library,
                     CLAWT, SKILL_LIBRARY, GObject)

/**
 * clawt_skill_library_new:
 * @directory: where the skills live, from `skills.dir`
 *
 * A library over @directory, not yet scanned.
 *
 * Returns: (transfer full): the library
 */
ClawtSkillLibrary *clawt_skill_library_new(const gchar *directory);

const gchar *clawt_skill_library_get_directory(ClawtSkillLibrary *self);

/**
 * clawt_skill_library_scan:
 * @self: a #ClawtSkillLibrary
 *
 * Re-reads every skill from disk.
 *
 * A directory that does not exist is an empty library, not an error:
 * `skills.dir` has a default and most fleets will never create it.
 * Creating it here would mean a read had a side effect, which is what
 * makes an accidental typo in the path leave an empty directory behind
 * for somebody to puzzle over later.
 */
void clawt_skill_library_scan(ClawtSkillLibrary *self);

/**
 * clawt_skill_library_set_watching:
 * @self: a #ClawtSkillLibrary
 * @watching: whether to follow the directory
 *
 * Follows @directory with a #GFileMonitor, rescanning after a debounce
 * and emitting #ClawtSkillLibrary::changed.
 *
 * The debounce timer is attached to the context that is thread-default
 * **when this is called**, captured here rather than at whichever call
 * site happened to reach it: an embedded daemon runs its own loop, and a
 * timer on the global default context in that arrangement never fires
 * at all.
 */
void clawt_skill_library_set_watching(ClawtSkillLibrary *self,
                                      gboolean           watching);

gboolean clawt_skill_library_get_watching(ClawtSkillLibrary *self);

/**
 * clawt_skill_library_list:
 * @self: a #ClawtSkillLibrary
 *
 * Every skill, by name.
 *
 * Returns: (transfer container) (element-type ClawtSkill): the skills
 */
GPtrArray *clawt_skill_library_list(ClawtSkillLibrary *self);

/**
 * clawt_skill_library_lookup:
 * @self: a #ClawtSkillLibrary
 * @name: a skill name
 *
 * Returns: (transfer none) (nullable): the skill, or %NULL
 */
ClawtSkill *clawt_skill_library_lookup(ClawtSkillLibrary *self,
                                       const gchar       *name);

/**
 * clawt_skill_library_get_problems:
 * @self: a #ClawtSkillLibrary
 *
 * Directories that look like skills and could not be read, each with the
 * reason.
 *
 * Kept rather than only logged, because a skill that fails to parse is
 * invisible in every other view: assigning it warns that no skill of
 * that name exists, which sends somebody to look at the assignment
 * rather than at the file.
 *
 * Returns: (transfer none) (element-type utf8): the messages
 */
GPtrArray *clawt_skill_library_get_problems(ClawtSkillLibrary *self);

/**
 * clawt_skill_library_create:
 * @self: a #ClawtSkillLibrary
 * @name: what to call it
 * @description: when an agent should reach for it
 * @body: (nullable): the markdown, or %NULL for a starting template
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a new skill into the library, enabled.
 *
 * Enabled because somebody sat here and wrote it, which is the review.
 * That is the one difference from clawt_skill_library_import(), and it
 * is the difference the whole security posture turns on.
 *
 * Returns: (transfer none) (nullable): the new skill, or %NULL
 */
ClawtSkill *clawt_skill_library_create(ClawtSkillLibrary  *self,
                                       const gchar        *name,
                                       const gchar        *description,
                                       const gchar        *body,
                                       GError            **error);

/**
 * clawt_skill_library_create_taught:
 * @self: a #ClawtSkillLibrary
 * @name: what to call it
 * @description: when an agent should reach for it
 * @body: the markdown a model wrote from a recorded demonstration
 * @origin: (nullable): which recording it came from, as `teach:<id>`
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a synthesized skill into the library, **disabled**.
 *
 * The same write as clawt_skill_library_create() with a different
 * provenance, so the name rules, the description bound, the traversal
 * gate and the warning scan are one implementation rather than two --
 * and the AI-written half is not the one that skipped a check.
 *
 * Disabled because nobody has read it.  A skill clawtilla wrote is not
 * more trusted than one somebody downloaded: the model that wrote it
 * read a recording, and a recording of a demonstration is untrusted
 * input the moment it contains anything a person typed.
 *
 * Returns: (transfer none) (nullable): the new skill, or %NULL
 */
ClawtSkill *clawt_skill_library_create_taught(ClawtSkillLibrary  *self,
                                              const gchar        *name,
                                              const gchar        *description,
                                              const gchar        *body,
                                              const gchar        *origin,
                                              GError            **error);

/**
 * clawt_skill_library_import:
 * @self: a #ClawtSkillLibrary
 * @source: a directory holding a `SKILL.md`, or a `SKILL.md` itself
 * @origin: (nullable): where it came from, for the provenance record
 * @error: (out) (optional): return location for a #GError
 *
 * Copies a skill in from somewhere else, **disabled**, markdown only.
 *
 * Three rules, and each of them exists because of the same audit: a
 * registry sweep found working exfiltration payloads in between 2% and
 * 13% of public skills, and they were almost never in the markdown.
 *
 * - It lands **disabled**.  Nothing in it reaches any prompt until a
 *   person has read it and said so.
 * - **Markdown only.**  Sibling scripts are not copied.  They are
 *   recorded on the skill and *shown*, because a skill whose steps say
 *   "run scripts/setup.sh" fails confusingly rather than obviously when
 *   the script is silently absent.
 * - **Provenance is pinned**: @origin and a SHA-256 of the rendered
 *   `SKILL.md` are recorded now, so a later edit is detectable rather
 *   than merely deniable.
 *
 * Returns: (transfer none) (nullable): the imported skill, or %NULL
 */
ClawtSkill *clawt_skill_library_import(ClawtSkillLibrary  *self,
                                       const gchar        *source,
                                       const gchar        *origin,
                                       GError            **error);

/**
 * clawt_skill_library_set_enabled:
 * @self: a #ClawtSkillLibrary
 * @name: a skill name
 * @enabled: whether it may reach a prompt
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean clawt_skill_library_set_enabled(ClawtSkillLibrary  *self,
                                         const gchar        *name,
                                         gboolean            enabled,
                                         GError            **error);

/**
 * clawt_skill_library_remove:
 * @self: a #ClawtSkillLibrary
 * @name: a skill name
 * @error: (out) (optional): return location for a #GError
 *
 * Deletes the skill's directory.
 *
 * The removal is bounded by the library's own directory through
 * clawt_remove_tree(), which checks the canonical path per child --
 * because the name reaching this came from a client and the library
 * root came from a config file, and only one of those has been
 * validated.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_skill_library_remove(ClawtSkillLibrary  *self,
                                    const gchar        *name,
                                    GError            **error);

G_END_DECLS
