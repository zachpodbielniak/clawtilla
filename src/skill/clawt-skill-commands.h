/*
 * clawt-skill-commands.h - `/name` in clawtilla's own chat
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The expansion happens **here, in the daemon**, and never in a client.
 * Both clients then send byte-identical text for the same `/name args`,
 * which is the only way the two can be said to have the same feature --
 * and there is one implementation of argument substitution rather than
 * two that agree until somebody types a `$`.
 *
 * ai-glib's #AiCommandSet already does all of this over a registry: it
 * turns a skill into a command, knows its description and argument hint,
 * and substitutes `$ARGUMENTS` and `$1`..`$9`.  clawtilla points it at
 * an agent's workspace and asks.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * ClawtSkillCommand:
 * @name: what to type after the slash
 * @description: one line, for the completion popup
 * @argument_hint: (nullable): what to type after the name
 * @origin: which harness's directory it was found in
 *
 * One `/name` an agent's workspace offers.
 */
typedef struct {
    gchar *name;
    gchar *description;
    gchar *argument_hint;
    gchar *origin;
} ClawtSkillCommand;

void clawt_skill_command_free(ClawtSkillCommand *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtSkillCommand, clawt_skill_command_free)

/**
 * clawt_skill_commands_list:
 * @agent: the agent whose workspace to look in
 *
 * Every `/name` this agent's workspace offers, sorted by name.
 *
 * Read from the workspace rather than from the bindings, deliberately:
 * a skill somebody put in the workspace by hand is a command that
 * genuinely works, and a listing that only knew about clawtilla's own
 * links would offer a smaller set than the harness actually has.
 *
 * Returns: (transfer full) (element-type ClawtSkillCommand): the
 *   commands
 */
GPtrArray *clawt_skill_commands_list(ClawtAgentConfig *agent);

/**
 * clawt_skill_commands_expand:
 * @agent: the agent whose workspace to look in
 * @name: the command, with or without its leading slash
 * @arguments: (nullable): everything typed after it
 * @error: (out) (optional): return location for a #GError
 *
 * The text to send, with @arguments substituted.
 *
 * Substitution is ai-glib's, which walks the body and replaces the
 * placeholders it knows.  Nothing here reaches `printf`: a skill body is
 * a file somebody else wrote, and a `%s` in it is a percent and an ess.
 *
 * Returns: (transfer full) (nullable): the prompt, or %NULL with @error
 */
gchar *clawt_skill_commands_expand(ClawtAgentConfig  *agent,
                                   const gchar       *name,
                                   const gchar       *arguments,
                                   GError           **error);

/**
 * clawt_skill_command_line_is_command:
 * @line: (nullable): what somebody typed
 *
 * Whether @line looks like `/something` rather than a message.
 *
 * Returns: %TRUE when it should be resolved as a command
 */
gboolean clawt_skill_command_line_is_command(const gchar *line);

G_END_DECLS
