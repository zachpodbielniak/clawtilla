/*
 * clawt-fed-selector.h - One grammar for naming peers and agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `federation.rules`, the sidebar's filter, `clawtilla federation list
 * --filter` and an agent's `federation_reach` all say "these peers" or
 * "these agents".  They say it one way, so a rule and a filter that look
 * the same match the same things.
 *
 * A peer selector is terms joined by `+` (all must match), with `,`
 * between alternatives (any may match):
 *
 *   *               every peer
 *   name:GLOB       the alias or the label
 *   tier:TIER       owned, trusted, limited, untrusted
 *   owner:GLOB      the owner's display name
 *   tag:TAG         carries this tag
 *   health:HEALTH   live, offline, ...
 *   online          live or degraded
 *   !TERM           not TERM
 *
 * e.g. `tag:client-acme`, `tier:trusted+online`, `owner:alice*,tag:lab`.
 *
 * An agent selector names our own agents: `*`, an id glob, `team:NAME`,
 * `tag:TAG` (a tag on the agent's team), with `!` and `+` as above.
 *
 * Globs are g_pattern_match_simple(): `*` and `?`.  Matching is
 * case-insensitive, like every other name a person types here.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * clawt_fed_selector_check:
 * @selector: a peer selector
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses an empty term, an unknown key and an unknown tier or health,
 * naming the term -- a typo in a rule must not quietly match nothing.
 *
 * Returns: %TRUE if @selector is well formed
 */
gboolean clawt_fed_selector_check(const gchar  *selector,
                                  GError      **error);

/**
 * clawt_fed_selector_matches:
 * @selector: a peer selector
 * @peer: the peer: `alias`, `label`, `tier`, `owner`, `tags` (array of
 *   strings) and `health`, each optional
 *
 * A malformed selector matches nothing.
 *
 * Returns: %TRUE if @peer matches
 */
gboolean clawt_fed_selector_matches(const gchar *selector,
                                    JsonObject  *peer);

/**
 * clawt_fed_agent_selector_check:
 * @selector: an agent selector
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if @selector is well formed
 */
gboolean clawt_fed_agent_selector_check(const gchar  *selector,
                                        GError      **error);

/**
 * clawt_fed_agent_selector_matches:
 * @selector: an agent selector
 * @agent_id: the agent
 * @team: (nullable): its team
 * @tags: (array zero-terminated=1) (nullable): its team's tags
 *
 * Returns: %TRUE if the agent matches
 */
gboolean clawt_fed_agent_selector_matches(const gchar *selector,
                                          const gchar *agent_id,
                                          const gchar *team,
                                          gchar      **tags);

G_END_DECLS
