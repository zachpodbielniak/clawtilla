/*
 * clawt-skill-bundled.c - Skills clawtilla ships with
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A bundled skill is written into the fleet's skills directory like one
 * somebody wrote, because that is the layout every agent CLI already
 * reads.  The difference is the marker file beside it, which is how a
 * newer clawtilla knows a copy is its own to replace.
 */

#include "clawtilla.h"
#include "skill/clawt-skill-bundled.h"
#include "integration/clawt-integration.h"

#include <string.h>

#include "clawt-bundled-skills.h"

/* "web-research/SKILL.md" -> "web-research". */
static gchar *
skill_of(const gchar *path)
{
    const gchar *slash = strchr(path, '/');

    return slash != NULL ? g_strndup(path, (gsize)(slash - path)) : g_strdup(path);
}

/* A digest of everything one bundled skill consists of, paths included. */
static gchar *
bundled_digest(const gchar *skill)
{
    g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(embedded_files); i++) {
        g_autofree gchar *owner = skill_of(embedded_files[i].path);

        if (g_strcmp0(owner, skill) != 0)
            continue;
        g_checksum_update(sum, (const guchar *)embedded_files[i].path, -1);
        g_checksum_update(sum, (const guchar *)embedded_files[i].content, -1);
    }

    return g_strdup(g_checksum_get_string(sum));
}

/*
 * Writes one skill.  The marker goes last: a crash part-way leaves a
 * directory without it, which the next start treats as somebody's own and
 * leaves alone -- the safe side, since a half-written copy cannot be
 * mistaken for a finished one of ours.
 */
static gboolean
install_one(const gchar *skills_dir, const gchar *skill, const gchar *digest, GError **error)
{
    g_autofree gchar *directory = g_build_filename(skills_dir, skill, NULL);
    g_autofree gchar *marker = g_build_filename(directory, CLAWT_SKILL_BUNDLED_MARKER, NULL);
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(embedded_files); i++) {
        g_autofree gchar *owner = skill_of(embedded_files[i].path);
        g_autofree gchar *target = NULL;
        g_autofree gchar *parent = NULL;

        if (g_strcmp0(owner, skill) != 0)
            continue;

        target = g_build_filename(skills_dir, embedded_files[i].path, NULL);
        parent = g_path_get_dirname(target);
        if (!clawt_ensure_dir(parent, 0700, error))
            return FALSE;
        if (!clawt_write_file_atomic(target, embedded_files[i].content, -1, 0600, FALSE, error))
            return FALSE;
    }

    return clawt_write_file_atomic(marker, digest, -1, 0600, FALSE, error);
}

gboolean
clawt_skill_install_bundled(const gchar *skills_dir, GError **error)
{
    g_autoptr(GHashTable) done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gsize i;

    g_return_val_if_fail(skills_dir != NULL, FALSE);

    for (i = 0; i < G_N_ELEMENTS(embedded_files); i++) {
        g_autofree gchar *skill = skill_of(embedded_files[i].path);
        g_autofree gchar *directory = NULL;
        g_autofree gchar *marker = NULL;
        g_autofree gchar *digest = NULL;
        g_autofree gchar *written = NULL;

        if (g_hash_table_contains(done, skill))
            continue;
        g_hash_table_add(done, g_strdup(skill));

        directory = g_build_filename(skills_dir, skill, NULL);
        marker = g_build_filename(directory, CLAWT_SKILL_BUNDLED_MARKER, NULL);
        digest = bundled_digest(skill);

        if (g_file_test(directory, G_FILE_TEST_EXISTS)) {
            /* Somebody's own skill by this name wins. */
            if (!g_file_get_contents(marker, &written, NULL, NULL))
                continue;
            if (g_strcmp0(g_strstrip(written), digest) == 0)
                continue;
        }

        if (!install_one(skills_dir, skill, digest, error)) {
            g_prefix_error(error, "bundled skill '%s': ", skill);
            return FALSE;
        }
    }

    return TRUE;
}

const gchar *
clawt_skill_bundled_file(const gchar *path)
{
    gsize i;

    for (i = 0; path != NULL && i < G_N_ELEMENTS(embedded_files); i++)
        if (g_strcmp0(embedded_files[i].path, path) == 0)
            return embedded_files[i].content;

    return NULL;
}

GStrv
clawt_skill_bundled_for_agent(ClawtConfig *config, ClawtAgentConfig *agent)
{
    g_autoptr(GPtrArray) bindings = NULL;
    g_autoptr(GStrvBuilder) names = g_strv_builder_new();
    guint i;

    if (config == NULL || agent == NULL)
        return NULL;

    /*
     * The same resolver that decides whether the web tools are offered,
     * so an agent that has the tools has the skill and one that has not,
     * has neither.
     */
    bindings = clawt_integration_resolve_for_agent(config, agent);
    for (i = 0; bindings != NULL && i < bindings->len; i++) {
        ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

        if (g_strcmp0(clawt_integration_binding_get_info(binding)->id, "libresearch") == 0) {
            g_strv_builder_add(names, CLAWT_SKILL_BUNDLED_WEB_RESEARCH);
            break;
        }
    }

    return g_strv_builder_end(names);
}
