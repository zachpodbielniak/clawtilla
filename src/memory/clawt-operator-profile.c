/*
 * clawt-operator-profile.c - What the fleet knows about the person it works for
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "memory/clawt-operator-profile.h"

#include <string.h>

struct _ClawtOperatorProfile {
    GObject parent_instance;

    gchar             *path;
    ClawtMemoryScopes *scopes;   /* unowned: the daemon outlives this */
};

G_DEFINE_FINAL_TYPE(ClawtOperatorProfile, clawt_operator_profile,
                    G_TYPE_OBJECT)

#define DEFAULT_LIMIT 25

ClawtOperatorProfile *
clawt_operator_profile_new(const gchar *state_dir, ClawtMemoryScopes *scopes)
{
    ClawtOperatorProfile *self;

    g_return_val_if_fail(state_dir != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_OPERATOR_PROFILE, NULL);
    self->path = g_build_filename(state_dir, "OPERATOR.org", NULL);

    if (scopes != NULL)
        self->scopes = g_object_ref(scopes);

    return self;
}

const gchar *
clawt_operator_profile_path(ClawtOperatorProfile *self)
{
    g_return_val_if_fail(CLAWT_IS_OPERATOR_PROFILE(self), NULL);

    return self->path;
}

gchar *
clawt_operator_profile_read_text(ClawtOperatorProfile *self)
{
    gchar *text = NULL;

    g_return_val_if_fail(CLAWT_IS_OPERATOR_PROFILE(self), NULL);

    if (!g_file_get_contents(self->path, &text, NULL, NULL))
        return NULL;

    return text;
}

gboolean
clawt_operator_profile_write_text(ClawtOperatorProfile *self,
                                  const gchar *text, GError **error)
{
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(CLAWT_IS_OPERATOR_PROFILE(self), FALSE);
    g_return_val_if_fail(text != NULL, FALSE);

    dir = g_path_get_dirname(self->path);

    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    return clawt_write_file_atomic(self->path, text, -1, 0600, FALSE, error);
}

GPtrArray *
clawt_operator_profile_learned(ClawtOperatorProfile *self, guint limit)
{
    ClawtMemoryStore *fleet;

    g_return_val_if_fail(CLAWT_IS_OPERATOR_PROFILE(self), NULL);

    if (limit == 0)
        limit = DEFAULT_LIMIT;

    /*
     * Opened for *read*, so a fleet that has never recorded anything
     * about its operator does not acquire a fleet.db by being asked.
     */
    fleet = (self->scopes != NULL)
        ? clawt_memory_scopes_open_for_read(self->scopes,
                                            CLAWT_MEMORY_SCOPE_FLEET, NULL)
        : NULL;

    if (fleet == NULL)
        return g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_memory_free);

    return clawt_memory_store_list(fleet, CLAWT_OPERATOR_CATEGORY, FALSE,
                                   limit, NULL);
}

gchar *
clawt_operator_profile_render(ClawtOperatorProfile *self, guint limit)
{
    g_autofree gchar *written = NULL;
    g_autoptr(GPtrArray) learned = NULL;
    g_autoptr(GString) out = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_OPERATOR_PROFILE(self), NULL);

    written = clawt_operator_profile_read_text(self);
    learned = clawt_operator_profile_learned(self, limit);

    if ((written == NULL || *written == '\0') && learned->len == 0)
        return NULL;

    out = g_string_new(NULL);

    if (written != NULL && *written != '\0') {
        g_string_append(out, g_strstrip(written));
        g_string_append_c(out, '\n');
    }

    if (learned->len > 0) {
        /*
         * The learned half is labelled and dated.
         *
         * Somebody reading their own profile has to be able to tell what
         * they wrote from what a fleet concluded about them, and to see
         * when it concluded it -- an inference from six months ago and
         * one from this morning are not the same claim.
         */
        if (out->len > 0)
            g_string_append_c(out, '\n');

        g_string_append(out,
                        "** What the fleet has recorded\n"
                        "\n"
                        "Written by agents as they worked. Correct or "
                        "delete any of it: these are fleet-scope memories "
                        "in the '" CLAWT_OPERATOR_CATEGORY "' category.\n"
                        "\n");

        for (i = 0; i < learned->len; i++) {
            ClawtMemory *memory = g_ptr_array_index(learned, i);
            g_autoptr(GDateTime) when = NULL;
            g_autofree gchar *day = NULL;

            when = g_date_time_new_from_unix_local(memory->created_at);
            day = (when != NULL) ? g_date_time_format(when, "%Y-%m-%d")
                                 : NULL;

            g_string_append_printf(out, "- %s", memory->content);

            if (day != NULL)
                g_string_append_printf(out, " /(%s, %s)/", day,
                                       memory->source != NULL
                                       ? memory->source : "unattributed");

            g_string_append_c(out, '\n');
        }
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static void
clawt_operator_profile_dispose(GObject *object)
{
    ClawtOperatorProfile *self = CLAWT_OPERATOR_PROFILE(object);

    g_clear_object(&self->scopes);
    g_clear_pointer(&self->path, g_free);

    G_OBJECT_CLASS(clawt_operator_profile_parent_class)->dispose(object);
}

static void
clawt_operator_profile_class_init(ClawtOperatorProfileClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_operator_profile_dispose;
}

static void
clawt_operator_profile_init(ClawtOperatorProfile *self)
{
    (void)self;
}
