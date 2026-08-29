/*
 * clawt-skill-commands.c - `/name` in clawtilla's own chat
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "skill/clawt-skill-commands.h"
#include "skill/clawt-skill-provision.h"

#include <string.h>

#include <ai-glib.h>

void
clawt_skill_command_free(ClawtSkillCommand *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->description);
    g_free(self->argument_hint);
    g_free(self->origin);
    g_free(self);
}

/*
 * A command set over one agent's workspace.
 *
 * The shell policy is pinned to NEVER, and the reason is two rules at
 * once.  ai-glib's default is opt-in: a resource declaring `shell: true`
 * has its `` !`cmd` `` backticks executed at resolve time.  Here that
 * would be a subprocess run on the daemon's main context from inside an
 * IPC handler, which is the thing this codebase has fixed five separate
 * times -- and it would let a file in a skills directory decide to run
 * code on the daemon's host, which is a permission nobody granted by
 * assigning a skill.  Expansion here produces text and nothing else.
 */
static AiCommandSet *
command_set_for(ClawtAgentConfig *agent)
{
    g_autoptr(AiResourceRegistry) registry = NULL;
    g_autofree gchar *workspace = NULL;
    AiCommandSet *set;

    workspace = clawt_agent_config_get_workspace(agent);

    if (workspace == NULL)
        return NULL;

    registry = ai_resource_registry_new();
    ai_resource_registry_set_working_directory(registry, workspace);
    ai_resource_registry_scan(registry);

    set = ai_command_set_new(registry);
    ai_command_set_set_shell_policy(set, AI_COMMAND_SHELL_NEVER);

    return set;
}

static gint
compare_commands(gconstpointer a, gconstpointer b)
{
    ClawtSkillCommand *const *x = a;
    ClawtSkillCommand *const *y = b;

    return g_strcmp0((*x)->name, (*y)->name);
}

GPtrArray *
clawt_skill_commands_list(ClawtAgentConfig *agent)
{
    g_autoptr(AiCommandSet) set = NULL;
    GPtrArray *out;
    GList *commands;
    GList *l;

    g_return_val_if_fail(agent != NULL, NULL);

    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_skill_command_free);

    set = command_set_for(agent);

    if (set == NULL)
        return out;

    commands = ai_command_set_list(set);

    for (l = commands; l != NULL; l = l->next) {
        AiCommand *command = l->data;
        ClawtSkillCommand *entry;
        const gchar *name = ai_command_get_name(command);

        /*
         * A resource whose name is not a valid command is skipped
         * rather than offered.  ai-glib names a namespaced file
         * `git:status`, which is a real command in claude's own
         * vocabulary and is not one clawtilla can round-trip: the name
         * comes back over IPC and is looked up again, and a colon is
         * not in the gate this subsystem validates against.
         */
        if (name == NULL || strchr(name, ':') != NULL ||
            strchr(name, '/') != NULL || strchr(name, ' ') != NULL)
            continue;

        entry = g_new0(ClawtSkillCommand, 1);
        entry->name = g_strdup(name);
        entry->description = g_strdup(ai_command_get_description(command));
        entry->argument_hint =
            g_strdup(ai_command_get_argument_hint(command));
        entry->origin = g_strdup(ai_command_get_origin(command));

        g_ptr_array_add(out, entry);
    }

    g_list_free_full(commands, g_object_unref);
    g_ptr_array_sort(out, compare_commands);

    return out;
}

gchar *
clawt_skill_commands_expand(ClawtAgentConfig  *agent,
                            const gchar       *name,
                            const gchar       *arguments,
                            GError           **error)
{
    g_autoptr(AiCommandSet) set = NULL;
    g_autoptr(AiCommandResult) result = NULL;
    g_autoptr(GError) local_error = NULL;
    g_autofree gchar *line = NULL;
    const gchar *bare;

    g_return_val_if_fail(agent != NULL, NULL);

    if (name == NULL || *name == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a command name is required");
        return NULL;
    }

    bare = (name[0] == '/') ? name + 1 : name;

    set = command_set_for(agent);

    if (set == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "this agent has no workspace to read commands "
                            "from");
        return NULL;
    }

    line = g_strdup_printf("/%s%s%s", bare,
                           (arguments != NULL && *arguments != '\0')
                               ? " " : "",
                           (arguments != NULL) ? arguments : "");

    /*
     * NULL cwd and NULL cancellable, which with the NEVER policy above
     * means no path through here can start a process.  Both are passed
     * explicitly rather than left to a default so that a later reader
     * can see the shell path is closed at both ends.
     */
    result = ai_command_set_resolve(set, line, NULL, NULL, &local_error);

    if (result == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            local_error != NULL
                                ? local_error->message
                                : "no such command");
        return NULL;
    }

    switch (ai_command_result_get_outcome(result)) {
    case AI_COMMAND_OUTCOME_PROMPT:
    case AI_COMMAND_OUTCOME_AGENT:
        return g_strdup(ai_command_result_get_prompt(result));

    case AI_COMMAND_OUTCOME_BUILTIN:
        /*
         * clawtilla registers no builtins with the set, so this can
         * only be reached if ai-glib gains one.  Named rather than
         * returned as an empty prompt, which would send an empty
         * message to an agent and cost a turn.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "'/%s' is handled by the CLI itself, not by clawtilla",
                    bare);
        return NULL;

    case AI_COMMAND_OUTCOME_NOT_A_COMMAND:
    default:
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "'/%s' is not a command this agent has", bare);
        return NULL;
    }
}

gboolean
clawt_skill_command_line_is_command(const gchar *line)
{
    if (line == NULL)
        return FALSE;

    return ai_command_set_is_command_line(line);
}
