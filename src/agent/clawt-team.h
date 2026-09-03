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
 * a team with two leads, an agent naming a team that does not exist, and
 * a team with a lead and nobody to lead.
 *
 * Warnings rather than errors. A fleet is edited by hand and half-built
 * states are ordinary; refusing to start over one would be far worse
 * than saying so.
 *
 * Returns: %TRUE when nothing is wrong
 */
gboolean clawt_team_validate_fleet(ClawtConfig  *config,
                                   GStrv        *warnings);

G_END_DECLS
