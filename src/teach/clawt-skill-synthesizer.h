/*
 * clawt-skill-synthesizer.h - Turning a recording into a skill draft
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The same shape as #ClawtAgentDesigner, deliberately: the model is
 * given tools that fill in a draft rather than a filesystem, the draft
 * is previewed, and the commit goes through the path manual creation
 * already uses.  A second write path for AI-written skills would be a
 * second implementation of the validation, and the two would drift --
 * with the AI-written one being the half nobody read.
 *
 * **A synthesized skill is not more trusted for having come from us.**
 * It lands disabled, every front-matter rule applies to it, and the
 * three warning checks run over it exactly as they do over an imported
 * one.  The model that wrote it read a trace, and a trace of a
 * demonstration is untrusted input the moment it contains anything a
 * person typed.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "skill/clawt-skill-library.h"
#include "teach/clawt-teach-trace.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_SKILL_SYNTHESIZER (clawt_skill_synthesizer_get_type())

G_DECLARE_FINAL_TYPE(ClawtSkillSynthesizer, clawt_skill_synthesizer,
                     CLAWT, SKILL_SYNTHESIZER, GObject)

/**
 * clawt_skill_synthesizer_new:
 * @library: (transfer none): where a committed draft is written
 * @config: (nullable) (transfer none): the fleet configuration, for
 *   `ai_assist.*`
 *
 * Returns: (transfer full): a new #ClawtSkillSynthesizer
 */
ClawtSkillSynthesizer *clawt_skill_synthesizer_new(
    ClawtSkillLibrary *library,
    ClawtConfig       *config);

/**
 * clawt_skill_synthesizer_set_provider: (skip)
 * @self: a #ClawtSkillSynthesizer
 * @provider: (transfer none): the AI provider to draft with
 *
 * Supplying the provider rather than building one inside is what lets
 * the tests drive this with AiMockProvider through the same code path.
 *
 * Skipped for introspection for the reason the designer's is: ai-glib
 * ships no GIR, so #AiProvider has no name a binding could resolve.
 */
void clawt_skill_synthesizer_set_provider(ClawtSkillSynthesizer *self,
                                          AiProvider            *provider);

/**
 * clawt_skill_synthesizer_set_provider_by_name:
 * @self: a #ClawtSkillSynthesizer
 * @provider_name: (nullable): a provider id from the model catalogue
 * @model: (nullable): the model to run
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the provider could be built
 */
gboolean clawt_skill_synthesizer_set_provider_by_name(
    ClawtSkillSynthesizer  *self,
    const gchar            *provider_name,
    const gchar            *model,
    GError                **error);

/**
 * clawt_skill_synthesizer_use_configured_provider:
 * @self: a #ClawtSkillSynthesizer
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the provider named by `ai_assist.provider` and `ai_assist.model`.
 *
 * Returns: %TRUE if a provider was built
 */
gboolean clawt_skill_synthesizer_use_configured_provider(
    ClawtSkillSynthesizer  *self,
    GError                **error);

/**
 * clawt_skill_synthesizer_set_max_turns:
 * @self: a #ClawtSkillSynthesizer
 * @max_turns: how many rounds the model gets
 */
void clawt_skill_synthesizer_set_max_turns(ClawtSkillSynthesizer *self,
                                           guint                  max_turns);

/**
 * clawt_skill_synthesizer_synthesize:
 * @self: a #ClawtSkillSynthesizer
 * @trace: (transfer none): what was recorded
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Runs the drafting conversation and returns what the model settled on.
 *
 * Nothing is written: the draft has to be committed separately, so a
 * person reads the skill before it exists.  That is not ceremony -- the
 * trace it was written from may contain a password, and the draft is
 * where that would surface.
 *
 * **It blocks**, so it belongs on a worker thread; the daemon's
 * `teach.synthesize` defers.
 *
 * Returns: (transfer none) (nullable) (element-type utf8 utf8): the
 *   draft as `name`, `description` and `body`
 */
GHashTable *clawt_skill_synthesizer_synthesize(
    ClawtSkillSynthesizer  *self,
    ClawtTeachTrace        *trace,
    GCancellable           *cancellable,
    GError                **error);

/**
 * clawt_skill_synthesizer_get_draft:
 * @self: a #ClawtSkillSynthesizer
 *
 * Returns: (transfer none) (element-type utf8 utf8): the draft
 */
GHashTable *clawt_skill_synthesizer_get_draft(ClawtSkillSynthesizer *self);

/**
 * clawt_skill_synthesizer_preview:
 * @self: a #ClawtSkillSynthesizer
 *
 * The `SKILL.md` the draft would become, front matter and all.
 *
 * The rendered file rather than a summary of it, because the rendered
 * file is what a reviewer has to read: the scan runs over exactly these
 * bytes, and a preview that showed something tidier would be a preview
 * of a different document.
 *
 * Returns: (transfer full): the preview
 */
gchar *clawt_skill_synthesizer_preview(ClawtSkillSynthesizer *self);

/**
 * clawt_skill_synthesizer_commit:
 * @self: a #ClawtSkillSynthesizer
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the draft into the library, **disabled**.
 *
 * Through clawt_skill_library_create_taught(), which is the same write
 * as manual creation with a different provenance -- so the name rules,
 * the description bound, the traversal gate and the warning scan are one
 * implementation rather than two.
 *
 * Returns: (transfer none) (nullable): the new skill
 */
ClawtSkill *clawt_skill_synthesizer_commit(ClawtSkillSynthesizer  *self,
                                           GError                **error);

/**
 * clawt_skill_synthesizer_get_transcript:
 * @self: a #ClawtSkillSynthesizer
 *
 * Returns: (transfer none) (nullable): what the model said while drafting
 */
const gchar *clawt_skill_synthesizer_get_transcript(
    ClawtSkillSynthesizer *self);

G_END_DECLS
