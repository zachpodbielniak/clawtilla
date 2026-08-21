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
