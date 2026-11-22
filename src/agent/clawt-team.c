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

#include <string.h>

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

    /*
     * An ephemeral agent works for the agent that spawned it, whatever
     * either one's place in a team (0.55.0): a member that may spawn a
     * helper may hand it the work, which is the only reason it exists.
     */
    if (g_strcmp0(clawt_agent_config_get_ephemeral_string(to, "parent"),
                  clawt_agent_config_get_id(from)) == 0)
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

        /*
         * The ephemeral team needs none: each helper's parent assigns to
         * it (clawt_team_may_assign()), so "only the chief can assign
         * into it" would be a false thing to tell somebody.
         */
        if (clawt_team_spec_has_tag(spec, CLAWT_TEAM_TAG_EPHEMERAL))
            continue;

        if (!g_hash_table_contains(leads, spec->id))
            g_ptr_array_add(found, g_strdup_printf(
                "team '%s' has no lead, so only the chief of staff can "
                "assign work into it.", spec->id));
    }

    /*
     * Tags (0.55.0).  A malformed one is reported, not dropped: it is
     * still in the file, and a tag silently ignored is a feature that
     * silently does not apply.  A unique tag on a second team is named
     * with the team that keeps it, the one clawt_team_find_by_tag()
     * answers with, so the warning and the behaviour agree.
     */
    {
        g_autoptr(GHashTable) holder = g_hash_table_new(g_str_hash, g_str_equal);

        for (i = 0; i < teams->len; i++) {
            ClawtTeamSpec *spec = g_ptr_array_index(teams, i);
            guint t;

            for (t = 0; spec->tags != NULL && spec->tags[t] != NULL; t++) {
                const gchar *tag = spec->tags[t];
                const ClawtTeamTagInfo *info;
                const gchar *first;

                if (!clawt_team_tag_is_valid(tag)) {
                    g_ptr_array_add(found, g_strdup_printf(
                        "team '%s' has the tag '%s', which is not a tag: "
                        "lowercase letters, digits and hyphens, with at "
                        "most one vendor: prefix", spec->id, tag));
                    continue;
                }

                info = clawt_team_tag_lookup(tag);
                if (info == NULL || !info->unique)
                    continue;

                first = g_hash_table_lookup(holder, tag);
                if (first == NULL)
                    g_hash_table_insert(holder, (gpointer)tag, spec->id);
                else
                    g_ptr_array_add(found, g_strdup_printf(
                        "teams '%s' and '%s' both have the tag '%s', which "
                        "only one team may have; '%s' keeps it, so take it "
                        "off '%s'", first, spec->id, tag, first, spec->id));
            }
        }
    }

    if (found->len == 0)
        return TRUE;

    if (warnings != NULL) {
        g_ptr_array_add(found, NULL);
        *warnings = (GStrv)g_ptr_array_free(g_steal_pointer(&found), FALSE);
    }

    return FALSE;
}

/* ── Tags (0.55.0) ───────────────────────────────────────────────── */

/*
 * The tags a feature looks for.  One table, walked by every client and
 * by the docs test, so a tag added here is a tag every client offers --
 * and a tag is only added here with the feature that reads it.
 */
static const ClawtTeamTagInfo team_tags[] = {
    { CLAWT_TEAM_TAG_EPHEMERAL, "Ephemeral",
      "Where ephemeral agents go: the temporary helpers an agent spawns "
      "with clawtilla_ephemeral_spawn and reaps when they are done.",
      TRUE },
};

guint
clawt_team_tag_count(void)
{
    return G_N_ELEMENTS(team_tags);
}

const ClawtTeamTagInfo *
clawt_team_tag_nth(guint n)
{
    return n < G_N_ELEMENTS(team_tags) ? &team_tags[n] : NULL;
}

const ClawtTeamTagInfo *
clawt_team_tag_lookup(const gchar *name)
{
    guint i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < G_N_ELEMENTS(team_tags); i++)
        if (g_strcmp0(team_tags[i].name, name) == 0)
            return &team_tags[i];
    return NULL;
}

/* One `[a-z][a-z0-9-]*` part, not ending in a hyphen. */
static gboolean
tag_part_is_valid(const gchar *start, const gchar *end)
{
    const gchar *p;

    if (end <= start || !g_ascii_islower(*start) || end[-1] == '-')
        return FALSE;
    for (p = start; p < end; p++)
        if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '-')
            return FALSE;
    return TRUE;
}

gboolean
clawt_team_tag_is_valid(const gchar *name)
{
    const gchar *colon;
    gsize length;

    if (name == NULL)
        return FALSE;
    length = strlen(name);
    if (length == 0 || length > 64)
        return FALSE;

    colon = strchr(name, ':');
    if (colon == NULL)
        return tag_part_is_valid(name, name + length);
    if (strchr(colon + 1, ':') != NULL)
        return FALSE;
    return tag_part_is_valid(name, colon) &&
           tag_part_is_valid(colon + 1, name + length);
}

gboolean
clawt_team_spec_has_tag(const ClawtTeamSpec *team, const gchar *tag)
{
    return team != NULL && team->tags != NULL && tag != NULL &&
           g_strv_contains((const gchar *const *)team->tags, tag);
}

gchar *
clawt_team_find_by_tag(ClawtConfig *config, const gchar *tag)
{
    g_autoptr(GPtrArray) teams = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

    if (tag == NULL)
        return NULL;
    teams = clawt_config_get_teams(config);
    for (i = 0; i < teams->len; i++) {
        ClawtTeamSpec *spec = g_ptr_array_index(teams, i);

        if (clawt_team_spec_has_tag(spec, tag))
            return g_strdup(spec->id);
    }
    return NULL;
}

/* ── The helper badge (0.55.0) ───────────────────────────────────── */

static gboolean
member_true(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return FALSE;
    node = json_object_get_member(object, name);
    return JSON_NODE_HOLDS_VALUE(node) &&
           json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
           json_node_get_boolean(node);
}

gboolean
clawt_ephemeral_badge_for(JsonObject   *agent,
                          const gchar **out_tone,
                          gchar       **out_tooltip)
{
    gboolean done;
    const gchar *parent = NULL;

    if (out_tone != NULL)
        *out_tone = NULL;
    if (out_tooltip != NULL)
        *out_tooltip = NULL;
    if (!member_true(agent, "ephemeral"))
        return FALSE;

    done = member_true(agent, "awaiting_reap");
    if (json_object_has_member(agent, "parent") &&
        JSON_NODE_HOLDS_VALUE(json_object_get_member(agent, "parent")))
        parent = json_object_get_string_member(agent, "parent");

    if (out_tone != NULL)
        *out_tone = done ? "warn" : "neutral";
    if (out_tooltip != NULL)
        *out_tooltip = done
            ? g_strdup_printf("%s's ephemeral helper: its work is done and it is "
                              "waiting for %s to reap it",
                              parent != NULL ? parent : "nobody",
                              parent != NULL ? parent : "somebody")
            : g_strdup_printf("%s's ephemeral helper, there until %s reaps it",
                              parent != NULL ? parent : "nobody",
                              parent != NULL ? parent : "somebody");
    return TRUE;
}

gchar *
clawt_team_tags_label(JsonObject *team)
{
    GString *out = g_string_new(NULL);
    JsonArray *tags;
    guint i;

    if (team == NULL || !json_object_has_member(team, "tags") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(team, "tags")))
        return g_string_free(out, FALSE);

    tags = json_object_get_array_member(team, "tags");
    for (i = 0; i < json_array_get_length(tags); i++) {
        JsonNode *node = json_array_get_element(tags, i);

        if (!JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING)
            continue;
        g_string_append_printf(out, "%s%s", out->len > 0 ? ", " : "",
                               json_node_get_string(node));
    }
    return g_string_free(out, FALSE);
}
