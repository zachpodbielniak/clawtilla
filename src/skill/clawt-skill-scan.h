/*
 * clawt-skill-scan.h - Reading a SKILL.md, and saying what is odd about it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Text in, skill out: nothing here touches a filesystem, which is what
 * lets the parser and the whole security scan be tested against string
 * literals rather than against a fixture tree somebody has to keep in
 * step.
 *
 * The scan **warns and never rejects**.  That is a decision, not an
 * omission.  Every pattern it looks for has an entirely legitimate use
 * -- a long base64 string is how one skill ships a lookup table and how
 * another ships a payload, and no amount of looking at the text
 * distinguishes them.  A checker that refused would be wrong often
 * enough to be turned off, and a checker that is turned off catches
 * nothing.  So it reports, in sentences, to the person who is about to
 * enable the skill; and until that person does, an imported skill
 * reaches no prompt at all.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

#include "clawt-types.h"
#include "skill/clawt-skill.h"

G_BEGIN_DECLS

/**
 * clawt_skill_parse:
 * @text: the contents of a `SKILL.md`
 * @length: @text's length, or -1 when it is NUL-terminated
 * @expected_name: (nullable): the directory the file was found in
 * @error: (out) (optional): return location for a #GError
 *
 * Parses front matter and body, validates both, and runs the scan.
 *
 * Front matter must open on the very first line.  A `---` further down
 * is a horizontal rule in every markdown renderer there is, and reading
 * one as front matter would mean a document's first section heading
 * became its `name`.
 *
 * When @expected_name is given, a `name:` that disagrees with it is
 * refused rather than preferred either way.  The directory is what every
 * harness resolves `/name` and skill lookup against, and the front
 * matter is what the model is shown; a skill whose two names differ is
 * one that answers to one name and describes itself as another.
 *
 * Returns: (transfer full) (nullable): the skill, or %NULL with @error
 */
ClawtSkill *clawt_skill_parse(const gchar  *text,
                              gssize        length,
                              const gchar  *expected_name,
                              GError      **error);

/**
 * clawt_skill_scan_text:
 * @text: any text a skill would carry
 *
 * The three things worth telling a reviewer about, as sentences.
 *
 * Three, and only three, because each one is a thing a person cannot
 * see by reading the file:
 *
 * - a **long base64-looking blob**, which is unreadable by construction;
 * - a **download piped into a shell**, where what runs is decided by a
 *   server at the moment it runs, so the file being reviewed does not
 *   contain the instruction being reviewed;
 * - **invisible Unicode** -- zero-width characters and bidirectional
 *   overrides -- which is the sharpest of the three, because it hides
 *   text from the reviewer while the model reads it exactly as written.
 *
 * Returns: (transfer full) (element-type utf8): the warnings, empty when
 *   nothing was noticed
 */
GPtrArray *clawt_skill_scan_text(const gchar *text);

/**
 * clawt_skill_scan:
 * @self: a #ClawtSkill
 *
 * Runs clawt_skill_scan_text() over the whole rendered file and records
 * what it found on @self.
 *
 * Over the *rendered* file rather than the body, because front matter is
 * text too: a zero-width character in a description is in every agent's
 * context on every turn without ever being opened.
 */
void clawt_skill_scan(ClawtSkill *self);

/**
 * clawt_skill_digest:
 * @text: a rendered `SKILL.md`
 *
 * The SHA-256 recorded at import and compared on every read.
 *
 * Returns: (transfer full): the digest, lowercase hex
 */
gchar *clawt_skill_digest(const gchar *text);

G_END_DECLS
