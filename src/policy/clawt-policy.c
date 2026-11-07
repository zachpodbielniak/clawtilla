/*
 * clawt-policy.c - Action rules: what an agent may do without asking
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Rules are strings, MATCH=MODE, in a list: the order of one list is the
 * whole answer, and a list of strings is something both clients and the
 * CLI already edit. The values are checked against the enums' own
 * nicknames, never a list kept here.
 */

#include "clawtilla.h"
#include "policy/clawt-policy.h"

#include <string.h>
#include <libreclaw.h>

gboolean
clawt_policy_parse_rule(const gchar      *rule,
                        gchar           **out_match,
                        ClawtPolicyMode  *out_mode,
                        GError          **error)
{
    const gchar *equals;
    g_autofree gchar *match = NULL;
    g_autofree gchar *mode_text = NULL;
    gint mode = 0;

    if (rule == NULL || (equals = strchr(rule, '=')) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a rule: write MATCH=MODE, such as exec=ask",
                    rule != NULL ? rule : "");
        return FALSE;
    }

    match = g_strstrip(g_strndup(rule, (gsize)(equals - rule)));
    mode_text = g_strstrip(g_strdup(equals + 1));

    if (*match == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' matches nothing: name an action class or a tool", rule);
        return FALSE;
    }

    if (!clawt_enum_from_nick(CLAWT_TYPE_POLICY_MODE, mode_text, &mode)) {
        g_autofree gchar *allowed = clawt_enum_nick_list(CLAWT_TYPE_POLICY_MODE);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s': '%s' is not a mode; it takes one of %s",
                    rule, mode_text, allowed);
        return FALSE;
    }

    if (out_match != NULL)
        *out_match = g_steal_pointer(&match);
    if (out_mode != NULL)
        *out_mode = (ClawtPolicyMode)mode;
    return TRUE;
}

gboolean
clawt_policy_validate_rules(const gchar * const *rules, GError **error)
{
    guint i;

    for (i = 0; rules != NULL && rules[i] != NULL; i++)
        if (!clawt_policy_parse_rule(rules[i], NULL, NULL, error))
            return FALSE;

    return TRUE;
}

/* A class nickname matches its class; anything else is a glob on the tool. */
static gboolean
rule_matches(const gchar *match, const gchar *tool, ClawtActionClass action)
{
    gint klass = 0;

    if (clawt_enum_from_nick(CLAWT_TYPE_ACTION_CLASS, match, &klass))
        return (ClawtActionClass)klass == action;

    return tool != NULL && g_pattern_match_simple(match, tool);
}

ClawtPolicyMode
clawt_policy_mode_for(ClawtAgentConfig  *agent,
                      const gchar       *tool,
                      ClawtActionClass   action,
                      gchar            **out_rule)
{
    g_auto(GStrv) rules = NULL;
    guint i;

    if (out_rule != NULL)
        *out_rule = NULL;
    if (agent == NULL)
        return CLAWT_POLICY_ALLOW;

    rules = clawt_agent_config_get_string_list(agent, "policy.rules");

    for (i = 0; rules != NULL && rules[i] != NULL; i++) {
        g_autofree gchar *match = NULL;
        ClawtPolicyMode mode = CLAWT_POLICY_ALLOW;

        /*
         * An unparsable rule is skipped here because validation has
         * already shadowed the agent that holds it; a shadow runs no
         * tools, so this is only ever reached for a rule a hand edit
         * broke after load -- and skipping one is safer than refusing
         * every call over a typo nobody can see from the agent's side.
         */
        if (!clawt_policy_parse_rule(rules[i], &match, &mode, NULL))
            continue;

        if (rule_matches(match, tool, action)) {
            if (out_rule != NULL)
                *out_rule = g_strdup(rules[i]);
            return mode;
        }
    }

    return CLAWT_POLICY_ALLOW;
}

gboolean
clawt_policy_agent_has_rules(ClawtAgentConfig *agent)
{
    g_auto(GStrv) rules = NULL;

    if (agent == NULL)
        return FALSE;

    rules = clawt_agent_config_get_string_list(agent, "policy.rules");
    return rules != NULL && rules[0] != NULL;
}

gboolean
clawt_policy_routine_is_read_only(ClawtRoutine *routine)
{
    return routine != NULL &&
           (clawt_routine_get_boolean(routine, "read_only") ||
            clawt_routine_get_boolean(routine, "proactive"));
}

gboolean
clawt_policy_agent_needs_hook(ClawtAgentConfig *agent)
{
    ClawtConfig *fleet;
    GPtrArray *routines;
    guint i;

    if (clawt_policy_agent_has_rules(agent))
        return TRUE;

    fleet = agent != NULL ? clawt_agent_config_get_config(agent) : NULL;
    routines = fleet != NULL ? clawt_config_get_routines(fleet) : NULL;
    for (i = 0; routines != NULL && i < routines->len; i++) {
        ClawtRoutine *routine = g_ptr_array_index(routines, i);

        if (clawt_policy_routine_is_read_only(routine) &&
            g_strcmp0(clawt_routine_get_string(routine, "agent"),
                      clawt_agent_config_get_id(agent)) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Claude Code's own tool names. This is the one list here, because the
 * names are the CLI's and nothing in clawtilla can enumerate them; an
 * unknown name falls to EXEC, so a tool added to the CLI later is asked
 * about rather than waved through.
 */
static const struct {
    const gchar      *name;
    ClawtActionClass  action;
} NATIVE_TOOLS[] = {
    { "Read",         CLAWT_ACTION_READ },
    { "Glob",         CLAWT_ACTION_READ },
    { "Grep",         CLAWT_ACTION_READ },
    { "LS",           CLAWT_ACTION_READ },
    { "WebFetch",     CLAWT_ACTION_READ },
    { "WebSearch",    CLAWT_ACTION_READ },
    { "TodoRead",     CLAWT_ACTION_READ },
    { "TodoWrite",    CLAWT_ACTION_READ },
    { "Write",        CLAWT_ACTION_WRITE },
    { "Edit",         CLAWT_ACTION_WRITE },
    { "MultiEdit",    CLAWT_ACTION_WRITE },
    { "NotebookEdit", CLAWT_ACTION_WRITE },
    { "Bash",         CLAWT_ACTION_EXEC },
    { "Task",         CLAWT_ACTION_EXEC }
};

ClawtActionClass
clawt_policy_class_for_native_tool(const gchar *tool)
{
    gsize i;

    if (tool == NULL)
        return CLAWT_ACTION_EXEC;

    for (i = 0; i < G_N_ELEMENTS(NATIVE_TOOLS); i++)
        if (g_strcmp0(NATIVE_TOOLS[i].name, tool) == 0)
            return NATIVE_TOOLS[i].action;

    if (g_str_has_prefix(tool, "mcp__"))
        return CLAWT_ACTION_EXTERNAL;

    return CLAWT_ACTION_EXEC;
}

gboolean
clawt_policy_provider_gates_native_tools(const gchar *provider)
{
    /* Normalised as libreclaw does, since that decides which CLI runs. */
    const gchar *cli = lc_provider_type_normalize(provider, "agent");

    return g_strcmp0(cli, "claude-code") == 0 || g_strcmp0(cli, "claude-tmux") == 0;
}

gboolean
clawt_policy_native_is_clawtilla(const gchar *tool)
{
    return tool != NULL && g_str_has_prefix(tool, "mcp__clawtilla__");
}
