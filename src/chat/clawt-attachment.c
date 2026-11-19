/*
 * clawt-attachment.c - Files an agent sends to its operator
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "chat/clawt-attachment.h"
#include "clawt-error.h"
#include "clawt-util.h"

#include <gio/gio.h>

/*
 * An id is <random>-<safe filename>.
 *
 * The name is carried in the id so a client can label the file without a
 * second field to keep in step, and it is sanitised on the way in rather
 * than escaped at each use -- the same rule agent ids follow.  Every
 * character outside the safe set becomes "_", which cannot produce a
 * separator. The random prefix prevents the resulting id from being a
 * dot component, even when the original basename starts with dots.
 */
static gchar *
safe_name(const gchar *path)
{
    g_autofree gchar *base = g_path_get_basename(path);
    GString *out = g_string_new(NULL);
    const gchar *p;

    for (p = base; *p != '\0'; p++) {
        if (g_ascii_isalnum(*p) || *p == '.' || *p == '_')
            g_string_append_c(out, *p);
        else
            g_string_append_c(out, '_');
    }

    /*
     * A name that is only dots would make "." or ".." after the id's
     * prefix is stripped, and an empty one would leave a trailing "-".
     */
    if (out->len == 0 || g_strcmp0(out->str, ".") == 0 ||
        g_strcmp0(out->str, "..") == 0) {
        g_string_assign(out, "file");
    }

    /* Bounded, because it becomes a filename. */
    if (out->len > 96)
        g_string_truncate(out, 96);

    return g_string_free(out, FALSE);
}

gchar *
clawt_attachment_store(const gchar *dir, const gchar *path, GError **error)
{
    g_autofree gchar *id = NULL;
    g_autofree gchar *random = NULL;
    g_autofree gchar *name = NULL;
    g_autofree gchar *target = NULL;
    g_autoptr(GFile) source_file = NULL;
    g_autoptr(GFile) target_file = NULL;

    g_return_val_if_fail(dir != NULL, NULL);
    g_return_val_if_fail(path != NULL, NULL);

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no file at %s", path);
        return NULL;
    }

    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create %s", dir);
        return NULL;
    }

    random = clawt_generate_id(NULL);
    name = safe_name(path);
    id = g_strdup_printf("%s-%s", random, name);
    target = g_build_filename(dir, id, NULL);

    source_file = g_file_new_for_path(path);
    target_file = g_file_new_for_path(target);

    if (!g_file_copy(source_file, target_file, G_FILE_COPY_OVERWRITE, NULL,
                     NULL, NULL, error))
        return NULL;

    return g_steal_pointer(&id);
}

/* The character set an id is made of, checked rather than trusted. */
static gboolean
id_is_ours(const gchar *id)
{
    const gchar *p;

    if (id == NULL || *id == '\0' || *id == '.')
        return FALSE;

    for (p = id; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    }

	/*
	 * Adjacent dots inside a basename are ordinary filename bytes, not a
	 * parent-directory component. Leading dots and both separators were
	 * rejected above. Accepting these also repairs ids already stored by
	 * older versions, whose writer has always preserved dots in names.
	 */
	return TRUE;
}

gchar *
clawt_attachment_path(const gchar *dir, const gchar *id)
{
    g_return_val_if_fail(dir != NULL, NULL);

    if (!id_is_ours(id))
        return NULL;

    return g_build_filename(dir, id, NULL);
}

gchar *
clawt_attachment_name(const gchar *id)
{
    const gchar *dash;

    if (!id_is_ours(id))
        return NULL;

    dash = strchr(id, '-');

    if (dash == NULL || dash[1] == '\0')
        return g_strdup(id);

    return g_strdup(dash + 1);
}

/* ── The block an operator's message carries ─────────────────────── */

GString *
clawt_attachment_block_new(const gchar *body)
{
    GString *out = g_string_new(body);

    if (out->len > 0)
        g_string_append(out, "\n\n");

    /*
     * The marker, then the instruction.  The marker is what clients
     * match on; the sentence after it is for the agent, which reads
     * files with its own tools and needs telling which path those
     * tools can open.
     */
    g_string_append(out, CLAWT_ATTACHMENT_MARKER
                         ". Open them with your own read tool at the host "
                         "path below; it runs on the host even when your "
                         "shell does not.\n");

    return out;
}

void
clawt_attachment_block_add(GString     *block,
                           const gchar *name,
                           const gchar *host_path,
                           const gchar *path,
                           const gchar *id)
{
    g_return_if_fail(block != NULL);
    g_return_if_fail(name != NULL);

    g_string_append_printf(block, "- %s\n  %s\n", name,
                           host_path != NULL ? host_path : "");

    /*
     * Both paths, host first: given only the container one, an agent
     * could stat the file with computer_exec and never open it with its
     * own read tool -- which is what happened to the first image anybody
     * sent.
     */
    if (path != NULL && *path != '\0' && g_strcmp0(host_path, path) != 0)
        g_string_append_printf(block, "  (inside your container: %s)\n",
                               path);

    /*
     * The daemon's own copy, by id: what every client previews from,
     * since the host path opens only on the daemon's machine.
     */
    if (id != NULL && *id != '\0')
        g_string_append_printf(block, "  clawt:%s\n", id);
}

void
clawt_attachment_block_add_unsaved(GString *block, const gchar *name)
{
    g_return_if_fail(block != NULL);
    g_return_if_fail(name != NULL);

    g_string_append_printf(block, "- %s (could not be saved)\n", name);
}
