/*
 * clawt-util.c - Small shared helpers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "clawt-util.h"

#include <glib/gstdio.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

gchar *
clawt_expand_path(const gchar *path)
{
    static const struct {
        const gchar *name;
        const gchar *(*getter)(void);
    } vars[] = {
        { "$XDG_RUNTIME_DIR", NULL },
        { "$XDG_DATA_HOME",   NULL },
        { "$XDG_CONFIG_HOME", NULL },
        { "$XDG_CACHE_HOME",  NULL },
        { "$HOME",            NULL }
    };
    g_autofree gchar *work = NULL;
    gsize i;

    if (path == NULL)
        return NULL;

    work = g_strdup(path);

    for (i = 0; i < G_N_ELEMENTS(vars); i++) {
        const gchar *replacement = NULL;
        g_autofree gchar *fallback = NULL;
        gchar *expanded;

        if (strstr(work, vars[i].name) == NULL)
            continue;

        if (g_strcmp0(vars[i].name, "$XDG_RUNTIME_DIR") == 0) {
            replacement = g_get_user_runtime_dir();
        } else if (g_strcmp0(vars[i].name, "$XDG_DATA_HOME") == 0) {
            replacement = g_get_user_data_dir();
        } else if (g_strcmp0(vars[i].name, "$XDG_CONFIG_HOME") == 0) {
            replacement = g_get_user_config_dir();
        } else if (g_strcmp0(vars[i].name, "$XDG_CACHE_HOME") == 0) {
            replacement = g_get_user_cache_dir();
        } else {
            replacement = g_get_home_dir();
        }

        if (replacement == NULL) {
            fallback = g_build_filename(g_get_home_dir(), ".local", NULL);
            replacement = fallback;
        }

        expanded = g_strdup(work);
        {
            g_auto(GStrv) parts = g_strsplit(expanded, vars[i].name, -1);
            g_free(expanded);
            expanded = g_strjoinv(replacement, parts);
        }

        g_free(work);
        work = expanded;
    }

    /*
     * "~" is only a home directory at the start of a path.  A file genuinely
     * called "back~up" in some directory is not, and rewriting it would be a
     * silent corruption rather than a helpful expansion.
     */
    if (work[0] == '~' && (work[1] == '/' || work[1] == '\0')) {
        gchar *expanded = g_build_filename(g_get_home_dir(),
                                           work[1] != '\0' ? work + 2 : "",
                                           NULL);
        g_free(work);
        work = expanded;
    }

    return g_steal_pointer(&work);
}

gboolean
clawt_ensure_dir(const gchar *path, gint mode, GError **error)
{
    g_autofree gchar *expanded = NULL;

    g_return_val_if_fail(path != NULL, FALSE);

    expanded = clawt_expand_path(path);

    if (g_mkdir_with_parents(expanded, mode) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create directory %s: %s",
                    expanded, g_strerror(errno));
        return FALSE;
    }

    /*
     * g_mkdir_with_parents() applies mode through the umask, so asking for
     * 0700 on a machine with a permissive umask quietly yields 0755.  For a
     * directory holding credentials that is the entire problem, so the mode
     * is set again explicitly rather than requested and hoped for.
     */
    if (g_chmod(expanded, mode) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not set mode %o on %s: %s",
                    (guint)mode, expanded, g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_write_file_atomic(const gchar  *path,
                        const gchar  *contents,
                        gssize        length,
                        gint          mode,
                        gboolean      keep_backup,
                        GError      **error)
{
    g_autofree gchar *expanded = NULL;
    g_autofree gchar *tmp_path = NULL;
    g_autofree gchar *backup_path = NULL;
    gsize to_write;

    g_return_val_if_fail(path != NULL, FALSE);
    g_return_val_if_fail(contents != NULL, FALSE);

    expanded = clawt_expand_path(path);
    to_write = (length < 0) ? strlen(contents) : (gsize)length;

    tmp_path = g_strdup_printf("%s.tmp-%d", expanded, (int)getpid());

    if (!g_file_set_contents(tmp_path, contents, (gssize)to_write, error)) {
        g_prefix_error(error, "writing %s: ", tmp_path);
        return FALSE;
    }

    if (g_chmod(tmp_path, mode) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not set mode %o on %s: %s",
                    (guint)mode, tmp_path, g_strerror(errno));
        g_unlink(tmp_path);
        return FALSE;
    }

    /*
     * The backup is taken before the rename, so an interrupted write leaves
     * both the original (still in place) and its copy.  Keeping only one
     * generation is deliberate: this is a safety net for "the daemon
     * rewrote my config and I want the old one", not a version history.
     */
    if (keep_backup && g_file_test(expanded, G_FILE_TEST_EXISTS)) {
        backup_path = g_strdup_printf("%s.bak", expanded);

        if (g_rename(expanded, backup_path) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not back up %s: %s",
                        expanded, g_strerror(errno));
            g_unlink(tmp_path);
            return FALSE;
        }
    }

    if (g_rename(tmp_path, expanded) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not replace %s: %s", expanded, g_strerror(errno));
        g_unlink(tmp_path);

        /* Put the original back rather than leaving nothing at all. */
        if (backup_path != NULL)
            g_rename(backup_path, expanded);

        return FALSE;
    }

    return TRUE;
}

gchar *
clawt_generate_id(const gchar *prefix)
{
    static const gchar alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    gint64 now_ms = g_get_real_time() / 1000;
    gchar timestamp[11];
    gchar random_part[11];
    gint i;

    /* 50 bits of timestamp, most significant first, so ids sort by time. */
    for (i = 9; i >= 0; i--) {
        timestamp[i] = alphabet[now_ms & 0x1f];
        now_ms >>= 5;
    }
    timestamp[10] = '\0';

    for (i = 0; i < 10; i++)
        random_part[i] = alphabet[g_random_int_range(0, 32)];
    random_part[10] = '\0';

    if (prefix != NULL)
        return g_strdup_printf("%s-%s%s", prefix, timestamp, random_part);

    return g_strdup_printf("%s%s", timestamp, random_part);
}

/*
 * Resolves a path as far as it exists.
 *
 * realpath() fails outright on a path whose last component does not exist
 * yet, which is most write targets.  Resolving the deepest existing parent
 * and re-appending the rest gives the same protection for a file about to
 * be created: the symlinks and ".." in the parents are still collapsed.
 */
gchar *
clawt_canonicalize_missing(const gchar *path)
{
    g_autofree gchar *expanded = NULL;

    /*
     * NULL in, NULL out.  A sandbox with no root is legitimate --
     * confine: none has nothing to pin -- and callers pass the root
     * straight through.
     */
    if (path == NULL)
        return NULL;

    expanded = clawt_expand_path(path);
    g_autofree gchar *remainder = NULL;
    g_autofree gchar *current = g_strdup(expanded);

    while (TRUE) {
        gchar *real = realpath(current, NULL);
        g_autofree gchar *parent = NULL;
        g_autofree gchar *base = NULL;

        if (real != NULL) {
            gchar *joined = (remainder != NULL)
                            ? g_build_filename(real, remainder, NULL)
                            : g_strdup(real);
            free(real);
            return joined;
        }

        parent = g_path_get_dirname(current);
        base = g_path_get_basename(current);

        /* Reached the top without resolving anything. */
        if (g_strcmp0(parent, current) == 0)
            return g_steal_pointer(&expanded);

        {
            gchar *longer = (remainder != NULL)
                            ? g_build_filename(base, remainder, NULL)
                            : g_strdup(base);

            g_free(remainder);
            remainder = longer;
        }

        g_free(current);
        current = g_strdup(parent);
    }
}

gboolean
clawt_path_is_within(const gchar *path, const gchar *root)
{
    gsize root_length;

    if (path == NULL || root == NULL)
        return FALSE;

    root_length = strlen(root);

    if (!g_str_has_prefix(path, root))
        return FALSE;

    /*
     * A prefix match is not enough: "/home/zach/srcevil" starts with
     * "/home/zach/src" and is a different directory entirely.  The next
     * character has to be a separator, or the strings have to be equal.
     */
    return path[root_length] == '\0' || path[root_length] == '/' ||
           (root_length > 0 && root[root_length - 1] == '/');
}


/*
 * Variables a child always needs, whatever else it is given.
 */
static const gchar *const passthrough_env[] = {
    "PATH", "HOME", "USER", "LOGNAME", "SHELL", "LANG", "TERM", "TZ",
    "XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
    NULL
};

GSource *
clawt_timeout_add_seconds(guint interval, GSourceFunc function,
                          gpointer data)
{
    GSource *source = g_timeout_source_new_seconds(interval);
    GMainContext *context = g_main_context_get_thread_default();

    g_source_set_callback(source, function, data, NULL);
    g_source_attach(source, context);

    return source;
}

GStrv
clawt_build_child_environment(GHashTable *extra)
{
    g_autoptr(GPtrArray) out = g_ptr_array_new();
    gsize i;

    for (i = 0; passthrough_env[i] != NULL; i++) {
        const gchar *value = g_getenv(passthrough_env[i]);

        if (value != NULL)
            g_ptr_array_add(out, g_strdup_printf("%s=%s",
                                                 passthrough_env[i], value));
    }

    if (extra != NULL) {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, extra);

        while (g_hash_table_iter_next(&iter, &key, &value))
            g_ptr_array_add(out, g_strdup_printf("%s=%s", (const gchar *)key,
                                                 (const gchar *)value));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

gboolean
clawt_check_socket_path(const gchar *path, GError **error)
{
    gsize length;

    g_return_val_if_fail(path != NULL, FALSE);

    length = strlen(path);

    if (length <= CLAWT_MAX_SOCKET_PATH)
        return TRUE;

    /*
     * Refused here, with the number, rather than letting the bind go
     * ahead.  An over-long path does not fail at bind time -- the socket
     * simply is not created where it was asked for, and the first sign is
     * an unrelated ENOENT further down.
     */
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                "the socket path is %u bytes; the kernel accepts at most "
                "%d. Put it somewhere shorter, such as under "
                "$XDG_RUNTIME_DIR: %s",
                (guint)length, CLAWT_MAX_SOCKET_PATH, path);

    return FALSE;
}

gchar *
clawt_generate_token(GError **error)
{
    guchar bytes[32];
    gchar *out;
    gsize i;
    FILE *pool;
    gsize got;

    /*
     * Read straight from the kernel pool rather than going through any
     * userspace generator: this value is what stops one local process
     * claiming another agent's identity.
     */
    pool = fopen("/dev/urandom", "rb");
    if (pool == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open /dev/urandom: %s", g_strerror(errno));
        return NULL;
    }

    got = fread(bytes, 1, sizeof(bytes), pool);
    fclose(pool);

    if (got != sizeof(bytes)) {
        /*
         * A short read is refused rather than padded.  Quietly using
         * fewer random bytes than intended is the exact failure mode that
         * makes a token look strong while being weak.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "short read from /dev/urandom: wanted %u bytes, got %u",
                    (guint)sizeof(bytes), (guint)got);
        return NULL;
    }

    out = g_malloc(sizeof(bytes) * 2 + 1);

    for (i = 0; i < sizeof(bytes); i++)
        g_snprintf(out + (i * 2), 3, "%02x", bytes[i]);

    return out;
}

gchar *
clawt_redact_secrets(const gchar *text)
{
    /*
     * Two shapes are matched.  Assignments name the thing being assigned,
     * which is how a key usually appears in a log line or a command; the
     * standalone patterns catch tokens that were pasted on their own, where
     * there is no key to go by and the shape is all there is.
     */
    static const gchar *assignment_keys[] = {
        "api[_-]?key", "secret", "token", "password", "passwd",
        "auth", "credential", "bearer", "access[_-]?key", "private[_-]?key",
        NULL
    };
    static const gchar *standalone[] = {
        "sk-ant-[A-Za-z0-9_-]{16,}",
        "sk-[A-Za-z0-9]{20,}",
        "gh[pousr]_[A-Za-z0-9]{20,}",
        "xoxb-[A-Za-z0-9-]{10,}",
        "syt_[A-Za-z0-9_]{10,}",
        "-----BEGIN[A-Z ]*PRIVATE KEY-----",
        NULL
    };
    gchar *work;
    gsize i;

    if (text == NULL)
        return NULL;

    work = g_strdup(text);

    for (i = 0; assignment_keys[i] != NULL; i++) {
        g_autofree gchar *pattern = NULL;
        g_autoptr(GRegex) regex = NULL;
        gchar *replaced;

        pattern = g_strdup_printf(
            "((?i:%s)[\"']?\\s*[:=]\\s*[\"']?)([^\\s\"',;)}]{4,})",
            assignment_keys[i]);

        regex = g_regex_new(pattern, 0, 0, NULL);
        if (regex == NULL)
            continue;

        replaced = g_regex_replace(regex, work, -1, 0, "\\1[REDACTED]",
                                   0, NULL);
        if (replaced != NULL) {
            g_free(work);
            work = replaced;
        }
    }

    for (i = 0; standalone[i] != NULL; i++) {
        g_autoptr(GRegex) regex = g_regex_new(standalone[i], 0, 0, NULL);
        gchar *replaced;

        if (regex == NULL)
            continue;

        replaced = g_regex_replace_literal(regex, work, -1, 0, "[REDACTED]",
                                           0, NULL);
        if (replaced != NULL) {
            g_free(work);
            work = replaced;
        }
    }

    return work;
}

gboolean
clawt_is_valid_id(const gchar *id)
{
    const gchar *p;
    gsize length;

    if (id == NULL || *id == '\0')
        return FALSE;

    length = strlen(id);
    if (length > 64)
        return FALSE;

    /*
     * Ids become directory names, socket names and SQLite filenames.
     * Restricting the character set here means none of those call sites has
     * to escape anything, and "../etc" is rejected once rather than
     * defended against repeatedly.
     */
    for (p = id; *p != '\0'; p++) {
        if (g_ascii_islower(*p) || g_ascii_isdigit(*p) ||
            *p == '-' || *p == '_')
            continue;

        return FALSE;
    }

    /* Leading punctuation makes for confusing paths and unreadable output. */
    if (id[0] == '-' || id[0] == '_')
        return FALSE;

    return TRUE;
}

gboolean
clawt_copy_tree(const gchar *source, const gchar *target, gboolean keep_git,
                guint *copied, GError **error)
{
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    g_return_val_if_fail(source != NULL, FALSE);
    g_return_val_if_fail(target != NULL, FALSE);

    if (!clawt_ensure_dir(target, 0700, error))
        return FALSE;

    dir = g_dir_open(source, 0, error);

    if (dir == NULL)
        return FALSE;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *from = g_build_filename(source, name, NULL);
        g_autofree gchar *to = g_build_filename(target, name, NULL);
        g_autoptr(GFile) from_file = NULL;
        g_autoptr(GFile) to_file = NULL;

        /*
         * The usual reason to copy a directory into somebody else\'s
         * tree is to take the contents and not the history, and a
         * nested repository inside a workspace surprises people much
         * later.
         */
        if (!keep_git && g_strcmp0(name, ".git") == 0)
            continue;

        if (g_file_test(from, G_FILE_TEST_IS_DIR) &&
            !g_file_test(from, G_FILE_TEST_IS_SYMLINK)) {
            if (!clawt_copy_tree(from, to, keep_git, copied, error))
                return FALSE;

            continue;
        }

        from_file = g_file_new_for_path(from);
        to_file = g_file_new_for_path(to);

        /*
         * NOFOLLOW_SYMLINKS: a link pointing outside the source would
         * otherwise pull in whatever it pointed at, which for a
         * workspace holding a link to ~/.ssh is not a copy anybody
         * asked for.
         */
        if (!g_file_copy(from_file, to_file,
                         G_FILE_COPY_OVERWRITE |
                         G_FILE_COPY_NOFOLLOW_SYMLINKS |
                         G_FILE_COPY_ALL_METADATA,
                         NULL, NULL, NULL, error))
            return FALSE;

        if (copied != NULL)
            (*copied)++;
    }

    return TRUE;
}
