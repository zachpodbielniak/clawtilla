/*
 * clawt-test-util.h - Shared helpers for the test suite
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Header-only, because the test rule compiles one .c file per binary and
 * a shared object file would mean teaching it about a second one.
 */

#pragma once

#include <glib.h>
#include <glib/gstdio.h>

/*
 * Removes a temporary directory and everything in it.
 *
 * g_rmdir() silently does nothing on a directory that is not empty, and
 * every fixture here creates files inside the one it made -- so each run
 * of the suite left a handful of /tmp/clawt-* directories behind, for
 * ever.  Deleting the tree is what the fixtures always meant.
 */
static inline void
clawt_test_remove_tree(const gchar *path)
{
    GDir *dir;
    const gchar *name;

    if (path == NULL)
        return;

    dir = g_dir_open(path, 0, NULL);

    if (dir != NULL) {
        while ((name = g_dir_read_name(dir)) != NULL) {
            gchar *child = g_build_filename(path, name, NULL);

            /*
             * Symlinks are unlinked, never followed: a fixture that
             * created one pointing outside its own directory would
             * otherwise have this delete whatever it pointed at.
             */
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                clawt_test_remove_tree(child);
            else
                g_unlink(child);

            g_free(child);
        }

        g_dir_close(dir);
    }

    g_rmdir(path);
}
