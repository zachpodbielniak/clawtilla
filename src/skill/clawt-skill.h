/*
 * clawt-skill.h - One reusable procedure, as a directory on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A skill is `<skills.dir>/<name>/SKILL.md`: markdown with YAML front
 * matter naming it and saying when to use it.  That layout is not
 * clawtilla's invention and is not negotiable -- it is the intersection
 * every one of the five CLIs libreclaw can drive already understands, so
 * a skill written once is readable by all of them without translation.
 *
 * Everything about the type follows from one fact: the text in a skill
 * reaches a model's context, and a good deal of it arrives from
 * somewhere else.  A registry audit of public skills found confirmed
 * exfiltration payloads in between 2% and 13% of them, almost always in
 * a sibling script rather than in the markdown.  So a skill carries its
 * provenance, its digest and its warnings alongside its text, an
 * imported one is disabled until a person has read it, and scripts are
 * not copied at all.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * CLAWT_SKILL_MAX_NAME:
 *
 * The longest a skill's name may be.
 *
 * Matches what the harnesses themselves accept.  A longer name is
 * refused rather than truncated: truncation would make two skills
 * collide under one directory name, and the directory name is the
 * identity.
 */
#define CLAWT_SKILL_MAX_NAME (64)

/**
 * CLAWT_SKILL_MAX_DESCRIPTION:
 *
 * The longest a skill's description may be.
 *
 * The description is the *only* part of a skill that is in context
 * before the skill is used -- every harness lists name and description
 * so the model can decide whether to open the body.  An unbounded
 * description is therefore an unbounded prompt on every turn, paid for
 * by every agent the skill is assigned to.
 */
#define CLAWT_SKILL_MAX_DESCRIPTION (1024)

#define CLAWT_TYPE_SKILL (clawt_skill_get_type())

GType clawt_skill_get_type(void) G_GNUC_CONST;

/**
 * clawt_skill_new:
 * @name: the skill's name, which is also its directory name
 *
 * An empty skill with no body and no description.
 *
 * Returns: (transfer full) (nullable): the skill, or %NULL when @name is
 *   not a valid skill name
 */
ClawtSkill *clawt_skill_new(const gchar *name);

ClawtSkill *clawt_skill_copy(ClawtSkill *self);
void        clawt_skill_free(ClawtSkill *self);

const gchar *clawt_skill_get_name(ClawtSkill *self);
const gchar *clawt_skill_get_description(ClawtSkill *self);
const gchar *clawt_skill_get_body(ClawtSkill *self);
const gchar *clawt_skill_get_directory(ClawtSkill *self);
const gchar *clawt_skill_get_origin_url(ClawtSkill *self);
const gchar *clawt_skill_get_digest(ClawtSkill *self);
gint64       clawt_skill_get_imported_at(ClawtSkill *self);

ClawtSkillSource clawt_skill_get_source(ClawtSkill *self);
gboolean         clawt_skill_get_enabled(ClawtSkill *self);

void clawt_skill_set_description(ClawtSkill *self, const gchar *description);
void clawt_skill_set_body(ClawtSkill *self, const gchar *body);
void clawt_skill_set_directory(ClawtSkill *self, const gchar *directory);
void clawt_skill_set_origin_url(ClawtSkill *self, const gchar *url);
void clawt_skill_set_digest(ClawtSkill *self, const gchar *digest);
void clawt_skill_set_imported_at(ClawtSkill *self, gint64 stamp);
void clawt_skill_set_source(ClawtSkill *self, ClawtSkillSource source);
void clawt_skill_set_enabled(ClawtSkill *self, gboolean enabled);

/**
 * clawt_skill_get_meta:
 * @self: a #ClawtSkill
 * @key: a front-matter key
 *
 * A front-matter value clawtilla does not itself understand.
 *
 * Everything but `name` and `description` is preserved verbatim and
 * acted on by nobody here.  `allowed-tools` is the reason that is the
 * rule rather than an omission: it is a Claude Code extension that grok,
 * antigravity and opencode ignore entirely, and which is documented not
 * to restrict anything even where it is read.  Treating it as a
 * permission boundary would be a boundary that holds on one harness in
 * five, which is worse than none because somebody would rely on it.
 *
 * Returns: (transfer none) (nullable): the value, or %NULL
 */
const gchar *clawt_skill_get_meta(ClawtSkill *self, const gchar *key);

/**
 * clawt_skill_set_meta:
 * @self: a #ClawtSkill
 * @key: a front-matter key
 * @value: (nullable): the value, or %NULL to drop the key
 */
void clawt_skill_set_meta(ClawtSkill  *self,
                          const gchar *key,
                          const gchar *value);

/**
 * clawt_skill_get_meta_keys:
 * @self: a #ClawtSkill
 *
 * The preserved front-matter keys, in the order they were read.
 *
 * Order is kept so that rewriting a skill's file does not reorder
 * somebody's front matter, which would show up as a diff in every skill
 * the first time clawtilla touched it.
 *
 * Returns: (transfer full) (array zero-terminated=1): the keys
 */
GStrv clawt_skill_get_meta_keys(ClawtSkill *self);

/**
 * clawt_skill_get_warnings:
 * @self: a #ClawtSkill
 *
 * What the scan noticed, in sentences meant for whoever is reviewing it.
 *
 * Never a reason to refuse: the scan reports, the reviewer decides.  A
 * base64 blob is how one skill ships a lookup table and how another
 * ships a payload, and nothing in the text can tell them apart.
 *
 * Returns: (transfer none) (element-type utf8): the warnings, possibly
 *   empty
 */
GPtrArray *clawt_skill_get_warnings(ClawtSkill *self);

void clawt_skill_add_warning(ClawtSkill *self, const gchar *warning);

/**
 * clawt_skill_get_skipped:
 * @self: a #ClawtSkill
 *
 * Files that were in the source and were deliberately not copied.
 *
 * Shown rather than logged.  The point of markdown-only import is that
 * nothing executable comes along; the point of *listing* what was left
 * behind is that a skill whose instructions say "run scripts/setup.sh"
 * will otherwise fail in a way that reads as clawtilla being broken.
 *
 * Returns: (transfer none) (element-type utf8): the file names
 */
GPtrArray *clawt_skill_get_skipped(ClawtSkill *self);

void clawt_skill_add_skipped(ClawtSkill *self, const gchar *filename);

/**
 * clawt_skill_name_is_valid:
 * @name: a candidate name
 *
 * Whether @name may be used as a skill name, and therefore as a
 * directory name under `skills.dir`.
 *
 * **This is the traversal gate.**  A skill's name is joined onto a
 * directory on every path in this subsystem -- reading it, linking it
 * into five workspaces, removing it -- so the check has to be the
 * function, not a line at whichever entry point somebody remembered.
 * Lowercase ASCII letters, digits and single interior hyphens; no dots,
 * no slashes, no leading or trailing hyphen, and therefore no way to
 * name a skill `.`, `..`, `a/b` or `.hidden`.
 *
 * A percent-encoded name must be decoded *before* it reaches this;
 * clawt_skill_name_from_wire() is that decode.
 *
 * Returns: %TRUE if the name is usable
 */
gboolean clawt_skill_name_is_valid(const gchar *name);

/**
 * clawt_skill_name_from_wire:
 * @raw: a name as it arrived from a client, a URL or a config file
 * @error: (out) (optional): return location for a #GError
 *
 * Percent-decodes @raw and then validates it.
 *
 * The order is deliberate, and it is worth being exact about what it
 * buys, because the obvious claim is not quite true.  `%` is not in the
 * allowlist either, so a single encoded traversal is refused whichever
 * way round the two steps go -- the allowlist is doing that work, not
 * the decode.
 *
 * What the order buys is two things the allowlist does not.  The
 * refusal names `../` rather than `%2e%2e%2f`, which is what a person
 * needs in order to understand what was actually asked for.  And the
 * value this returns is the *decoded* one, so a caller cannot hand a
 * validated-but-still-encoded string to something further down that
 * decodes it again -- which is the shape the traversal would actually
 * take, since it needs a decode somebody else performs.
 *
 * Returns: (transfer full) (nullable): the decoded name, or %NULL with
 *   @error set
 */
gchar *clawt_skill_name_from_wire(const gchar *raw, GError **error);

/**
 * clawt_skill_directory_for:
 * @skills_dir: the library's root
 * @name: a skill name
 *
 * Where a skill of that name lives.
 *
 * Goes through clawt_skill_name_is_valid() and returns %NULL rather than
 * building a path from a name it has not checked, so a caller cannot
 * reach outside @skills_dir by forgetting to validate.
 *
 * Returns: (transfer full) (nullable): the directory, or %NULL
 */
gchar *clawt_skill_directory_for(const gchar *skills_dir, const gchar *name);

/**
 * clawt_skill_render:
 * @self: a #ClawtSkill
 *
 * The complete `SKILL.md`: front matter, then the body.
 *
 * Returns: (transfer full): the file's text
 */
gchar *clawt_skill_render(ClawtSkill *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtSkill, clawt_skill_free)

G_END_DECLS
