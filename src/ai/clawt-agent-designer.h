/*
 * clawt-agent-designer.h - Designing an agent by describing it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_AGENT_DESIGNER (clawt_agent_designer_get_type())

G_DECLARE_FINAL_TYPE(ClawtAgentDesigner, clawt_agent_designer, CLAWT,
                     AGENT_DESIGNER, GObject)

/**
 * clawt_agent_designer_new:
 * @config: (transfer none): the fleet configuration a new agent joins
 *
 * Turns a description in words into an agent.
 *
 * The model is given tools that edit a draft, not the config file: it can
 * only produce something clawtilla would have accepted anyway, and the
 * commit goes through the same path as creating an agent by hand.  There
 * is no second implementation to drift.
 *
 * Returns: (transfer full): a new #ClawtAgentDesigner
 */
ClawtAgentDesigner *clawt_agent_designer_new(ClawtConfig *config);

/**
 * clawt_agent_designer_set_provider: (skip)
 * @self: a #ClawtAgentDesigner
 * @provider: (transfer none): the AI provider to drive the design with
 *
 * Supplying the provider rather than building one inside is what lets the
 * tests drive this with AiMockProvider and get the same code path.
 *
 * Skipped for introspection: ai-glib does not ship a GIR, so #AiProvider
 * has no name a binding could resolve.  A language binding drives the
 * designer through the daemon's `design.*` requests instead.
 */
void clawt_agent_designer_set_provider(ClawtAgentDesigner *self,
                                       AiProvider         *provider);

/**
 * clawt_agent_designer_use_configured_provider:
 * @self: a #ClawtAgentDesigner
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the provider named by `ai_assist.provider` and `ai_assist.model`
 * and uses it.
 *
 * Separate from clawt_agent_designer_set_provider() so tests and hosts
 * can supply their own provider without the config having to describe
 * one.
 *
 * Returns: %TRUE if a provider was built
 */
/**
 * clawt_agent_designer_pin_identity:
 * @self: a #ClawtAgentDesigner
 * @id: (nullable): the id the person chose, or %NULL to let the model pick
 * @name: (nullable): the display name they chose, or %NULL
 *
 * Fixes the agent's id and name so the model cannot change them.
 *
 * An id typed into a form is a decision. A model that renames it -- and
 * they do, routinely, to something they consider more descriptive --
 * leaves the agent appearing under a name nobody chose, and any script
 * that expected the id it asked for looking at the wrong agent.
 */
/**
 * clawt_agent_designer_pin_computer:
 * @self: a #ClawtAgentDesigner
 * @type: (nullable): the computer type the person chose
 * @settings: (nullable) (element-type utf8 utf8): its settings, by config
 *   key -- `computer.vm.image`, `computer.vm.cpus` and so on
 *
 * Fixes the computer, so the model configures an agent around it rather
 * than choosing one.
 *
 * A VM is the case this exists for.  The designer has no way to name a
 * disk image -- the images that exist are the ones somebody fetched, not
 * something to invent a path for -- so a VM the model picked on its own
 * is always an agent that refuses to provision, with a message naming a
 * setting the model never saw.  Pinning is how the choice and the image
 * arrive together.
 *
 * Written into the draft immediately, so the preview shows what will be
 * created.
 */
void clawt_agent_designer_pin_computer(ClawtAgentDesigner *self,
                                       const gchar        *type,
                                       GHashTable         *settings);

void clawt_agent_designer_pin_identity(ClawtAgentDesigner *self,
                                       const gchar        *id,
                                       const gchar        *name);

/**
 * clawt_agent_designer_set_provider_by_name:
 * @self: a #ClawtAgentDesigner
 * @provider_name: (nullable): a provider id from the model catalogue, or
 *   %NULL for claude-code
 * @model: (nullable): the model to run, or %NULL for the provider's own
 *   default
 * @error: (out) (optional): return location for a #GError
 *
 * Chooses which model does the designing.
 *
 * Separate from the agent's own model: the one that drafts an agent and
 * the one that then runs it have no reason to be the same, and a person
 * will often want their best model for the first and a cheap one for the
 * second.
 *
 * Returns: %TRUE if the provider could be built
 */
gboolean clawt_agent_designer_set_provider_by_name(
    ClawtAgentDesigner  *self,
    const gchar         *provider_name,
    const gchar         *model,
    GError             **error);

gboolean clawt_agent_designer_use_configured_provider(
    ClawtAgentDesigner  *self,
    GError             **error);

/**
 * clawt_agent_designer_set_max_turns:
 * @self: a #ClawtAgentDesigner
 * @max_turns: how many rounds the model gets
 *
 * A cap because a model that never calls commit would otherwise loop for
 * as long as the API keeps answering.
 */
void clawt_agent_designer_set_max_turns(ClawtAgentDesigner *self,
                                        guint               max_turns);

/**
 * clawt_agent_designer_design:
 * @self: a #ClawtAgentDesigner
 * @description: what the agent should be, in words
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Runs the design conversation and returns what the model settled on.
 *
 * Nothing is written: the draft has to be committed separately, so a
 * person sees the result before it becomes an agent.
 *
 * Returns: (transfer none) (nullable): the draft, or %NULL on failure
 */
GHashTable *clawt_agent_designer_design(ClawtAgentDesigner  *self,
                                        const gchar         *description,
                                        GCancellable        *cancellable,
                                        GError             **error);

/**
 * clawt_agent_designer_get_draft:
 * @self: a #ClawtAgentDesigner
 *
 * Returns: (transfer none) (element-type utf8 utf8): the draft as
 *   config keys and values
 */
GHashTable *clawt_agent_designer_get_draft(ClawtAgentDesigner *self);

/**
 * clawt_agent_designer_preview:
 * @self: a #ClawtAgentDesigner
 *
 * The YAML the draft would add to the configuration.
 *
 * Returns: (transfer full): the preview
 */
gchar *clawt_agent_designer_preview(ClawtAgentDesigner *self);

/**
 * clawt_agent_designer_get_files:
 * @self: a #ClawtAgentDesigner
 *
 * The workspace files the model drafted, by name.
 *
 * Only files the model actually wrote appear. Anything it left alone is
 * scaffolded from the defaults at commit, so a partial draft still
 * produces a complete workspace.
 *
 * Returns: (transfer none) (element-type utf8 utf8): name to content
 */
GHashTable *clawt_agent_designer_get_files(ClawtAgentDesigner *self);

/**
 * clawt_agent_designer_commit:
 * @self: a #ClawtAgentDesigner
 * @error: (out) (optional): return location for a #GError
 *
 * Adds the drafted agent to the configuration.
 *
 * Validated first and refused if it would not work, so a model that
 * drafted something impossible produces an error rather than an agent
 * that cannot start.
 *
 * Returns: (transfer none) (nullable): the new agent's configuration
 */
ClawtAgentConfig *clawt_agent_designer_commit(ClawtAgentDesigner  *self,
                                              GError             **error);

/**
 * clawt_agent_designer_get_transcript:
 * @self: a #ClawtAgentDesigner
 *
 * What the model said while designing, for showing progress.
 *
 * Returns: (transfer none) (nullable): the final text
 */
const gchar *clawt_agent_designer_get_transcript(ClawtAgentDesigner *self);

G_END_DECLS
