/*
 * clawt-exchange.c - The shared drop-box agents pass files through
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-exchange.h"

#include <glib/gstdio.h>

#include <stdlib.h>
#include <string.h>

struct _ClawtExchange {
    GObject parent_instance;

    gchar  *root;
    gint64  max_bytes;
};

G_DEFINE_FINAL_TYPE(ClawtExchange, clawt_exchange, G_TYPE_OBJECT)

ClawtExchange *
clawt_exchange_new(const gchar *root, gint64 max_bytes)
{
    ClawtExchange *self = g_object_new(CLAWT_TYPE_EXCHANGE, NULL);

    g_return_val_if_fail(root != NULL, NULL);

    self->root = clawt_expand_path(root);
    self->max_bytes = max_bytes;

    return self;
}

const gchar *
clawt_exchange_get_root(ClawtExchange *self)
{
    g_return_val_if_fail(CLAWT_IS_EXCHANGE(self), NULL);

    return self->root;
}

gboolean
clawt_exchange_prepare(ClawtExchange *self, const gchar *agent_id,
                       GError **error)
{
    g_autofree gchar *shared = NULL;
    g_autofree gchar *own = NULL;

    g_return_val_if_fail(CLAWT_IS_EXCHANGE(self), FALSE);

    if (!clawt_ensure_dir(self->root, 0700, error))
        return FALSE;

    shared = g_build_filename(self->root, "shared", NULL);
    if (!clawt_ensure_dir(shared, 0700, error))
        return FALSE;

    if (agent_id == NULL)
        return TRUE;

    own = g_build_filename(self->root, agent_id, NULL);

    return clawt_ensure_dir(own, 0700, error);
}

GPtrArray *
clawt_exchange_get_mounts(ClawtExchange *self, const gchar *agent_id)
{
    GPtrArray *mounts;
    ClawtMount *mount;

    g_return_val_if_fail(CLAWT_IS_EXCHANGE(self), NULL);

    mounts = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);

    /*
     * The whole exchange, read-only.  Reading across the exchange is
     * deliberate -- that is how an agent picks up what another left for
     * it -- and the read-write grants below are layered on top.
     *
     * Shared relabelling (:z), not private (:Z).  Several containers see
     * this directory at once, and a private label would let whichever one
     * started last make it unreadable to the others -- which is exactly
     * the failure the exchange exists to avoid.
     */
    mount = clawt_mount_new(self->root, CLAWT_EXCHANGE_MOUNT_POINT);
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RO);
    clawt_mount_set_create(mount, TRUE);
    clawt_mount_set_relabel(mount, CLAWT_RELABEL_SHARED);
    g_ptr_array_add(mounts, mount);

    {
        g_autofree gchar *shared_source = g_build_filename(self->root,
                                                           "shared", NULL);
        g_autofree gchar *shared_target =
            g_build_filename(CLAWT_EXCHANGE_MOUNT_POINT, "shared", NULL);

        mount = clawt_mount_new(shared_source, shared_target);
        clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
        clawt_mount_set_create(mount, TRUE);
        clawt_mount_set_relabel(mount, CLAWT_RELABEL_SHARED);
        g_ptr_array_add(mounts, mount);
    }

    if (agent_id != NULL) {
        g_autofree gchar *own_source = g_build_filename(self->root, agent_id,
                                                        NULL);
        g_autofree gchar *own_target =
            g_build_filename(CLAWT_EXCHANGE_MOUNT_POINT, agent_id, NULL);

        mount = clawt_mount_new(own_source, own_target);
        clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
        clawt_mount_set_create(mount, TRUE);
        clawt_mount_set_relabel(mount, CLAWT_RELABEL_SHARED);
        g_ptr_array_add(mounts, mount);
    }

    return mounts;
}

gchar *
clawt_exchange_resolve(ClawtExchange  *self,
                       const gchar    *agent_id,
                       const gchar    *relative,
                       gboolean        for_writing,
                       GError        **error)
{
    g_autofree gchar *joined = NULL;
    g_autofree gchar *canonical = NULL;
    g_autofree gchar *root_canonical = NULL;
    g_autofree gchar *shared_prefix = NULL;
    g_autofree gchar *own_prefix = NULL;

    g_return_val_if_fail(CLAWT_IS_EXCHANGE(self), NULL);
    g_return_val_if_fail(relative != NULL, NULL);

    if (g_path_is_absolute(relative)) {
        /*
         * An agent may hand back the in-computer path it was given, which
         * is the mount point rather than the host directory.  Accepting it
         * saves a class of confusing refusal; anything else absolute is
         * still rejected below by the containment check.
         */
        if (g_str_has_prefix(relative, CLAWT_EXCHANGE_MOUNT_POINT))
            relative += strlen(CLAWT_EXCHANGE_MOUNT_POINT);
    }

    while (*relative == G_DIR_SEPARATOR)
        relative++;

    joined = g_build_filename(self->root, relative, NULL);

    /*
     * Canonicalised before the check, so `..` and a symlink whose target
     * is outside are caught by the same test.  Two separate tests for the
     * two cases is how one of them ends up not quite matching the other.
     */
    canonical = clawt_canonicalize_missing(joined);
    root_canonical = clawt_canonicalize_missing(self->root);

    if (canonical == NULL || root_canonical == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' could not be resolved", relative);
        return NULL;
    }

    if (!clawt_path_is_within(canonical, root_canonical)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside the exchange", relative);
        return NULL;
    }

    if (!for_writing)
        return g_steal_pointer(&canonical);

    /*
     * Reading is open across the exchange; writing is not.  An agent that
     * could overwrite another's drop-box could quietly replace the file
     * that agent was about to act on.
     */
    shared_prefix = g_build_filename(root_canonical, "shared", NULL);

    if (clawt_path_is_within(canonical, shared_prefix))
        return g_steal_pointer(&canonical);

    if (agent_id != NULL) {
        own_prefix = g_build_filename(root_canonical, agent_id, NULL);

        if (clawt_path_is_within(canonical, own_prefix))
            return g_steal_pointer(&canonical);
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                "you may write to shared/ and to %s/, not to '%s'",
                agent_id != NULL ? agent_id : "your own directory", relative);

    return NULL;
}

GPtrArray *
clawt_exchange_list(ClawtExchange *self, const gchar *relative)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GDir) dir = NULL;
    GPtrArray *out;
    const gchar *name;

    g_return_val_if_fail(CLAWT_IS_EXCHANGE(self), NULL);

    path = (relative != NULL && *relative != '\0')
           ? clawt_exchange_resolve(self, NULL, relative, FALSE, NULL)
           : g_strdup(self->root);

    if (path == NULL)
        return NULL;

    dir = g_dir_open(path, 0, NULL);
    if (dir == NULL)
        return NULL;

    out = g_ptr_array_new_with_free_func(g_free);

    while ((name = g_dir_read_name(dir)) != NULL)
        g_ptr_array_add(out, g_strdup(name));

    g_ptr_array_sort_values(out, (GCompareFunc)g_strcmp0);

    return out;
}

typedef struct {
    gchar  *path;
    gint64  mtime;
    gint64  size;
} FileEntry;

static void
file_entry_free(gpointer data)
{
    FileEntry *entry = data;

    g_free(entry->path);
    g_free(entry);
}

static void
collect(const gchar *dir_path, GPtrArray *out)
{
    g_autoptr(GDir) dir = g_dir_open(dir_path, 0, NULL);
    const gchar *name;

    if (dir == NULL)
        return;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *child = g_build_filename(dir_path, name, NULL);
        GStatBuf info;

        if (g_lstat(child, &info) != 0)
            continue;

        /*
         * Symlinks are counted but never followed.  A link into the wider
         * filesystem would make the sweep delete files outside the
         * exchange entirely.
         */
        if (S_ISLNK(info.st_mode))
            continue;

        if (S_ISDIR(info.st_mode)) {
            collect(child, out);
        } else {
            FileEntry *entry = g_new0(FileEntry, 1);

            entry->path = g_steal_pointer(&child);
            entry->mtime = (gint64)info.st_mtime;
            entry->size = (gint64)info.st_size;
            g_ptr_array_add(out, entry);
        }
    }
}

gint64
clawt_exchange_get_size(ClawtExchange *self)
{
    g_autoptr(GPtrArray) files = NULL;
    gint64 total = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_EXCHANGE(self), 0);

    files = g_ptr_array_new_with_free_func(file_entry_free);
    collect(self->root, files);

    for (i = 0; i < files->len; i++)
        total += ((FileEntry *)g_ptr_array_index(files, i))->size;

    return total;
}

static gint
by_mtime(gconstpointer a, gconstpointer b)
{
    const FileEntry *left = *(FileEntry * const *)a;
    const FileEntry *right = *(FileEntry * const *)b;

    if (left->mtime < right->mtime)
        return -1;
    if (left->mtime > right->mtime)
        return 1;

    return 0;
}

guint
clawt_exchange_sweep(ClawtExchange *self, gint max_age_days)
{
    g_autoptr(GPtrArray) files = NULL;
    gint64 now;
    gint64 total = 0;
    guint removed = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_EXCHANGE(self), 0);

    files = g_ptr_array_new_with_free_func(file_entry_free);
    collect(self->root, files);

    now = g_get_real_time() / G_USEC_PER_SEC;

    for (i = 0; i < files->len; i++)
        total += ((FileEntry *)g_ptr_array_index(files, i))->size;

    g_ptr_array_sort(files, by_mtime);

    for (i = 0; i < files->len; i++) {
        FileEntry *entry = g_ptr_array_index(files, i);
        gboolean too_old = (max_age_days > 0) &&
                           (now - entry->mtime >
                            (gint64)max_age_days * 24 * 60 * 60);
        gboolean over_cap = (self->max_bytes > 0) && (total > self->max_bytes);

        if (!too_old && !over_cap)
            continue;

        if (g_unlink(entry->path) != 0)
            continue;

        total -= entry->size;
        removed++;
    }

    return removed;
}

static void
clawt_exchange_finalize(GObject *object)
{
    ClawtExchange *self = CLAWT_EXCHANGE(object);

    g_free(self->root);

    G_OBJECT_CLASS(clawt_exchange_parent_class)->finalize(object);
}

static void
clawt_exchange_class_init(ClawtExchangeClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_exchange_finalize;
}

static void
clawt_exchange_init(ClawtExchange *self)
{
    (void)self;
}
