/*
 * clawt-util.h - Small shared helpers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_expand_path:
 * @path: (nullable): a path that may begin with "~" or contain $XDG_* variables
 *
 * Expands the path spellings the configuration uses.
 *
 * Config files are written by people, who write "~/.clawtilla" and
 * "$XDG_RUNTIME_DIR/clawtilla/daemon.sock" because that is what those
 * places are called.  Passing either to open() verbatim creates a
 * directory named "~" in the working directory, which is a confusing way
 * to discover the expansion was missing.
 *
 * Only $XDG_RUNTIME_DIR, $XDG_DATA_HOME, $XDG_CONFIG_HOME, $XDG_CACHE_HOME
 * and $HOME are expanded, each with the fallback the XDG spec gives it.
 * General environment interpolation is deliberately absent: a config value
 * should not change meaning based on what happened to be exported.
 *
 * Returns: (transfer full) (nullable): the expanded path, or %NULL if
 *   @path was %NULL
 */
gchar *clawt_expand_path(const gchar *path);

/**
 * clawt_ensure_dir:
 * @path: directory to create
 * @mode: permissions for any directory created
 * @error: (out) (optional): return location for a #GError
 *
 * Creates @path and any missing parents, then verifies the mode.
 *
 * The verification is the point: g_mkdir_with_parents() applies @mode
 * subject to the umask, so asking for 0700 on a machine with a permissive
 * umask silently gives 0755 -- which for a directory holding credentials is
 * the whole problem.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_ensure_dir(const gchar  *path,
                          gint          mode,
                          GError      **error);

/**
 * clawt_write_file_atomic:
 * @path: file to write
 * @contents: data to write
 * @length: length of @contents, or -1 if NUL-terminated
 * @mode: permissions for the file
 * @keep_backup: whether to keep the previous contents as "@path.bak"
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a file by creating a temporary alongside it and renaming, so a
 * crash or a full disk leaves the original intact rather than truncated.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_write_file_atomic(const gchar  *path,
                                 const gchar  *contents,
                                 gssize        length,
                                 gint          mode,
                                 gboolean      keep_backup,
                                 GError      **error);

/**
 * clawt_generate_id:
 * @prefix: (nullable): a short prefix, or %NULL
 *
 * Generates a sortable unique identifier: a millisecond timestamp in base32
 * followed by randomness, so ids sort by creation time.
 *
 * Sortability is what makes a mailbox query cheap -- ordering by id is
 * ordering by arrival, with no separate index.
 *
 * Returns: (transfer full): a new identifier
 */
gchar *clawt_generate_id(const gchar *prefix);

/**
 * clawt_redact_secrets:
 * @text: text that may contain credentials
 *
 * Replaces anything that looks like a credential with a placeholder.
 *
 * Applied when writing the event log and transcripts, not when displaying
 * them.  A transcript is replayed into every context rebuild, so a leaked
 * key in one is permanent; redacting at display time would leave the
 * original on disk forever.
 *
 * Returns: (transfer full): the redacted text
 */
gchar *clawt_redact_secrets(const gchar *text);

/**
 * clawt_is_valid_id:
 * @id: (nullable): a candidate identifier
 *
 * Whether @id is usable as an agent or room id: non-empty, no more than 64
 * characters, and only lowercase letters, digits, "-" and "_".
 *
 * Ids become directory names, socket names and SQLite filenames, so the
 * character set is restricted at the door rather than escaped at each use.
 *
 * Returns: %TRUE if @id is acceptable
 */
gboolean clawt_is_valid_id(const gchar *id);

G_END_DECLS
