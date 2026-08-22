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
 * clawt_copy_tree:
 * @source: the directory to copy
 * @target: where to put it
 * @keep_git: whether to copy `.git` directories too
 * @copied: (out) (optional): how many files were written
 * @error: (out) (optional): return location for a #GError
 *
 * Copies a directory tree, recursively.
 *
 * `.git` is skipped unless @keep_git, because the usual reason to copy
 * a directory into somebody else's tree is to take the contents and not
 * the history -- and a nested repository inside a workspace is a thing
 * that surprises people much later.
 *
 * Symbolic links are copied as links rather than followed: a link
 * pointing outside the source would otherwise pull in whatever it
 * pointed at.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_copy_tree(const gchar  *source,
                         const gchar  *target,
                         gboolean      keep_git,
                         guint        *copied,
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
/**
 * clawt_canonicalize_missing:
 * @path: a path that may not exist yet
 *
 * Resolves @path as far as it exists.
 *
 * realpath() fails outright on a path whose last component does not exist
 * yet, which is most write targets.  Resolving the deepest existing
 * parent and re-appending the rest gives a file about to be created the
 * same protection: the symlinks and ".." in its parents are collapsed.
 *
 * Returns: (transfer full): the resolved path
 */
gchar *clawt_canonicalize_missing(const gchar *path);

/**
 * clawt_path_is_within:
 * @path: a canonical path
 * @root: a canonical directory
 *
 * Whether @path is @root or sits underneath it.
 *
 * A plain prefix test is not enough -- "/home/zach/srcevil" starts with
 * "/home/zach/src" and is somewhere else entirely -- so the next
 * character must be a separator.  Every containment check in clawtilla
 * goes through this one function: two implementations of this test is how
 * one of them ends up subtly more permissive than the other.
 *
 * Returns: %TRUE if @path is contained by @root
 */
gboolean clawt_path_is_within(const gchar *path, const gchar *root);

/**
 * clawt_generate_token:
 * @error: (out) (optional): return location for a #GError
 *
 * Generates a shared secret: 32 bytes from the kernel's random pool,
 * hex-encoded.
 *
 * Deliberately not clawt_generate_id().  Ids are seeded from GLib's
 * Mersenne Twister and appear in message ids, task ids and log lines, so
 * anyone who can read a transcript can recover the generator state and
 * predict the next value.  That is fine for an identifier and useless for
 * a secret.
 *
 * Returns: (transfer full) (nullable): the token, or %NULL if the random
 *   pool could not be read
 */
gchar *clawt_generate_token(GError **error);

/**
 * CLAWT_MAX_SOCKET_PATH:
 *
 * The longest unix socket path the kernel will accept.
 *
 * sockaddr_un.sun_path is 108 bytes on Linux including the terminator.
 * Exceeding it does not fail where you would expect: the bind appears to
 * succeed and the socket file is simply not where you asked for it, which
 * surfaces much later as a confusing "no such file or directory" from
 * something else entirely.
 */
#define CLAWT_MAX_SOCKET_PATH 107

/**
 * clawt_check_socket_path:
 * @path: the socket path to bind
 * @error: (out) (optional): return location for a #GError
 *
 * Checks a unix socket path is short enough to bind.
 *
 * Returns: %TRUE if the path can be used
 */
gboolean clawt_check_socket_path(const gchar *path, GError **error);

/**
 * clawt_build_child_environment:
 * @extra: (element-type utf8 utf8) (nullable): variables to add
 *
 * Builds the environment a spawned child gets: a fixed allowlist taken
 * from this process, plus whatever @extra names.
 *
 * Never the daemon's environment wholesale.  A stray ANTHROPIC_API_KEY
 * reaching a subscription CLI quietly moves it onto pay-as-you-go billing
 * nobody agreed to, and a stray SSH_AUTH_SOCK hands an agent every key in
 * the user's agent.  Both spawn paths -- the supervised agent process and
 * a command run on a host computer -- use this, because an allowlist that
 * covers only one of them protects nothing.
 *
 * Returns: (transfer full) (array zero-terminated=1): the environment
 */
GStrv clawt_build_child_environment(GHashTable *extra);

/**
 * clawt_timeout_add_seconds:
 * @interval: seconds between calls
 * @function: (scope notified): what to call
 * @data: data for @function
 *
 * Adds a repeating timer to the *thread-default* main context.
 *
 * g_timeout_add_seconds() always uses the global default context, which
 * for a daemon embedded in another program -- one that runs its own loop
 * on its own context -- means the timer never fires at all.  That has
 * silently disabled a keepalive and a restart policy here already, so
 * every periodic timer in clawtilla goes through this.
 *
 * Returns: (transfer full): the source; destroy it to cancel
 */
GSource *clawt_timeout_add_seconds(guint       interval,
                                   GSourceFunc function,
                                   gpointer    data);

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
