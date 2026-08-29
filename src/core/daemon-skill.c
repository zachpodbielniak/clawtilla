/*
 * daemon-skill.c - The client surface: skill.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Nothing here leaves the machine or waits on a subprocess, so no verb
 * defers.  That is worth stating rather than assuming: `skill.expand`
 * looks like the sort of thing that might run something, and it
 * deliberately cannot -- the command set it goes through has its shell
 * policy pinned off, so expansion is text substitution and returns at
 * once.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * One skill, as a client sees it.
 *
 * The warnings and the skipped files are in every listing rather than
 * only in `skill.show`, because they are the whole reason a person is
 * looking at the list: an imported skill arrives disabled with a
 * warning attached, and a listing that made you open each one to find
 * out which is a listing nobody reads twice.
 */
static void
add_skill_object(JsonBuilder *builder, ClawtSkill *skill, gboolean with_body)
{
    GPtrArray *warnings;
    GPtrArray *skipped;
    guint i;

    json_builder_begin_object(builder);
    clawt_daemon_add_string_member(builder, "name",
                                   clawt_skill_get_name(skill));
    clawt_daemon_add_string_member(builder, "description",
                                   clawt_skill_get_description(skill));
    clawt_daemon_add_string_member(builder, "directory",
                                   clawt_skill_get_directory(skill));
    clawt_daemon_add_string_member(builder, "origin",
                                   clawt_skill_get_origin_url(skill));
    clawt_daemon_add_string_member(builder, "sha256",
                                   clawt_skill_get_digest(skill));
    clawt_daemon_add_string_member(
        builder, "source",
        clawt_enum_to_nick(CLAWT_TYPE_SKILL_SOURCE,
                           (gint)clawt_skill_get_source(skill)));

    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, clawt_skill_get_enabled(skill));

    json_builder_set_member_name(builder, "imported_at");
    json_builder_add_int_value(builder, clawt_skill_get_imported_at(skill));

    warnings = clawt_skill_get_warnings(skill);
    json_builder_set_member_name(builder, "warnings");
    json_builder_begin_array(builder);

    for (i = 0; warnings != NULL && i < warnings->len; i++)
        json_builder_add_string_value(builder,
                                      g_ptr_array_index(warnings, i));

    json_builder_end_array(builder);

    skipped = clawt_skill_get_skipped(skill);
    json_builder_set_member_name(builder, "skipped");
    json_builder_begin_array(builder);

    for (i = 0; skipped != NULL && i < skipped->len; i++)
        json_builder_add_string_value(builder, g_ptr_array_index(skipped, i));

    json_builder_end_array(builder);

    if (with_body)
        clawt_daemon_add_string_member(builder, "body",
                                       clawt_skill_get_body(skill));

    json_builder_end_object(builder);
}

/*
 * The library, or a refusal that says which of the two reasons it is.
 *
 * "Skills are turned off" and "no directory is configured" send a
 * reader to entirely different places, and answering both with an
 * empty list would send them to neither.
 */
static ClawtSkillLibrary *
library_or_error(ClawtDaemon *self, JsonNode *request, JsonNode **error_out)
{
    *error_out = NULL;

    if (self->skills != NULL)
        return self->skills;

    if (self->config != NULL &&
        !clawt_config_get_boolean(self->config, "skills.enabled")) {
        *error_out = clawt_ipc_error_new(
            request, CLAWT_ERROR_NOT_SUPPORTED,
            "skills are turned off for this fleet; set skills.enabled");
        return NULL;
    }

    *error_out = clawt_ipc_error_new(
        request, CLAWT_ERROR_CONFIG_INVALID,
        "no skills directory is configured; set skills.dir");

    return NULL;
}

/*
 * Read a skill name out of a payload, decoded before it is validated.
 *
 * Every verb below goes through this rather than reading the member
 * itself.  A name is joined onto a directory on every path in this
 * subsystem, so the gate belongs at the one place they all pass rather
 * than at each of them -- the shape this codebase has already paid for
 * five times over with a rule enforced at one call site.
 */
static gchar *
payload_skill_name(JsonObject *payload, const gchar *member, GError **error)
{
    const gchar *raw = clawt_ipc_payload_string(payload, member);

    if (raw == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "which skill? '%s' is required", member);
        return NULL;
    }

    return clawt_skill_name_from_wire(raw, error);
}

/*
 * Apply one change to a list of skill names, wherever that list lives.
 *
 * Assignment can be written in three places and the three are read by
 * one resolver, so they are written by one function too: three copies
 * of "read the list, add or remove a name, write it back" is three
 * chances to write a scalar where the schema says sequence.
 */
static gboolean
edit_name_list(GStrv        current,
               const gchar *name,
               gboolean     add,
               GStrv       *out)
{
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    gboolean changed = FALSE;
    gsize i;

    for (i = 0; current != NULL && current[i] != NULL; i++) {
        if (g_strcmp0(current[i], name) == 0) {
            if (add) {
                /*
                 * Already there.  Reported as no change rather than as
                 * success, so a client can say "it already had that"
                 * instead of claiming to have done something.
                 */
                g_ptr_array_free(values, TRUE);
                *out = NULL;
                return FALSE;
            }

            changed = TRUE;
            continue;
        }

        g_ptr_array_add(values, g_strdup(current[i]));
    }

    if (add) {
        g_ptr_array_add(values, g_strdup(name));
        changed = TRUE;
    }

    if (!changed) {
        g_ptr_array_free(values, TRUE);
        *out = NULL;
        return FALSE;
    }

    g_ptr_array_add(values, NULL);
    *out = (GStrv)g_ptr_array_free(values, FALSE);

    return TRUE;
}

JsonNode *
clawt_daemon_handle_skill(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    if (g_strcmp0(kind, "skill.list") == 0) {
        g_autoptr(GPtrArray) skills = NULL;
        GPtrArray *problems;
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "skills");
        json_builder_begin_array(builder);

        if (self->skills != NULL) {
            skills = clawt_skill_library_list(self->skills);

            for (i = 0; i < skills->len; i++)
                add_skill_object(builder, g_ptr_array_index(skills, i),
                                 FALSE);
        }

        json_builder_end_array(builder);

        /*
         * An empty list has two causes and they are not the same
         * problem, so the reply says which.  Anywhere an empty result
         * could read as an answer, say why it is empty.
         */
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(builder, self->skills != NULL);

        clawt_daemon_add_string_member(
            builder, "directory",
            self->skills != NULL
                ? clawt_skill_library_get_directory(self->skills) : NULL);

        json_builder_set_member_name(builder, "problems");
        json_builder_begin_array(builder);

        problems = (self->skills != NULL)
                   ? clawt_skill_library_get_problems(self->skills) : NULL;

        for (i = 0; problems != NULL && i < problems->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(problems, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.show") == 0) {
        g_autofree gchar *name = NULL;
        ClawtSkillLibrary *library;
        ClawtSkill *skill;
        JsonNode *refusal = NULL;

        library = library_or_error(self, request, &refusal);

        if (library == NULL)
            return refusal;

        name = payload_skill_name(payload, "name", &error);

        if (name == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        skill = clawt_skill_library_lookup(library, name);

        if (skill == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no skill of that name");

        add_skill_object(builder, skill, TRUE);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.create") == 0) {
        g_autofree gchar *name = NULL;
        ClawtSkillLibrary *library;
        ClawtSkill *skill;
        JsonNode *refusal = NULL;

        library = library_or_error(self, request, &refusal);

        if (library == NULL)
            return refusal;

        name = payload_skill_name(payload, "name", &error);

        if (name == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        skill = clawt_skill_library_create(
            library, name,
            clawt_ipc_payload_string(payload, "description"),
            clawt_ipc_payload_string(payload, "body"), &error);

        if (skill == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "skill.changed", name);
        add_skill_object(builder, skill, TRUE);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.import") == 0) {
        const gchar *source = clawt_ipc_payload_string(payload, "source");
        ClawtSkillLibrary *library;
        ClawtSkill *skill;
        JsonNode *refusal = NULL;

        library = library_or_error(self, request, &refusal);

        if (library == NULL)
            return refusal;

        if (source == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a directory holding a SKILL.md is "
                                       "required");

        /*
         * A local path only, and no fetching.  Importing over the
         * network from an IPC handler would be the exact thing the
         * daemon must not do -- and it would also mean the bytes
         * somebody reviews are not necessarily the bytes that were
         * fetched.  Whoever wants a skill from the internet clones it
         * first, with a tool that shows them what arrived.
         */
        skill = clawt_skill_library_import(
            library, source,
            clawt_ipc_payload_string(payload, "origin"), &error);

        if (skill == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "skill.changed",
                             clawt_skill_get_name(skill));
        add_skill_object(builder, skill, TRUE);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.enable") == 0) {
        g_autofree gchar *name = NULL;
        ClawtSkillLibrary *library;
        ClawtSkill *skill;
        JsonNode *refusal = NULL;
        gboolean enabled = TRUE;

        library = library_or_error(self, request, &refusal);

        if (library == NULL)
            return refusal;

        name = payload_skill_name(payload, "name", &error);

        if (name == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (json_object_has_member(payload, "enabled"))
            enabled = json_object_get_boolean_member(payload, "enabled");

        if (!clawt_skill_library_set_enabled(library, name, enabled, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Enabling a skill changes what reaches every agent that has
         * it, so the workspaces are rewritten here rather than at the
         * next start.  A skill enabled and not linked is the same
         * silence as a skill assigned and not linked.
         */
        {
            g_autoptr(GPtrArray) refusals =
                clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "skill.changed", name);

            skill = clawt_skill_library_lookup(library, name);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "name", name);
            json_builder_set_member_name(builder, "enabled");
            json_builder_add_boolean_value(
                builder, skill != NULL && clawt_skill_get_enabled(skill));
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.remove") == 0) {
        g_autofree gchar *name = NULL;
        ClawtSkillLibrary *library;
        JsonNode *refusal = NULL;

        library = library_or_error(self, request, &refusal);

        if (library == NULL)
            return refusal;

        name = payload_skill_name(payload, "name", &error);

        if (name == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_skill_library_remove(library, name, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The links go with it.  A symlink into a directory that no
         * longer exists enumerates as a symlink and is skipped in
         * silence by every harness, so leaving them would leave every
         * agent looking healthy and loading nothing.
         */
        {
            g_autoptr(GPtrArray) refusals =
                clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "skill.changed", name);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "name", name);
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.assign") == 0 ||
        g_strcmp0(kind, "skill.unassign") == 0) {
        gboolean add = g_strcmp0(kind, "skill.assign") == 0;
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *team_id = clawt_ipc_payload_string(payload, "team");
        gboolean fleet = json_object_has_member(payload, "fleet") &&
                         json_object_get_boolean_member(payload, "fleet");
        g_autofree gchar *name = NULL;
        g_auto(GStrv) current = NULL;
        g_auto(GStrv) updated = NULL;
        gboolean changed;

        name = payload_skill_name(payload, "name", &error);

        if (name == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if ((agent_id != NULL) + (team_id != NULL) + (fleet ? 1 : 0) != 1)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "name exactly one of agent, team or fleet -- a skill is "
                "assigned in one place and resolved from all three");

        if (agent_id != NULL) {
            ClawtAgentConfig *config =
                clawt_config_get_agent(self->config, agent_id);

            if (config == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "no such agent");

            if (clawt_agent_config_has_key(config, "skills"))
                current = clawt_agent_config_get_string_list(config,
                                                             "skills");

            changed = edit_name_list(current, name, add, &updated);

            if (changed)
                clawt_agent_config_set_string_list(
                    config, "skills", (const gchar *const *)updated);
        } else if (team_id != NULL) {
            if (clawt_config_get_team(self->config, team_id) == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "no such team");

            current = clawt_config_get_team_string_list(self->config, team_id,
                                                        "skills");
            changed = edit_name_list(current, name, add, &updated);

            if (changed)
                clawt_config_set_team_string_list(
                    self->config, team_id, "skills",
                    (const gchar *const *)updated);
        } else {
            current = clawt_config_get_string_list(self->config,
                                                   "defaults.skills");
            changed = edit_name_list(current, name, add, &updated);

            if (changed)
                clawt_config_set_string_list(
                    self->config, "defaults.skills",
                    (const gchar *const *)updated);
        }

        if (changed && !clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        {
            g_autoptr(GPtrArray) refusals =
                clawt_daemon_render_refusals_new();

            if (changed)
                clawt_daemon_render_all_agents_into(self, refusals);

            clawt_event_bus_emit(self->bus, "skill.changed", name);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "name", name);
            json_builder_set_member_name(builder, "changed");
            json_builder_add_boolean_value(builder, changed);
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.reload") == 0) {
        guint count = 0;

        clawt_daemon_reload_skills(self);

        {
            g_autoptr(GPtrArray) refusals =
                clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "skill.changed", NULL);

            if (self->skills != NULL) {
                g_autoptr(GPtrArray) skills =
                    clawt_skill_library_list(self->skills);

                count = skills->len;
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "count");
            json_builder_add_int_value(builder, count);
            clawt_daemon_add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.commands") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *config;
        g_autoptr(GPtrArray) commands = NULL;
        guint i;

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent?");

        config = clawt_config_get_agent(self->config, agent_id);

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        commands = clawt_skill_commands_list(config);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "commands");
        json_builder_begin_array(builder);

        for (i = 0; i < commands->len; i++) {
            ClawtSkillCommand *command = g_ptr_array_index(commands, i);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "name", command->name);
            clawt_daemon_add_string_member(builder, "description",
                                           command->description);
            clawt_daemon_add_string_member(builder, "argument_hint",
                                           command->argument_hint);
            clawt_daemon_add_string_member(builder, "origin", command->origin);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "skill.expand") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        ClawtAgentConfig *config;
        g_autofree gchar *prompt = NULL;

        if (agent_id == NULL || name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and name are both required");

        config = clawt_config_get_agent(self->config, agent_id);

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        /*
         * The name is *not* put through the skill-name gate here.
         *
         * A command is not always a skill: a workspace can hold a
         * hand-written `.claude/commands/foo.md` whose name is anything
         * that harness accepts, and refusing it would make clawtilla's
         * composer offer fewer commands than the CLI itself has.  It is
         * safe because nothing here builds a path from it -- the lookup
         * is against a registry that already scanned the workspace, so
         * an unknown name is a miss rather than a traversal.
         */
        prompt = clawt_skill_commands_expand(
            config, name, clawt_ipc_payload_string(payload, "arguments"),
            &error);

        if (prompt == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "name", name);
        clawt_daemon_add_string_member(builder, "prompt", prompt);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
