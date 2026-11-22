/*
 * clawt-team.h - Who may hand work to whom
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
#include <json-glib/json-glib.h>

#include "clawt-enums.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * clawt_team_role_of:
 * @agent: (nullable): an agent's configuration
 *
 * The agent's standing in its team.
 *
 * Returns: its role, or %CLAWT_TEAM_MEMBER when it has none
 */
ClawtTeamRole clawt_team_role_of(ClawtAgentConfig *agent);

/**
 * clawt_team_may_assign:
 * @from: (nullable): the agent that wants to hand work over
 * @to: (nullable): the agent it wants to hand work to
 * @reason: (out) (optional) (transfer full): why not, when the answer is
 *   %FALSE -- written for the agent that asked, so it stops rather than
 *   looking for another way round
 *
 * Whether @from may put work on @to's list.
 *
 * Three rules and no more. The chief of staff may assign to anybody: it
 * is the lead of every team, and the point of it is dividing work across
 * them. A team lead may assign within its own team and nowhere else.
 * Everybody else may assign to nobody.
 *
 * This is deliberately not about *talking*. Any agent may message, ask
 * and share a room with any other -- handing something over in
 * conversation is collaboration, and the fleet would be much less useful
 * without it. What is gated here is putting a task on somebody's list.
 *
 * Pure, so the rule can be exercised without a daemon, a fleet or a
 * running agent. A permission check that needs all three to test is a
 * permission check that gets tested once.
 *
 * Returns: %TRUE when the assignment is allowed
 */
gboolean clawt_team_may_assign(ClawtAgentConfig  *from,
                               ClawtAgentConfig  *to,
                               gchar            **reason);

/**
 * ClawtTeamBadge:
 * @CLAWT_TEAM_BADGE_NONE: nothing to say -- an ordinary member
 * @CLAWT_TEAM_BADGE_LEAD: may hand work to its own team
 * @CLAWT_TEAM_BADGE_CHIEF: may hand work to anybody
 *
 * Which standing a client draws beside an agent's name in a list.
 */
typedef enum {
    CLAWT_TEAM_BADGE_NONE = 0,
    CLAWT_TEAM_BADGE_LEAD,
    CLAWT_TEAM_BADGE_CHIEF
} ClawtTeamBadge;

/**
 * clawt_team_badge_for:
 * @chief_of_staff: `agent.list`'s own `chief_of_staff` boolean
 * @team_role: (nullable): `agent.list`'s own `team_role` nick, or %NULL
 *
 * Which of the two standings, if either, a row should say out loud.
 *
 * Here rather than in each client for the reason this codebase keeps
 * relearning: a rule both clients apply is one they will eventually
 * apply differently, and this one is invisible when it goes wrong -- a
 * badge nobody draws looks exactly like an agent that does not have
 * the role.  That is how `team_role` came to be in the daemon's reply
 * from the day the sidebar learned to group by team, and drawn by
 * neither client for as long: the chief was marked and every lead
 * under it was not.
 *
 * Never both.  The chief of staff is the lead of every team, so an
 * agent that is both is completely described by the stronger of the
 * two, and drawing the weaker one beside it reads as a second fact
 * rather than as the same one.
 *
 * Takes the reply's own spellings rather than a #ClawtAgentConfig,
 * because the callers are clients and a client has the JSON and not
 * the fleet.  A @team_role the daemon did not send -- or one from a
 * daemon newer than this build -- is %CLAWT_TEAM_BADGE_NONE, which is
 * the answer that claims least.
 *
 * Returns: the badge to draw
 */
ClawtTeamBadge clawt_team_badge_for(gboolean     chief_of_staff,
                                    const gchar *team_role);

/**
 * clawt_team_validate_fleet:
 * @config: the fleet configuration
 * @warnings: (out) (optional) (transfer full) (array zero-terminated=1):
 *   what is wrong, one line each
 *
 * Checks the things that can only be seen with the whole fleet in view:
 * a team with two leads, an agent naming a team that does not exist, a
 * team with a lead and nobody to lead, a tag that cannot be one, and a
 * unique tag on two teams.
 *
 * Warnings rather than errors. A fleet is edited by hand and half-built
 * states are ordinary; refusing to start over one would be far worse
 * than saying so.
 *
 * Returns: %TRUE when nothing is wrong
 */
gboolean clawt_team_validate_fleet(ClawtConfig  *config,
                                   GStrv        *warnings);

/**
 * CLAWT_TEAM_TAG_EPHEMERAL:
 *
 * The team ephemeral agents join (since 0.55.0).
 */
#define CLAWT_TEAM_TAG_EPHEMERAL "ephemeral"

/**
 * ClawtTeamTagInfo:
 * @name: the tag, as written in `teams.tags`
 * @label: how to show it to a person
 * @description: what giving a team this tag does
 * @unique: whether only one team may carry it
 *
 * A tag some feature of clawtilla looks for.
 */
typedef struct {
    const gchar *name;
    const gchar *label;
    const gchar *description;
    gboolean     unique;
} ClawtTeamTagInfo;

/**
 * clawt_team_tag_count:
 *
 * Returns: how many well-known tags there are
 */
guint clawt_team_tag_count(void);

/**
 * clawt_team_tag_nth:
 * @n: an index below clawt_team_tag_count()
 *
 * The well-known tags, in the order to show them.  Walk this rather
 * than keeping a list: a client's copy is a copy that drifts.
 *
 * Returns: (transfer none) (nullable): the tag, or %NULL past the end
 */
const ClawtTeamTagInfo *clawt_team_tag_nth(guint n);

/**
 * clawt_team_tag_lookup:
 * @name: (nullable): a tag
 *
 * Returns: (transfer none) (nullable): @name's entry when it is
 *   well-known, else %NULL -- which is not an error: a tag nobody here
 *   knows is a person's or a plugin's
 */
const ClawtTeamTagInfo *clawt_team_tag_lookup(const gchar *name);

/**
 * clawt_team_tag_is_valid:
 * @name: (nullable): a tag
 *
 * Lowercase letters, digits and hyphens, starting with a letter, with
 * at most one `vendor:` prefix of the same shape: `ephemeral`,
 * `acme:night-shift`.  Kept narrow so a tag is safe in a file name, a
 * URL and a chip, and cannot be confused with a team id containing it.
 *
 * Returns: %TRUE when @name can be a tag
 */
gboolean clawt_team_tag_is_valid(const gchar *name);

/**
 * clawt_team_spec_has_tag:
 * @team: a team
 * @tag: a tag
 *
 * Returns: %TRUE when @team carries @tag
 */
gboolean clawt_team_spec_has_tag(const ClawtTeamSpec *team, const gchar *tag);

/**
 * clawt_team_find_by_tag:
 * @config: the fleet configuration
 * @tag: a tag
 *
 * The team a feature means by @tag: the first in the team list (by
 * `order`, then file order) that carries it.  A second team carrying a
 * unique tag is reported by clawt_team_validate_fleet() and otherwise
 * ignored, so the answer here and the warning there name the same team.
 *
 * Returns: (transfer full) (nullable): the team's id, or %NULL when no
 *   team carries @tag
 */
gchar *clawt_team_find_by_tag(ClawtConfig *config, const gchar *tag);

/**
 * clawt_team_tags_label:
 * @team: (nullable): a team from `team.list`
 *
 * The team's tags, comma separated, in the order written -- what a team
 * editor's Tags field shows and what `team.set` reads back from one.
 *
 * Returns: (transfer full): the tags, or an empty string for none
 */
gchar *clawt_team_tags_label(JsonObject *team);

/**
 * clawt_ephemeral_badge_for:
 * @agent: (nullable): an agent from `agent.list`
 * @out_tone: (out) (optional) (transfer none): the badge's tone, `warn`
 *   when its work is done and it is waiting to be reaped, else `neutral`
 * @out_tooltip: (out) (optional) (transfer full): whose helper it is
 *   and what state it is in, for a tooltip or a row's subtitle
 *
 * Whether to badge @agent as an ephemeral helper (since 0.55.0), from
 * `agent.list`'s `ephemeral`, `parent` and `awaiting_reap` -- one answer
 * for every client's sidebar.  The badge's text is "HELPER".
 *
 * Returns: %TRUE when @agent is ephemeral
 */
gboolean clawt_ephemeral_badge_for(JsonObject   *agent,
                                   const gchar **out_tone,
                                   gchar       **out_tooltip);

G_END_DECLS
