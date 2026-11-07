/*
 * clawt-policy.h - Action rules: what an agent may do without asking
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
 * clawt_policy_parse_rule:
 * @rule: one rule, `MATCH=MODE`
 * @out_match: (out) (optional) (transfer full): the match: an action class
 *   nickname or a tool-name glob
 * @out_mode: (out) (optional): the mode
 * @error: return location for why the rule is not one
 *
 * Returns: %TRUE when @rule is a rule
 */
gboolean clawt_policy_parse_rule(const gchar      *rule,
                                 gchar           **out_match,
                                 ClawtPolicyMode  *out_mode,
                                 GError          **error);

/**
 * clawt_policy_validate_rules:
 * @rules: (nullable) (array zero-terminated=1): the rules
 * @error: return location for the first rule that is not one
 *
 * Returns: %TRUE when every rule parses
 */
gboolean clawt_policy_validate_rules(const gchar * const  *rules,
                                     GError              **error);

/**
 * clawt_policy_mode_for:
 * @agent: the agent making the call
 * @tool: the tool's name as the agent called it
 * @action: what kind of thing the call does
 * @out_rule: (out) (optional) (transfer full) (nullable): the rule that
 *   decided, or %NULL when none matched
 *
 * The agent's rules (or the fleet's, when the agent has none), first match
 * wins. A rule matches when its MATCH is @action's nickname or a glob that
 * matches @tool. No match is %CLAWT_POLICY_ALLOW: rules are for what a
 * fleet wants to stop, and a fleet with none behaves as before there were
 * any.
 *
 * Returns: the mode
 */
ClawtPolicyMode clawt_policy_mode_for(ClawtAgentConfig  *agent,
                                      const gchar       *tool,
                                      ClawtActionClass   action,
                                      gchar            **out_rule);

/**
 * clawt_policy_agent_has_rules:
 * @agent: an agent
 *
 * Whether any rule applies to @agent at all -- what decides whether its
 * CLI is handed the PreToolUse hook, so an agent with no rules pays
 * nothing.
 *
 * Returns: %TRUE when the agent or the fleet has rules
 */
gboolean clawt_policy_agent_has_rules(ClawtAgentConfig *agent);

/**
 * clawt_policy_class_for_native_tool:
 * @tool: a tool name as an agent CLI reports it (Claude Code's `Bash`,
 *   `Write`, `mcp__server__tool`, ...)
 *
 * The class of one of the CLI's own tools. A name this does not know is
 * %CLAWT_ACTION_EXEC, and a third party's MCP tool %CLAWT_ACTION_EXTERNAL:
 * never read, because a guess must fall on the side that asks.
 *
 * Returns: the class
 */
ClawtActionClass clawt_policy_class_for_native_tool(const gchar *tool);

/**
 * clawt_policy_native_is_clawtilla:
 * @tool: a tool name as an agent CLI reports it
 *
 * Whether @tool is one of clawtilla's own, reached through the CLI's MCP
 * client (`mcp__clawtilla__clawtilla_*`). The daemon gates those itself
 * when they arrive, so the CLI hook lets them through rather than asking
 * twice.
 *
 * Returns: %TRUE for clawtilla's own tools
 */
gboolean clawt_policy_native_is_clawtilla(const gchar *tool);

G_END_DECLS
