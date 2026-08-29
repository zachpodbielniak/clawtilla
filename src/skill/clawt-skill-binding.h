/*
 * clawt-skill-binding.h - Which skills an agent gets, and why
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors #ClawtIntegrationBinding deliberately, down to the shape of
 * the resolver.  A skill can be named in three places -- `defaults`, a
 * team, an agent -- and everything downstream (the workspace links, the
 * TOOLS.org region, the clients' listings, the CLI) reads bindings and
 * never looks at which of the three it came from.  Two resolvers would
 * be two behaviours, and the one nobody tested would be the one that
 * ran.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "skill/clawt-skill.h"
#include "skill/clawt-skill-library.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_SKILL_BINDING (clawt_skill_binding_get_type())

GType clawt_skill_binding_get_type(void) G_GNUC_CONST;

/**
 * clawt_skill_binding_ref:
 * @self: a #ClawtSkillBinding
 *
 * Returns: (transfer full): @self, with one more reference
 */
ClawtSkillBinding *clawt_skill_binding_ref(ClawtSkillBinding *self);

/**
 * clawt_skill_binding_unref:
 * @self: (transfer full) (nullable): a #ClawtSkillBinding
 *
 * Drops a reference, freeing the binding at zero.
 */
void               clawt_skill_binding_unref(ClawtSkillBinding *self);

/**
 * clawt_skill_binding_get_name:
 * @self: a #ClawtSkillBinding
 *
 * Returns: (transfer none): the skill's name, whether or not it exists
 */
const gchar *clawt_skill_binding_get_name(ClawtSkillBinding *self);

/**
 * clawt_skill_binding_get_skill:
 * @self: a #ClawtSkillBinding
 *
 * The skill itself, when the library has one of that name.
 *
 * %NULL is not an error state to hide: a name in `agents.skills` that
 * matches nothing is a binding that reaches nobody, and saying so is
 * what the fleet rule about silent selectors requires.
 *
 * Returns: (transfer none) (nullable): the skill, or %NULL
 */
ClawtSkill *clawt_skill_binding_get_skill(ClawtSkillBinding *self);

/**
 * clawt_skill_binding_get_origin:
 * @self: a #ClawtSkillBinding
 *
 * Where the assignment was written: `agent`, `team` or `fleet`.
 *
 * Returns: (transfer none): the origin
 */
const gchar *clawt_skill_binding_get_origin(ClawtSkillBinding *self);

/**
 * clawt_skill_binding_is_active:
 * @self: a #ClawtSkillBinding
 *
 * Whether this binding actually puts anything in front of the agent.
 *
 * False for a name matching nothing and false for a skill that is
 * present but not enabled.  The provisioner asks this rather than
 * asking two questions of its own, because "assigned" and "in effect"
 * are different and a client showing only the first would say an agent
 * had a skill that reaches no prompt.
 *
 * Returns: %TRUE when the skill exists and is enabled
 */
gboolean clawt_skill_binding_is_active(ClawtSkillBinding *self);

/**
 * clawt_skill_resolve_for_agent:
 * @config: the fleet configuration
 * @agent: an agent's configuration
 * @library: (nullable): the skills on disk, or %NULL to resolve names only
 *
 * Every skill this agent has: its own list, its team's, and the fleet's.
 *
 * The **one** function that answers this.  Agent beats team beats
 * fleet, which only decides the recorded origin -- the three lists are
 * additive, so a skill named twice is one skill, counted once, and
 * attributed to the most specific place it was asked for.
 *
 * A name that matches no skill still produces a binding, with a %NULL
 * skill, so that a caller can say the selector reached nothing.  Silence
 * there is the failure the fleet rule exists to prevent.
 *
 * Returns: (transfer full) (element-type ClawtSkillBinding): the
 *   bindings, sorted by name
 */
GPtrArray *clawt_skill_resolve_for_agent(ClawtConfig       *config,
                                         ClawtAgentConfig  *agent,
                                         ClawtSkillLibrary *library);

/**
 * clawt_skill_bindings_warnings:
 * @bindings: (element-type ClawtSkillBinding): resolved bindings
 * @agent_id: whose bindings they are
 *
 * One sentence per assignment that reaches nothing.
 *
 * A half-built fleet is ordinary, so this is warnings rather than
 * errors.  But a selector matching nothing has to *say* so: an agent
 * quietly missing the procedure it was configured with is indis-
 * tinguishable from an agent that has it and ignored it.
 *
 * Returns: (transfer full) (element-type utf8): the messages
 */
GPtrArray *clawt_skill_bindings_warnings(GPtrArray   *bindings,
                                         const gchar *agent_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtSkillBinding, clawt_skill_binding_unref)

G_END_DECLS
