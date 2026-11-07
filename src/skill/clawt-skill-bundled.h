/*
 * clawt-skill-bundled.h - Skills clawtilla ships with
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

#include <glib.h>

#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * CLAWT_SKILL_BUNDLED_WEB_RESEARCH:
 *
 * The skill that teaches the clawtilla_web_* tools and LQL.  Assigned to
 * every agent with a `libresearch` integration.
 */
#define CLAWT_SKILL_BUNDLED_WEB_RESEARCH "web-research"

/**
 * CLAWT_SKILL_BUNDLED_MARKER:
 *
 * The file in a skill directory that says clawtilla installed it.  It holds
 * a digest of what was written, so a newer clawtilla replaces its own copy
 * -- and only its own: a directory without the marker is somebody's, and
 * is left alone even when its name matches.
 */
#define CLAWT_SKILL_BUNDLED_MARKER ".clawtilla-bundled"

/**
 * clawt_skill_install_bundled:
 * @skills_dir: the fleet's skills directory
 * @error: return location for the first failure
 *
 * Writes every bundled skill into @skills_dir that is missing, or that
 * clawtilla wrote and has since changed.  A skill of the same name that
 * somebody wrote themselves wins, untouched.
 *
 * Returns: %TRUE when every bundled skill is in place or deliberately left
 */
gboolean clawt_skill_install_bundled(const gchar  *skills_dir,
                                     GError      **error);

/**
 * clawt_skill_bundled_file:
 * @path: a bundled file, relative to the skills directory, such as
 *   "web-research/references/lql.md"
 *
 * The text of one bundled file, for a place that needs it without the
 * skill -- an agent whose CLI does not load skills at all.
 *
 * Returns: (transfer none) (nullable): the text, or %NULL
 */
const gchar *clawt_skill_bundled_file(const gchar *path);

/**
 * clawt_skill_bundled_for_agent:
 * @config: the fleet configuration
 * @agent: an agent's configuration
 *
 * The bundled skills this agent gets without asking, because of what it
 * has rather than what it was assigned: `web-research` for an agent with a
 * `libresearch` integration.
 *
 * Returns: (transfer full) (nullable): a %NULL-terminated list of names
 */
GStrv clawt_skill_bundled_for_agent(ClawtConfig      *config,
                                    ClawtAgentConfig *agent);

G_END_DECLS
