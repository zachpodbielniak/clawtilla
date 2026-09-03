/*
 * clawt-team.c - Who may hand work to whom
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-team.h"

ClawtTeamRole
clawt_team_role_of(ClawtAgentConfig *agent)
{
    if (agent == NULL)
        return CLAWT_TEAM_MEMBER;

    return (ClawtTeamRole)clawt_agent_config_get_enum(agent, "team_role");
}

ClawtTeamBadge
clawt_team_badge_for(gboolean chief_of_staff, const gchar *team_role)
{
    if (chief_of_staff)
        return CLAWT_TEAM_BADGE_CHIEF;

    /*
     * Resolved through the enum rather than compared against a spelled
     * out "lead".  A nickname written into a client is a nickname that
     * can drift from the one the type produces, and this codebase has
     * already drawn every completed task grey for exactly that -- a
     * comparison against two strings neither of which the enum emits,
     * reported by nothing, because a missing colour looks like a
     * design decision.
     */
    {
        gint value = 0;

        if (clawt_enum_from_nick(CLAWT_TYPE_TEAM_ROLE, team_role, &value) &&
            (ClawtTeamRole)value == CLAWT_TEAM_LEAD)
            return CLAWT_TEAM_BADGE_LEAD;
    }

    return CLAWT_TEAM_BADGE_NONE;
}

/* The team an agent belongs to, or NULL when it belongs to none. */
static const gchar *
team_of(ClawtAgentConfig *agent)
{
    const gchar *team;

    if (agent == NULL)
        return NULL;

    team = clawt_agent_config_get_string(agent, "team");

    return (team != NULL && *team != '\0') ? team : NULL;
}

gboolean
clawt_team_may_assign(ClawtAgentConfig  *from,
                      ClawtAgentConfig  *to,
                      gchar            **reason)
{
    const gchar *from_team;
    const gchar *to_team;

    if (reason != NULL)
        *reason = NULL;

    if (from == NULL || to == NULL) {
        if (reason != NULL)
            *reason = g_strdup("that agent is not in this fleet");
        return FALSE;
    }

    /*
     * Assigning to yourself is not an error, it is a no-op with extra
     * steps -- and an agent that does it has usually lost track of who
     * it is. Said plainly rather than allowed.
     */
    if (g_strcmp0(clawt_agent_config_get_id(from),
                  clawt_agent_config_get_id(to)) == 0) {
        if (reason != NULL)
            *reason = g_strdup("that is you: do the work rather than "
                               "delegating it to yourself");
        return FALSE;
    }

    /*
     * The chief of staff is the lead of every team. Dividing work across
     * them is the whole of the role, so it is not confined to one.
     */
    if (clawt_agent_config_get_boolean(from, "chief_of_staff"))
        return TRUE;

    from_team = team_of(from);
    to_team = team_of(to);

    if (clawt_team_role_of(from) != CLAWT_TEAM_LEAD) {
        if (reason != NULL)
            *reason = g_strdup_printf(
                "you are a member of your team, not its lead, so work is "
                "not yours to assign. You can still message %s, ask them a "
                "question, or share a room with them -- handing something "
                "over in conversation is not the same thing and is not "
                "restricted.",
                clawt_agent_config_get_id(to));
        return FALSE;
    }

    if (from_team == NULL) {
        if (reason != NULL)
            *reason = g_strdup("you are marked as a lead but belong to no "
                               "team, so there is nobody you lead. Set "
                               "agents.team, or ask the user to.");
        return FALSE;
    }

    if (g_strcmp0(from_team, to_team) != 0) {
        if (reason != NULL)
            *reason = g_strdup_printf(
                "%s is not on your team (%s), and a lead assigns only "
                "within their own. Send it to the chief of staff, who can "
                "hand it to the right team's lead -- or message %s "
                "directly if it is a question rather than a task.",
                clawt_agent_config_get_id(to), from_team,
                clawt_agent_config_get_id(to));
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_team_validate_fleet(ClawtConfig *config, GStrv *warnings)
{
    g_autoptr(GPtrArray) found = NULL;
    g_autoptr(GHashTable) leads = NULL;
    g_autoptr(GHashTable) members = NULL;
    g_autoptr(GPtrArray) teams = NULL;
    GPtrArray *agents;
    guint i;

    if (warnings != NULL)
        *warnings = NULL;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), TRUE);

    found = g_ptr_array_new_with_free_func(g_free);
    leads = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    members = g_hash_table_new(g_str_hash, g_str_equal);
    teams = clawt_config_get_teams(config);

    agents = clawt_config_get_agents(config);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
        const gchar *agent_id = clawt_agent_config_get_id(agent);
        const gchar *team = team_of(agent);
        guint j;
        gboolean known = FALSE;

        if (team == NULL)
            continue;

        for (j = 0; j < teams->len; j++) {
            ClawtTeamSpec *spec = g_ptr_array_index(teams, j);

            if (g_strcmp0(spec->id, team) == 0) {
                known = TRUE;
                break;
            }
        }

        /*
         * An agent naming a team that is not declared. It still runs and
         * still talks; it is simply in a group of one that nothing else
         * knows about, which is worth saying out loud because the usual
         * cause is a typo.
         */
        if (!known)
            g_ptr_array_add(found, g_strdup_printf(
                "agent '%s' is on team '%s', which is not declared under "
                "teams:", agent_id, team));

        g_hash_table_add(members, (gpointer)team);

        if (clawt_team_role_of(agent) != CLAWT_TEAM_LEAD)
            continue;

        /*
         * Two leads on one team. Not picked between: which of them may
         * assign is exactly the question, and answering it by whichever
         * happens to be first in the file is the sort of decision nobody
         * can see being made.
         */
        if (g_hash_table_contains(leads, team)) {
            g_ptr_array_add(found, g_strdup_printf(
                "team '%s' has two leads, '%s' and '%s'. Only one may "
                "assign work; make the other a member.",
                team, (const gchar *)g_hash_table_lookup(leads, team),
                agent_id));
            continue;
        }

        g_hash_table_insert(leads, g_strdup(team), g_strdup(agent_id));
    }

    /*
     * A team with members and no lead. Legitimate -- the chief of staff
     * can still assign into it -- but it is usually not what somebody
     * meant, so it is said once rather than discovered when a hand-off
     * goes nowhere.
     */
    for (i = 0; i < teams->len; i++) {
        ClawtTeamSpec *spec = g_ptr_array_index(teams, i);

        if (!g_hash_table_contains(members, spec->id))
            continue;

        if (!g_hash_table_contains(leads, spec->id))
            g_ptr_array_add(found, g_strdup_printf(
                "team '%s' has no lead, so only the chief of staff can "
                "assign work into it.", spec->id));
    }

    if (found->len == 0)
        return TRUE;

    if (warnings != NULL) {
        g_ptr_array_add(found, NULL);
        *warnings = (GStrv)g_ptr_array_free(g_steal_pointer(&found), FALSE);
    }

    return FALSE;
}
