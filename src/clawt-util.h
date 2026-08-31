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
 * clawt_normalize_path_lexically:
 * @path: (nullable): a path, absolute or relative
 *
 * Collapses "." and ".." by text alone, touching no filesystem.
 *
 * The companion to clawt_canonicalize_missing() for a path that names
 * *another machine's* filesystem.  realpath() is the right answer when
 * the path is here and the wrong one when it is not: over ssh the whole
 * path is unresolvable locally, so clawt_canonicalize_missing() walks up
 * to "/" and hands back the string it was given -- ".." and all.  A
 * containment check then reads "/srv/work/../../etc/shadow" as being
 * inside "/srv/work", because it is a prefix with a separator after it,
 * while the *remote* kernel resolves the "..". That is an escape, and it
 * is why the remote sandbox does not share the local canonicaliser.
 *
 * ".." above the root of an absolute path is dropped rather than kept,
 * which is what the kernel does with "/..".  In a relative path a
 * leading ".." has nothing to cancel against and is kept, so the caller
 * can still see that it escapes.
 *
 * What this deliberately cannot do is resolve a symlink -- the file is
 * on the other machine.  A remote symlink pointing out of the allowed
 * tree is not visible to this check, and clawt_sandbox_describe() says
 * so rather than implying the boundary is complete.
 *
 * Returns: (transfer full) (nullable): the normalised path, or %NULL if
 *   @path was %NULL
 */
gchar *clawt_normalize_path_lexically(const gchar *path);

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
 * clawt_remove_tree:
 * @path: the directory to remove
 * @root: the directory it must be inside
 * @error: (out) (optional): return location for a #GError
 *
 * Removes @path and everything in it, refusing anything that is not
 * within @root.
 *
 * The guard is the point.  This is reached from "remove this agent and
 * everything it owns", where the paths come from configuration somebody
 * edits -- a workspace root left empty, or pointing at a home
 * directory, turns a tidy-up into a catastrophe, and there is no undo
 * to reach for afterwards.
 *
 * A @path that does not exist is success: the caller asked for it gone.
 *
 * Returns: %TRUE when nothing is left at @path
 */
gboolean clawt_remove_tree(const gchar  *path,
                           const gchar  *root,
                           GError      **error);

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

/**
 * clawt_timeout_add_seconds_full:
 * @interval: seconds between calls
 * @function: (scope notified): what to call
 * @data: data for @function
 * @notify: (nullable): called when the source is destroyed
 *
 * As clawt_timeout_add_seconds(), for a timer whose data has to be freed.
 *
 * A one-shot timer that returns %G_SOURCE_REMOVE cannot free its own
 * data on every path -- the source is also destroyed when whoever armed
 * it gives up first, and that path never reaches the callback.  Handing
 * the free to the source is the only spelling that covers both.
 *
 * Returns: (transfer full): the source; destroy it to cancel
 */
GSource *clawt_timeout_add_seconds_full(guint          interval,
                                        GSourceFunc    function,
                                        gpointer       data,
                                        GDestroyNotify notify);

/**
 * clawt_secure_equals:
 * @a: (nullable): one value
 * @b: (nullable): the other
 *
 * Compares two secrets in time that does not depend on their contents.
 *
 * `strcmp()` stops at the first byte that differs, so the time it takes
 * says how much of the secret the caller guessed right -- and a webhook
 * endpoint can be called as often as somebody likes, which is exactly
 * the setting where that leak is worth an attacker's while. GLib has no
 * such helper, so this is it.
 *
 * There is no early return, the length difference is folded into the
 * same accumulator as the bytes, and %NULL is never equal to anything
 * including %NULL: a missing secret compared against a missing header
 * must not authenticate a delivery.
 *
 * Returns: %TRUE if @a and @b are the same string
 */
gboolean clawt_secure_equals(const gchar *a, const gchar *b);

gchar *clawt_redact_secrets(const gchar *text);

/**
 * clawt_fts5_phrase:
 * @query: whatever a person or a model typed
 * @error: (out) (optional): return location for a #GError
 *
 * @query as an FTS5 phrase literal, or %NULL with a reason.
 *
 * An FTS5 query is *syntax*, not a search string.  A stray `"` ends the
 * literal, a bare `NOT` is an operator with nothing on its right, and an
 * unbalanced `(` is a parse error -- and every one of those comes back
 * as zero rows rather than as an error, which is indistinguishable from
 * a store that has never held the word.  Doubling the quotes and
 * wrapping the whole thing makes it one phrase, which is what was meant.
 *
 * A query with nothing tokenizable in it -- a lone quote, a row of
 * punctuation -- becomes an *empty* phrase, which matches nothing and
 * looks exactly the same again.  That is refused here with a message
 * saying so, because the one thing a search must never do is answer
 * "no matches" to a question it did not ask.
 *
 * One spelling, used by every FTS5 table in the tree: two would differ
 * exactly once, and the one that differed would be the silent half.
 *
 * Returns: (transfer full) (nullable): the quoted phrase, or %NULL
 */
gchar *clawt_fts5_phrase(const gchar  *query,
                         GError      **error);

/**
 * clawt_utf8_truncate:
 * @text: (nullable): the text to cut
 * @max_bytes: how many bytes the result may occupy
 * @from_end: %TRUE to keep the last @max_bytes, %FALSE to keep the first
 *
 * Cuts @text to @max_bytes without splitting a character.
 *
 * A byte budget applied to UTF-8 lands mid-sequence roughly half the
 * time, and what comes out is not shorter text -- it is text ending in a
 * replacement character, which reads as a corrupt transcript rather than
 * as a truncated one, and which json-glib and sqlite will both carry
 * onwards without complaint.  The cut is moved to the nearest character
 * boundary in the direction that keeps the result within budget.
 *
 * @from_end is what a summariser wants: what a piece of work concluded
 * is at the end of it, so a budget taken off the front records the plan
 * rather than the outcome.
 *
 * Text already inside the budget is copied unchanged, so a caller never
 * has to ask whether it needs to.
 *
 * Returns: (transfer full) (nullable): the cut text, or %NULL for %NULL
 */
gchar *clawt_utf8_truncate(const gchar *text,
                           gsize        max_bytes,
                           gboolean     from_end);

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

/**
 * clawt_agent_id_is_reserved:
 * @id: (nullable): a candidate agent id
 *
 * Whether @id is a sender name the routing rules key on -- "user",
 * "clawtilla", "routine" or "trigger".  A well-formed id can still be
 * one of these, and an agent that claimed one would inherit its
 * routing treatment wholesale: an agent called "clawtilla" would have
 * every message pass the loop guard unmeasured and close every
 * exchange it spoke into.  Checked beside clawt_is_valid_id() at the
 * two places an agent id enters the system.
 *
 * Returns: %TRUE if @id may not name an agent
 */
gboolean clawt_agent_id_is_reserved(const gchar *id);

/**
 * clawt_color_ink:
 * @hex: (nullable): a colour written as `#rgb` or `#rrggbb`
 *
 * Which of black or white is legible on @hex.
 *
 * `agents.color` is a hex string somebody types into a YAML file, and
 * both clients paint an agent's avatar with it -- so both have to decide
 * what colour the initials go in, and a light background with white
 * initials is unreadable.  One answer, from the sRGB relative luminance
 * the WCAG contrast formula uses, because two would differ for exactly
 * the colours near the boundary.
 *
 * A colour that is not one of the two accepted forms returns %NULL, and
 * the caller falls back to the avatar's own derived colour rather than
 * painting something.  It is also the validation: nothing else checks
 * this key, and it is spliced into a stylesheet.
 *
 * Returns: (nullable): `#000000`, `#ffffff`, or %NULL if @hex is not a
 *   colour this accepts
 */
const gchar *clawt_color_ink(const gchar *hex);

/**
 * clawt_time_ago_label:
 * @timestamp: when it happened, in microseconds since the epoch
 * @now: the current time, in the same units
 *
 * How long ago @timestamp was, as `just now`, `4m ago`, `2h ago` or
 * `3d ago`.
 *
 * For a list whose subject *is* recency -- alerts, mailbox items,
 * delegated tasks -- where a wall-clock time is a number the reader has
 * to subtract.  A chat transcript is the other case and uses
 * clawt_chat_time_label() instead: nothing re-renders a message that has
 * not changed, so a relative stamp there goes on saying `2m ago` for an
 * hour.
 *
 * @now is a parameter rather than a call to g_get_real_time() so this is
 * a pure function: both clients and the orchestration tools had their
 * own copy of this arithmetic, and a rule three surfaces apply is one
 * that has to be testable without any of them.
 *
 * A @timestamp in the future reads as `just now` rather than as a
 * negative age -- clocks move backwards, and a list is not the place to
 * report it.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_time_ago_label(gint64 timestamp, gint64 now);

/**
 * clawt_process_parent_of:
 * @pid: the process to ask about
 *
 * The parent of @pid, read from `/proc`.
 *
 * Returns: the parent pid, or 0 if @pid is gone or unreadable
 */
GPid clawt_process_parent_of(GPid pid);

/**
 * clawt_process_descendants:
 * @root: the process whose tree to walk
 *
 * Every process below @root, deepest first.
 *
 * @root itself is never in the result.  That is the whole point of the
 * function: interrupting an agent kills what its libreclaw spawned --
 * the AI CLI and whatever the AI CLI is running -- while libreclaw
 * itself keeps its link, its session and its mailbox.  Killing the root
 * as well would be stopping the agent, which is a different button.
 *
 * Deepest first so a kill walk signals a child before its parent: a
 * parent killed first reparents its children to init, and they are then
 * no longer findable through @root.
 *
 * Reads `/proc/<pid>/status`, not `/proc/<pid>/stat` -- the comm field
 * in the latter is bracketed and may itself contain `)` and spaces, so
 * every naive field split of it is wrong for a process whose name has
 * one.
 *
 * Returns: (transfer full) (element-type GPid): the descendants, deepest
 *   first, empty if there are none
 */
GArray *clawt_process_descendants(GPid root);

/**
 * clawt_process_is_descendant_of:
 * @pid: the process to check
 * @root: the ancestor to look for
 *
 * Whether @pid is still somewhere below @root right now.
 *
 * Asked again immediately before a signal is sent, because a pid read a
 * moment ago may have exited and been reused by then -- and the thing
 * that inherits a recycled pid is somebody else's process.  A stale
 * snapshot is the difference between stopping an agent's work and
 * killing an unrelated program.
 *
 * Returns: %TRUE if @pid descends from @root
 */
gboolean clawt_process_is_descendant_of(GPid pid, GPid root);

/**
 * clawt_command_shell_syntax_refusal:
 * @command: (nullable): a command line as somebody wrote it
 *
 * Whether @command is relying on a shell that will not be there.
 *
 * A command line handed to clawtilla_computer_exec goes through
 * g_shell_parse_argv() and is then spawned directly.  That is shell
 * lexing without shell semantics: quotes and word splitting are applied,
 * and `;`, `&&`, `|`, redirections, backquotes, `$(...)` and `$VAR` all
 * survive into argv as ordinary text.  Nothing errors -- the program
 * runs, exits 0 and prints the rest of the line back -- so an agent
 * reads it as a command that ran and behaved oddly rather than as a
 * command that was never a command.
 *
 * Read from the raw string, before the quotes are gone: after lexing,
 * `grep 'a|b' f` and `a | b` are the same argv, and quoting is the only
 * thing that says which one somebody meant.  Globs are deliberately not
 * flagged, because an unquoted `*.log` reaches the program unchanged and
 * that is sometimes the point.
 *
 * There is no shell because confinement inspects the translated argv.
 * The refusal therefore names `bash -c`, which is inspected the same way
 * rather than being a way around it.
 *
 * Returns: (transfer full) (nullable): a refusal naming the construct
 *   and the way to run it, or %NULL if the command is a plain argv
 */
gchar *clawt_command_shell_syntax_refusal(const gchar *command);

G_END_DECLS
