/*
 * clawt-avatar.c - Reading, sniffing and writing an agent's profile picture
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-avatar.h"
#include "agent/clawt-workspace.h"
#include "clawt-error.h"
#include "clawt-util.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/*
 * The extension order clawt_workspace_find_profile_picture() tries, and
 * the only names clawt_avatar_write() and clawt_avatar_clear() ever
 * touch.  One array rather than four separate string literals in each
 * function, so the order resolution follows and the set a write cleans
 * up cannot drift apart.
 */
static const gchar *const AVATAR_NAMES[] = {
    "profile-picture.png",
    "profile-picture.jpg",
    "profile-picture.jpeg",
    "profile-picture.webp",
    NULL
};

gchar *
clawt_avatar_resolve_path(const gchar *configured, const gchar *workspace)
{
    g_return_val_if_fail(workspace != NULL, NULL);

    if (configured != NULL && *configured != '\0') {
        g_autofree gchar *candidate = NULL;

        candidate = g_path_is_absolute(configured)
                    ? g_strdup(configured)
                    : g_build_filename(workspace, configured, NULL);

        if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR) &&
            g_access(candidate, R_OK) == 0)
            return g_steal_pointer(&candidate);

        /*
         * Named and missing are not the same thing as never named at
         * all.  Falling back in silence would make a typo in
         * agents.avatar indistinguishable from an agent that never had a
         * picture -- both would simply show initials -- and the person
         * who set it would have no way to learn their file is gone.
         */
        g_warning("agents.avatar: %s does not exist or cannot be read; "
                  "falling back to the auto-detected picture, if any",
                  candidate);
    }

    return clawt_workspace_find_profile_picture(workspace);
}

/*
 * Magic numbers, checked in the bytes themselves.
 *
 * Never the file's extension: a client's claim about its own upload is
 * exactly what an attacker controls, and a plain-text file saved as
 * "profile-picture.png" would otherwise be handed to a decoder as one.
 */
const gchar *
clawt_avatar_sniff_mime_type(const guchar *data, gsize length)
{
    static const guchar PNG_MAGIC[8] =
        { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

    if (data == NULL)
        return NULL;

    if (length >= 8 && memcmp(data, PNG_MAGIC, 8) == 0)
        return "image/png";

    if (length >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return "image/jpeg";

    /* RIFF <4-byte size> WEBP */
    if (length >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0)
        return "image/webp";

    return NULL;
}

const gchar *
clawt_avatar_extension_for_mime_type(const gchar *mime)
{
    if (g_strcmp0(mime, "image/png") == 0)
        return "png";

    /*
     * Stored as .jpg rather than .jpeg: AVATAR_NAMES tries .jpg first,
     * so a freshly written picture is found on the first comparison
     * clawt_workspace_find_profile_picture() makes rather than the
     * third.  .jpeg stays in the detected set for a file somebody placed
     * by hand.
     */
    if (g_strcmp0(mime, "image/jpeg") == 0)
        return "jpg";

    if (g_strcmp0(mime, "image/webp") == 0)
        return "webp";

    return NULL;
}

gchar *
clawt_avatar_compute_etag(const guchar *data, gsize length)
{
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, length);
}

gboolean
clawt_avatar_read(
    const gchar   *configured,
    const gchar   *workspace,
    gint64         max_bytes,
    guchar       **out_bytes,
    gsize         *out_length,
    gchar        **out_mime,
    gchar        **out_etag,
    GError       **error
){
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    const gchar *mime;
    GStatBuf info;
    gsize length = 0;

    g_return_val_if_fail(workspace != NULL, FALSE);
    g_return_val_if_fail(out_bytes != NULL, FALSE);
    g_return_val_if_fail(out_length != NULL, FALSE);
    g_return_val_if_fail(out_mime != NULL, FALSE);
    g_return_val_if_fail(out_etag != NULL, FALSE);

    path = clawt_avatar_resolve_path(configured, workspace);

    if (path == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "this agent has no profile picture");
        return FALSE;
    }

    /*
     * The stat's size, not a read followed by a length check: a picture
     * over the cap is refused for costing an allocation as well as for
     * costing a frame, and "never a truncated image" means the bytes
     * past the cap must never be read at all.
     */
    if (g_stat(path, &info) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "this agent has no profile picture");
        return FALSE;
    }

    if (max_bytes > 0 && (gint64)info.st_size > max_bytes) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s is %" G_GINT64_FORMAT " bytes, over "
                    "defaults.avatar_max_bytes (%" G_GINT64_FORMAT ")",
                    path, (gint64)info.st_size, max_bytes);
        return FALSE;
    }

    if (!g_file_get_contents(path, &contents, &length, error)) {
        return FALSE;
    }

    mime = clawt_avatar_sniff_mime_type((const guchar *)contents, length);

    if (mime == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s is not an image clawtilla recognises", path);
        return FALSE;
    }

    *out_bytes = (guchar *)g_steal_pointer(&contents);
    *out_length = length;
    *out_mime = g_strdup(mime);
    *out_etag = clawt_avatar_compute_etag(*out_bytes, length);

    return TRUE;
}

gboolean
clawt_avatar_write(
    const gchar   *workspace,
    const guchar  *data,
    gsize          length,
    gchar        **out_mime,
    GError       **error
){
    const gchar *mime;
    const gchar *ext;
    g_autofree gchar *filename = NULL;
    g_autofree gchar *target = NULL;
    guint i;

    g_return_val_if_fail(workspace != NULL, FALSE);
    g_return_val_if_fail(data != NULL || length == 0, FALSE);

    mime = clawt_avatar_sniff_mime_type(data, length);

    if (mime == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that is not an image clawtilla recognises "
                    "(png, jpeg and webp only)");
        return FALSE;
    }

    ext = clawt_avatar_extension_for_mime_type(mime);
    g_assert(ext != NULL);

    /*
     * Every auto-detected name removed first, not just the one about to
     * be replaced.  A picture set as .png and then replaced with a .jpg
     * would otherwise leave the old .png behind -- and it sorts first in
     * AVATAR_NAMES, so the *old* picture would keep winning resolution
     * after the "new" one had already been written.
     */
    for (i = 0; AVATAR_NAMES[i] != NULL; i++) {
        g_autofree gchar *existing = g_build_filename(workspace,
                                                       AVATAR_NAMES[i], NULL);

        g_unlink(existing);
    }

    filename = g_strconcat("profile-picture.", ext, NULL);
    target = g_build_filename(workspace, filename, NULL);

    if (!clawt_write_file_atomic(target, (const gchar *)data, (gssize)length,
                                 0644, FALSE, error))
        return FALSE;

    if (out_mime != NULL)
        *out_mime = g_strdup(mime);

    return TRUE;
}

gboolean
clawt_avatar_clear(const gchar *workspace)
{
    gboolean removed = FALSE;
    guint i;

    g_return_val_if_fail(workspace != NULL, FALSE);

    for (i = 0; AVATAR_NAMES[i] != NULL; i++) {
        g_autofree gchar *path = g_build_filename(workspace, AVATAR_NAMES[i],
                                                   NULL);

        if (g_unlink(path) == 0)
            removed = TRUE;
    }

    return removed;
}
