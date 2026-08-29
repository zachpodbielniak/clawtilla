/*
 * clawt-skill-provision.h - Putting an agent's skills where its CLI looks
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every path here is **asked of ai-glib's resource registry**, never
 * written down.  That is not tidiness: the five harnesses genuinely
 * disagree about where a skill lives, and the disagreement is not the
 * shape anyone expects.  `~/.claude/skills` is read by four of the five
 * and `~/.agents/skills` by three -- but not the same three -- so
 * neither is universal, and a constant in this file would silently
 * install nothing for whichever harnesses it left out.  The registry
 * knows; clawtilla asks.
 *
 * Antigravity is the exception that proves the rule and is *not* a
 * symlink.  Symlink support there is undocumented and untestable, and
 * the vendor shipped a first-class indirection instead --
 * `.agents/skills.json` with `entries[].path`.  Using the mechanism they
 * built is more likely to keep working than relying on behaviour nobody
 * has written down.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "skill/clawt-skill-binding.h"
#include "skill/clawt-skill-library.h"

G_BEGIN_DECLS

/**
 * clawt_skill_provider_origin:
 * @provider: an `agents.model.provider` value, or %NULL
 *
 * Which harness's directories to write into.
 *
 * Goes through `lc_provider_type_normalize()` first, so an agent
 * configured for something libreclaw does not drive gets the same
 * answer libreclaw will actually give it.  Anything else would provision
 * skills for a CLI that is not the one being run -- the harness would
 * find nothing and nothing would say why.
 *
 * Returns: (transfer none): the registry origin, e.g. `claude`
 */
const gchar *clawt_skill_provider_origin(const gchar *provider);

/**
 * clawt_skill_provision:
 * @config: (nullable): the fleet configuration
 * @agent: the agent's configuration
 * @library: the skills on disk
 * @warnings: (out) (optional) (element-type utf8) (transfer full): what
 *   was left alone, and why
 * @error: (out) (optional): return location for a #GError
 *
 * Writes this agent's skill links into its workspace, and takes away the
 * ones it should no longer have.
 *
 * **Ownership discipline**, the same as `.mcp.json`'s: clawtilla removes
 * only what clawtilla put there.  A link is recognised as ours by where
 * it *points* -- into the library's own directory -- rather than by a
 * record kept somewhere else, because a record and a filesystem drift
 * and the drift is silent.  A real directory sitting at one of these
 * paths is somebody's work: it is left exactly as it is, and reported.
 *
 * A **dangling** link is repaired or removed on every pass, never left.
 * A broken symlink enumerates as a symlink and is then skipped in
 * silence by every reader, which is precisely how the GNOME extensions
 * bug presented: the directory looked right, the tool reported healthy,
 * and every real call failed.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_skill_provision(ClawtConfig        *config,
                               ClawtAgentConfig   *agent,
                               ClawtSkillLibrary  *library,
                               GPtrArray         **warnings,
                               GError            **error);

/**
 * clawt_skill_provision_paths:
 * @agent: the agent's configuration
 * @kind_commands: %TRUE for the commands directory, %FALSE for skills
 *
 * Where this agent's provider looks, inside this agent's workspace.
 *
 * Exposed so a client and a test can say where a link ought to be
 * without either of them keeping its own copy of the answer.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the
 *   directories, most preferred first, or %NULL when the provider has no
 *   such concept
 */
GStrv clawt_skill_provision_paths(ClawtAgentConfig *agent,
                                  gboolean          kind_commands);

/**
 * clawt_skill_provision_describe:
 * @bindings: (element-type ClawtSkillBinding): resolved bindings
 *
 * The org text for `TOOLS.org`'s skills region.
 *
 * Written from the agent's side: not "the fleet has six skills" but
 * "here is what you know how to do, and what to type to use it".  An
 * agent believes its own files over a tool listing, because the file is
 * in the prompt and a listing is something it has to decide to ask for.
 *
 * Returns: (transfer full): the section body, without the markers
 */
gchar *clawt_skill_provision_describe(GPtrArray *bindings);

G_END_DECLS
