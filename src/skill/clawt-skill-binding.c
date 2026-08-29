/*
 * clawt-skill-binding.c - Which skills an agent gets, and why
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "skill/clawt-skill-binding.h"

#include <string.h>

struct _ClawtSkillBinding {
    gint        ref_count;

    gchar      *name;
    gchar      *origin;

    /*
     * Unowned: the library outlives every binding taken from it, and a
     * binding that held a copy would report an enabled flag from
     * whenever it was made rather than from now.
     */
    ClawtSkill *skill;
};

static ClawtSkillBinding *
binding_new(const gchar *name, const gchar *origin, ClawtSkill *skill)
{
    ClawtSkillBinding *self = g_new0(ClawtSkillBinding, 1);

    self->ref_count = 1;
    self->name = g_strdup(name);
    self->origin = g_strdup(origin);
    self->skill = skill;

    return self;
}

ClawtSkillBinding *
clawt_skill_binding_ref(ClawtSkillBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

void
clawt_skill_binding_unref(ClawtSkillBinding *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->name);
    g_free(self->origin);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtSkillBinding, clawt_skill_binding,
                    clawt_skill_binding_ref, clawt_skill_binding_unref)

const gchar *
clawt_skill_binding_get_name(ClawtSkillBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->name;
}

ClawtSkill *
clawt_skill_binding_get_skill(ClawtSkillBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->skill;
}

const gchar *
clawt_skill_binding_get_origin(ClawtSkillBinding *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->origin;
}

gboolean
clawt_skill_binding_is_active(ClawtSkillBinding *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->skill != NULL && clawt_skill_get_enabled(self->skill);
}

/* ── Resolving ───────────────────────────────────────────────────── */

/*
 * Add one list's worth, first writer winning.
 *
 * That is what makes agent beat team beat fleet without a comparison
 * per collision: the caller walks the three in that order and this
 * refuses to overwrite.  The precedence lives in one place -- the call
 * order below -- rather than being restated at each merge.
 */
static void
add_names(GPtrArray          *out,
          GHashTable         *seen,
          const gchar *const *names,
          const gchar        *origin,
          ClawtSkillLibrary  *library)
{
    gsize i;

    for (i = 0; names != NULL && names[i] != NULL; i++) {
        g_autofree gchar *trimmed = g_strdup(names[i]);

        g_strstrip(trimmed);

        if (*trimmed == '\0')
            continue;

        if (g_hash_table_contains(seen, trimmed))
            continue;

        g_hash_table_add(seen, g_strdup(trimmed));
        g_ptr_array_add(out, binding_new(
            trimmed, origin,
            library != NULL
                ? clawt_skill_library_lookup(library, trimmed)
                : NULL));
    }
}

static gint
compare_bindings(gconstpointer a, gconstpointer b)
{
    ClawtSkillBinding *const *x = a;
    ClawtSkillBinding *const *y = b;

    return g_strcmp0((*x)->name, (*y)->name);
}

GPtrArray *
clawt_skill_resolve_for_agent(ClawtConfig       *config,
                              ClawtAgentConfig  *agent,
                              ClawtSkillLibrary *library)
{
    GPtrArray *out;
    g_autoptr(GHashTable) seen = NULL;
    g_auto(GStrv) own = NULL;
    g_auto(GStrv) team_list = NULL;
    g_auto(GStrv) fleet = NULL;
    const gchar *team;

    g_return_val_if_fail(agent != NULL, NULL);

    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_skill_binding_unref);
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /*
     * The agent's own list first.
     *
     * Read straight from the agent's node rather than through
     * clawt_agent_config_get_string_list(), which falls back to
     * `defaults.skills` for this key -- correct for a caller asking
     * "what does this agent end up with", and wrong here, because it
     * would attribute the fleet's list to the agent and hide the fleet
     * pass entirely.
     */
    if (clawt_agent_config_has_key(agent, "skills"))
        own = clawt_agent_config_get_string_list(agent, "skills");

    add_names(out, seen, (const gchar *const *)own, "agent", library);

    team = clawt_agent_config_get_string(agent, "team");

    if (config != NULL && team != NULL && *team != '\0') {
        team_list = clawt_config_get_team_string_list(config, team, "skills");
        add_names(out, seen, (const gchar *const *)team_list, "team",
                  library);
    }

    if (config != NULL) {
        fleet = clawt_config_get_string_list(config, "defaults.skills");
        add_names(out, seen, (const gchar *const *)fleet, "fleet", library);
    }

    g_ptr_array_sort(out, compare_bindings);

    return out;
}

GPtrArray *
clawt_skill_bindings_warnings(GPtrArray *bindings, const gchar *agent_id)
{
    GPtrArray *out;
    guint i;

    out = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; bindings != NULL && i < bindings->len; i++) {
        ClawtSkillBinding *binding = g_ptr_array_index(bindings, i);

        if (binding->skill == NULL) {
            g_ptr_array_add(out, g_strdup_printf(
                "%s is assigned the skill '%s' (from the %s), and no such "
                "skill is in the library -- it reaches nobody",
                agent_id != NULL ? agent_id : "an agent",
                binding->name, binding->origin));
            continue;
        }

        if (!clawt_skill_get_enabled(binding->skill)) {
            /*
             * Said as well, and said differently.  "Assigned but
             * disabled" is a deliberate state -- it is what an import
             * lands in -- so this is a reminder rather than a
             * complaint, and phrasing it like the missing case would
             * send somebody looking for a file that is right there.
             */
            g_ptr_array_add(out, g_strdup_printf(
                "%s is assigned the skill '%s', which is not enabled yet, so "
                "nothing in it reaches the agent",
                agent_id != NULL ? agent_id : "an agent", binding->name));
        }
    }

    return out;
}
