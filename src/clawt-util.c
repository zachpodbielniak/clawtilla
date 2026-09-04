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
#include <errno.h>

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

gchar *
clawt_normalize_path_lexically(const gchar *path)
{
    g_auto(GStrv) parts = NULL;
    g_autoptr(GPtrArray) kept = NULL;
    g_autoptr(GString) out = NULL;
    gboolean absolute;
    gsize i;

    if (path == NULL)
        return NULL;

    absolute = (path[0] == '/');
    parts = g_strsplit(path, "/", -1);
    kept = g_ptr_array_new();

    for (i = 0; parts[i] != NULL; i++) {
        /* "" comes from a leading, trailing or doubled separator. */
        if (parts[i][0] == '\0' || g_strcmp0(parts[i], ".") == 0)
            continue;

        if (g_strcmp0(parts[i], "..") != 0) {
            g_ptr_array_add(kept, parts[i]);
            continue;
        }

        /*
         * Pop, unless there is nothing to pop against.  For an absolute
         * path that means dropping it outright, which is what the kernel
         * does with "/.." -- the root is its own parent.  For a relative
         * one it is kept, because a caller comparing it against a root
         * has to be able to see that it escapes.
         */
        if (kept->len > 0 &&
            g_strcmp0(g_ptr_array_index(kept, kept->len - 1), "..") != 0) {
            g_ptr_array_remove_index(kept, kept->len - 1);
        } else if (!absolute) {
            g_ptr_array_add(kept, parts[i]);
        }
    }

    out = g_string_new(NULL);

    for (i = 0; i < kept->len; i++) {
        if (out->len > 0 || absolute)
            g_string_append_c(out, '/');

        g_string_append(out, (const gchar *)g_ptr_array_index(kept, i));
    }

    /*
     * An absolute path that cancelled down to nothing is the root, and a
     * relative one that did is the current directory.  Returning "" for
     * either would make every containment test against it succeed.
     */
    if (out->len == 0)
        g_string_append(out, absolute ? "/" : ".");

    return g_string_free(g_steal_pointer(&out), FALSE);
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
    return clawt_timeout_add_seconds_full(interval, function, data, NULL);
}

GSource *
clawt_timeout_add_seconds_full(guint          interval,
                               GSourceFunc    function,
                               gpointer       data,
                               GDestroyNotify notify)
{
    GSource *source = g_timeout_source_new_seconds(interval);
    /*
     * Read here, in the function that attaches the source, rather than
     * taken as an argument.  Naming the context at the call site has
     * been got wrong five times in this tree; asking for it where the
     * attach happens cannot be got wrong at all.
     */
    GMainContext *context = g_main_context_get_thread_default();

    g_source_set_callback(source, function, data, notify);
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

gboolean
clawt_secure_equals(const gchar *a, const gchar *b)
{
    gsize len_a;
    gsize len_b;
    gsize i;
    /*
     * volatile so the accumulation survives -O2.  A compiler that can
     * prove the result is only read at the end is entitled to rewrite
     * the loop into the early return this function exists to avoid.
     */
    volatile guchar diff = 0;

    /*
     * A missing value is never equal, not even to another missing one.
     * The alternative -- NULL == NULL -- would mean a trigger with no
     * secret authenticated a delivery that sent no signature, which is
     * the one case that must stay a refusal.
     */
    if (a == NULL || b == NULL)
        return FALSE;

    len_a = strlen(a);
    len_b = strlen(b);

    /*
     * Folded in rather than returned on.  Comparing the lengths first
     * would answer "how long is the secret" in constant time, which is
     * the first thing worth knowing about it.
     */
    diff = (guchar)((len_a ^ len_b) | ((len_a ^ len_b) >> 8) |
                    ((len_a ^ len_b) >> 16));

    /*
     * Every byte of the longer string is read, and the shorter one is
     * indexed modulo its own length so the read stays in bounds without
     * a branch that depends on which ran out.
     */
    for (i = 0; i < MAX(len_a, len_b); i++) {
        guchar ca = (len_a > 0) ? (guchar)a[i % len_a] : 0;
        guchar cb = (len_b > 0) ? (guchar)b[i % len_b] : 0;

        diff |= (guchar)(ca ^ cb);
    }

    return diff == 0;
}

gchar *
clawt_fts5_phrase(const gchar *query, GError **error)
{
    g_auto(GStrv) parts = NULL;
    g_autofree gchar *escaped = NULL;
    const gchar *p;
    gboolean has_token = FALSE;

    if (query == NULL || *query == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "there is nothing to search for");
        return NULL;
    }

    /*
     * Whether the query has anything an FTS5 tokenizer would keep.
     *
     * The unicode61 tokenizer keeps letters and digits and drops
     * everything else, so a query of pure punctuation quotes into a
     * phrase with no terms in it -- which matches nothing, silently,
     * and reads as an empty store.  Checked against g_unichar_isalnum()
     * rather than ASCII, because a search in any other script is a
     * search.
     */
    for (p = query; *p != '\0'; p = g_utf8_next_char(p)) {
        if (g_unichar_isalnum(g_utf8_get_char(p))) {
            has_token = TRUE;
            break;
        }
    }

    if (!has_token) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' has no letters or digits in it, so there is "
                    "nothing to match", query);
        return NULL;
    }

    parts = g_strsplit(query, "\"", -1);
    escaped = g_strjoinv("\"\"", parts);

    return g_strdup_printf("\"%s\"", escaped);
}

gchar *
clawt_utf8_truncate(const gchar *text, gsize max_bytes, gboolean from_end)
{
    gsize length;
    const gchar *start;
    const gchar *end;

    if (text == NULL)
        return NULL;

    length = strlen(text);

    if (length <= max_bytes || max_bytes == 0)
        return (max_bytes == 0) ? g_strdup("") : g_strdup(text);

    if (from_end) {
        start = text + (length - max_bytes);

        /*
         * Forwards, never backwards.  Moving the start back to the
         * previous boundary would put the whole character back and take
         * the result *over* budget, which is exactly what a budget is
         * for.  g_utf8_find_next_char() from a continuation byte lands
         * on the next lead byte, which is the first whole character
         * inside the allowance.
         */
        if ((*start & 0xC0) == 0x80)
            start = g_utf8_find_next_char(start, text + length);

        return g_strdup(start != NULL ? start : "");
    }

    end = text + max_bytes;

    /*
     * And backwards at the other end, for the same reason: the cut has
     * to land on a boundary at or before the budget, so a character
     * straddling it is dropped rather than halved.
     */
    while (end > text && (*end & 0xC0) == 0x80)
        end--;

    return g_strndup(text, (gsize)(end - text));
}

gchar *
clawt_redact_secrets(const gchar *text)
{
    /*
     * Three shapes are matched.  Assignments name the thing being assigned,
     * which is how a key usually appears in a log line or a command; the
     * standalone patterns catch tokens that were pasted on their own, where
     * there is no key to go by and the shape is all there is; and the
     * prefixed patterns catch the ones an HTTP header introduces, where
     * the name and the value have nothing between them an assignment
     * would recognise.
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
        "github_pat_[A-Za-z0-9_]{20,}",
        "gl(?:pat|rt|ptt|soat|feat|cbt|dr|ft|imt|oat|rt)-[A-Za-z0-9_-]{16,}",
        "xox[abeoprs]-[A-Za-z0-9-]{10,}",
        "xapp-[A-Za-z0-9-]{10,}",
        "AIza[A-Za-z0-9_-]{30,}",
        "AKIA[A-Z0-9]{16}",
        "syt_[A-Za-z0-9_]{10,}",
        "-----BEGIN[A-Z ]*PRIVATE KEY-----",
        NULL
    };
    /*
     * A credential does not always arrive in a "key = value" shape, and
     * the assignment patterns cannot see the one it arrives in most
     * often.  An HTTP header names the scheme and then the token with no
     * separator between them: after "auth" comes "orization" rather than
     * a colon, and after "bearer" comes a space.  So `Authorization:
     * Bearer <token>` -- which is how every connector, the venture
     * bridge and ntfy carry theirs, and what a libreclaw child writes
     * when it logs a request -- went through untouched.
     *
     * These carry their own prefix group and go through the same
     * "\1[REDACTED]" replacement, so the line stays readable: the header
     * name and the scheme are what makes the log line worth keeping.
     */
    static const gchar *prefixed[] = {
        "((?i:(?:proxy-)?authorization)\\s*:\\s*"
        "(?i:bearer|basic|token|digest)?\\s*)([^\\s\"',;)}]{8,})",
        "((?i:bearer)\\s+)([^\\s\"',;)}]{8,})",
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

    for (i = 0; prefixed[i] != NULL; i++) {
        g_autoptr(GRegex) regex = g_regex_new(prefixed[i], 0, 0, NULL);
        gchar *replaced;

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
clawt_agent_id_is_reserved(const gchar *id)
{
    /*
     * The sender names the routing rules key on.  "user" is how
     * is_operator_room() recognises the operator's own conversations;
     * "clawtilla" is how the system signs a settle notice, which the
     * drain then delivers with the exchange closed and the loop guard
     * passes unmeasured; "routine" and "trigger" are what automated
     * work sends as.  An agent claiming any of them would inherit that
     * treatment wholesale -- an agent called "clawtilla" whose every
     * message bypassed the loop guard is the sharpest of the four.
     * Walked as a list here and nowhere else, so the next reserved
     * sender is added in one place.
     */
    static const gchar *reserved[] = {
        "user", "clawtilla", "routine", "trigger", NULL
    };
    guint i;

    if (id == NULL)
        return FALSE;

    for (i = 0; reserved[i] != NULL; i++) {
        if (g_strcmp0(id, reserved[i]) == 0)
            return TRUE;
    }

    return FALSE;
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

/* ── Removing what an agent owns ─────────────────────────────────── */

gboolean
clawt_remove_tree(const gchar *path, const gchar *root, GError **error)
{
    g_autoptr(GDir) dir = NULL;
    g_autofree gchar *resolved = NULL;
    const gchar *name;

    g_return_val_if_fail(path != NULL, FALSE);
    g_return_val_if_fail(root != NULL, FALSE);

    /*
     * A symlink is unlinked, never followed.
     *
     * An imported workspace may *be* a symlink into somebody's own
     * directory -- that is what `agent import --link` makes -- and
     * resolving it below would carry this into that directory and then
     * refuse, because it is outside the root. So the link would neither
     * be removed nor be safe to remove: `agent rm --purge` failed on
     * exactly the agents where following the path would have been worst.
     *
     * The link's own *location* is what has to be inside the root, so
     * that is what is checked. Removing the link leaves the directory it
     * pointed at completely alone, which is the whole reason somebody
     * chose to link rather than copy.
     */
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        g_autofree gchar *parent = g_path_get_dirname(path);
        g_autofree gchar *parent_resolved =
            clawt_canonicalize_missing(parent);
        g_autofree gchar *base = g_path_get_basename(path);
        g_autofree gchar *link_in_place = NULL;

        if (parent_resolved == NULL ||
            !clawt_path_is_within(parent_resolved, root)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "refusing to remove '%s': it is not inside '%s'",
                        path, root);
            return FALSE;
        }

        link_in_place = g_build_filename(parent_resolved, base, NULL);

        if (g_unlink(link_in_place) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not remove the link '%s': %s",
                        path, g_strerror(errno));
            return FALSE;
        }

        return TRUE;
    }

    /*
     * Refused before anything is touched, and refused on the
     * *canonical* path -- a symlink or a `..` inside a configured
     * workspace root would otherwise carry this somewhere nobody meant.
     */
    resolved = clawt_canonicalize_missing(path);

    if (resolved == NULL || !clawt_path_is_within(resolved, root)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "refusing to remove '%s': it is not inside '%s'",
                    path, root);
        return FALSE;
    }

    /* Asked for it gone, and it is gone. */
    if (!g_file_test(resolved, G_FILE_TEST_EXISTS))
        return TRUE;

    if (!g_file_test(resolved, G_FILE_TEST_IS_DIR)) {
        if (g_unlink(resolved) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "could not remove '%s': %s", resolved,
                        g_strerror(errno));
            return FALSE;
        }

        return TRUE;
    }

    dir = g_dir_open(resolved, 0, error);

    if (dir == NULL)
        return FALSE;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *child = g_build_filename(resolved, name, NULL);

        /*
         * The child is checked against the same root rather than against
         * its parent: a symlink pointing out of the tree is the case
         * this exists for, and it is only visible from the root.
         */
        if (!clawt_remove_tree(child, root, error))
            return FALSE;
    }

    g_clear_pointer(&dir, g_dir_close);

    if (g_rmdir(resolved) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not remove '%s': %s", resolved,
                    g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

const gchar *
clawt_color_ink(const gchar *hex)
{
    guint8 channel[3];
    gsize digits;
    gsize i;
    gdouble luminance;

    if (hex == NULL || hex[0] != '#')
        return NULL;

    digits = strlen(hex + 1);

    if (digits != 3 && digits != 6)
        return NULL;

    for (i = 0; i < digits; i++) {
        if (!g_ascii_isxdigit(hex[1 + i]))
            return NULL;
    }

    for (i = 0; i < 3; i++) {
        if (digits == 3) {
            gint v = g_ascii_xdigit_value(hex[1 + i]);

            channel[i] = (guint8)(v * 17);
        } else {
            channel[i] = (guint8)(g_ascii_xdigit_value(hex[1 + i * 2]) * 16 +
                                  g_ascii_xdigit_value(hex[2 + i * 2]));
        }
    }

    /*
     * The sRGB relative luminance WCAG uses, without the gamma
     * expansion.  The linearised form is more correct and the two
     * disagree only well away from the 0.5 boundary this compares
     * against, so the simpler one is used and said to be simpler rather
     * than presented as the real formula.
     */
    luminance = (0.2126 * channel[0] + 0.7152 * channel[1] +
                 0.0722 * channel[2]) / 255.0;

    return (luminance > 0.55) ? "#000000" : "#ffffff";
}

gchar *
clawt_time_ago_label(gint64 timestamp, gint64 now)
{
    gint64 seconds = (now - timestamp) / G_USEC_PER_SEC;

    if (seconds < 60)
        return g_strdup("just now");

    if (seconds < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", seconds / 60);

    if (seconds < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", seconds / 3600);

    return g_strdup_printf("%" G_GINT64_FORMAT "d ago", seconds / 86400);
}

/* ── Process trees ───────────────────────────────────────────────── */

GPid
clawt_process_parent_of(GPid pid)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    const gchar *line;

    if (pid <= 0)
        return 0;

    path = g_strdup_printf("/proc/%d/status", (gint)pid);

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return 0;

    /*
     * `status` rather than `stat`, because the comm field in `stat` is
     * wrapped in brackets and may contain both `)` and spaces -- so a
     * field split of it reads the wrong column for any process whose
     * name has one, which is every shell one-liner an agent runs.
     */
    line = strstr(contents, "\nPPid:");

    if (line == NULL)
        return 0;

    return (GPid)g_ascii_strtoll(line + strlen("\nPPid:"), NULL, 10);
}

gboolean
clawt_process_is_descendant_of(GPid pid, GPid root)
{
    guint hops;

    if (pid <= 0 || root <= 0 || pid == root)
        return FALSE;

    /*
     * Bounded, because the walk is over a tree read from /proc one
     * process at a time and nothing guarantees the snapshot is
     * consistent: a reparent between two reads could in principle be
     * seen as a cycle, and an unbounded loop here would hang the daemon
     * rather than answer wrongly.
     */
    for (hops = 0; hops < 64; hops++) {
        pid = clawt_process_parent_of(pid);

        if (pid == root)
            return TRUE;

        /* 1 is init, and 0 means the process went away mid-walk. */
        if (pid <= 1)
            return FALSE;
    }

    return FALSE;
}

/*
 * Depth-first pre-order: a parent is appended before everything below
 * it, so reversing the result puts every descendant before its parent.
 */
static void
collect_children(GPid parent, GArray *out, guint depth)
{
    g_autoptr(GDir) proc = NULL;
    const gchar *name;

    /*
     * A tree deeper than this is a runaway rather than an agent's work,
     * and the recursion is on the daemon's own stack.
     */
    if (depth > 32)
        return;

    proc = g_dir_open("/proc", 0, NULL);

    if (proc == NULL)
        return;

    while ((name = g_dir_read_name(proc)) != NULL) {
        GPid pid;

        if (!g_ascii_isdigit(name[0]))
            continue;

        pid = (GPid)g_ascii_strtoll(name, NULL, 10);

        if (pid <= 1 || pid == parent)
            continue;

        if (clawt_process_parent_of(pid) != parent)
            continue;

        g_array_append_val(out, pid);
        collect_children(pid, out, depth + 1);
    }
}

GArray *
clawt_process_descendants(GPid root)
{
    GArray *found = g_array_new(FALSE, FALSE, sizeof(GPid));
    GArray *ordered;
    guint i;

    if (root <= 1)
        return found;

    collect_children(root, found, 0);

    /*
     * Reversed, so the deepest process is signalled first.  Killing a
     * parent before its children hands those children to init, and they
     * are then no longer reachable from @root at all -- an agent's stop
     * would leave the compiler it had launched running for ever.
     */
    ordered = g_array_sized_new(FALSE, FALSE, sizeof(GPid), found->len);

    for (i = found->len; i > 0; i--)
        g_array_append_val(ordered, g_array_index(found, GPid, i - 1));

    g_array_unref(found);

    return ordered;
}

/*
 * The shell construct a command is relying on and will not get.
 *
 * Both routes to clawtilla_computer_exec hand a command line to
 * g_shell_parse_argv() and then spawn the result directly.  That applies
 * shell *lexing* -- quote removal and word splitting -- and no shell
 * *semantics*, so `;`, `&&`, `|`, redirections, backquotes, `$(...)` and
 * `$VAR` all survive as ordinary characters in argv and are handed to
 * the program as literal text.
 *
 * The failure is the bad kind.  Nothing errors: the command runs, exits
 * 0, and prints the rest of the line back.  `echo "whoami=$(whoami)"; for
 * p in ...; do ...; done` came back with the loop printed verbatim and
 * $(whoami) unexpanded, which from the agent's side reads as a command
 * that ran and produced strange output rather than as a command that was
 * never a command.  Confinement is why there is no shell -- the sandbox
 * inspects the translated argv -- so the answer is to say so, loudly,
 * rather than to wrap it.
 *
 * Scanned on the raw string rather than on the parsed argv, because
 * after lexing the quotes are gone and `grep 'a|b' f` is
 * indistinguishable from `a | b`.  Quoting is exactly what separates a
 * character somebody meant literally from one they expected a shell to
 * act on, so it has to be read before it is discarded.
 *
 * Globs are deliberately *not* flagged.  An unquoted `*.log` reaches the
 * program unchanged, which is sometimes what the program wanted; a
 * refusal there would break commands that work today.
 */
gchar *
clawt_command_shell_syntax_refusal(const gchar *command)
{
    const gchar *found = NULL;
    const gchar *p;
    gchar quote = '\0';

    if (command == NULL)
        return NULL;

    for (p = command; *p != '\0' && found == NULL; p++) {
        if (quote == '\'') {
            /* Single quotes protect everything, including a backslash. */
            if (*p == '\'')
                quote = '\0';
            continue;
        }

        if (*p == '\\') {
            /* An escaped character is literal wherever it appears. */
            if (p[1] != '\0')
                p++;
            continue;
        }

        if (quote == '"') {
            if (*p == '"')
                quote = '\0';
            else if (*p == '`')
                found = "a `backquoted command`";
            else if (*p == '$' && p[1] == '(')
                found = "a $(...) command substitution";
            else if (*p == '$' && (p[1] == '{' || g_ascii_isalpha(p[1]) ||
                                   p[1] == '_'))
                found = "a $VAR reference";

            continue;
        }

        switch (*p) {
        case '\'':
        case '"':
            quote = *p;
            break;
        case ';':
            found = "a ; between commands";
            break;
        case '|':
            found = "a pipe";
            break;
        case '&':
            found = "an & or &&";
            break;
        case '<':
        case '>':
            found = "a redirection";
            break;
        case '\n':
            found = "a line break between commands";
            break;
        case '`':
            found = "a `backquoted command`";
            break;
        case '$':
            if (p[1] == '(')
                found = "a $(...) command substitution";
            else if (p[1] == '{' || g_ascii_isalpha(p[1]) || p[1] == '_')
                found = "a $VAR reference";
            break;
        default:
            break;
        }
    }

    if (found == NULL)
        return NULL;

    /*
     * The remedy is named because a refusal an agent cannot act on is a
     * refusal it will retry in the same shape.  bash -c keeps the
     * sandbox honest: clawt_sandbox_check_argv() re-parses the nested
     * command line and inspects that too, so this is a supported route
     * rather than a way around confinement.
     */
    return g_strdup_printf(
        "That command contains %s, and this does not run commands through "
        "a shell -- it splits the line into arguments and runs the program "
        "directly. Pipes, redirections, ;, &&, backquotes, $(...) and "
        "$VAR would all arrive as literal text and the command would "
        "appear to succeed while doing nothing you asked for. Run it as: "
        "bash -c \"<the whole command line>\"", found);
}

gboolean
clawt_log_level_permits(ClawtLogLevel  ceiling,
                        GLogLevelFlags level)
{
    /*
     * Initialised, even though the switch below is exhaustive: GCC
     * cannot prove an enum-typed *parameter* only ever holds declared
     * members, so a release build warns, and this project's builds have
     * to be warning-free.
     *
     * The initialiser is the most permissive level rather than the
     * quietest, so a member the switch has not been taught about writes
     * too much rather than silently writing nothing. -Wswitch still
     * names it at compile time, which is the check that actually
     * catches it.
     */
    GLogLevelFlags bound = G_LOG_LEVEL_DEBUG;

    /*
     * The ceiling as a GLib flag. Written as a switch with no default:
     * -Wswitch names a ClawtLogLevel added later rather than letting it
     * fall through to whatever the last case happened to leave behind.
     */
    switch (ceiling) {
    case CLAWT_LOG_ERROR:
        /*
         * G_LOG_LEVEL_CRITICAL, not G_LOG_LEVEL_ERROR.
         *
         * GLib's G_LOG_LEVEL_ERROR is the *fatal* one -- it aborts the
         * process -- and g_critical() is what an ordinary failure is
         * logged at.  Bounding at ERROR would therefore mean `error`
         * kept only the messages printed on the way to a crash and
         * dropped every failure the daemon survived, which is the
         * opposite of "only failures" and would be invisible: the
         * setting would look like it was working.
         */
        bound = G_LOG_LEVEL_CRITICAL;
        break;
    case CLAWT_LOG_WARNING:
        bound = G_LOG_LEVEL_WARNING;
        break;
    case CLAWT_LOG_INFO:
        bound = G_LOG_LEVEL_INFO;
        break;
    case CLAWT_LOG_DEBUG:
        bound = G_LOG_LEVEL_DEBUG;
        break;
    }

    level &= G_LOG_LEVEL_MASK;

    /*
     * Anything that is not one of the four is passed through. Testing
     * for membership rather than comparing straight away is what keeps
     * a custom level from being judged against a scale it is not on.
     */
    if (level != G_LOG_LEVEL_ERROR &&
        level != G_LOG_LEVEL_CRITICAL &&
        level != G_LOG_LEVEL_WARNING &&
        level != G_LOG_LEVEL_MESSAGE &&
        level != G_LOG_LEVEL_INFO &&
        level != G_LOG_LEVEL_DEBUG)
        return TRUE;

    /* Lower is more severe, so "at or above the ceiling" is a <=. */
    return level <= bound;
}

/**
 * clawt_clip_line:
 * @value: (nullable): the text
 * @limit: how many characters to keep
 *
 * Clips a field to @limit characters, on a character boundary.
 *
 * These fields come out of clawtilla.yaml, and with an imported team
 * they were written by somebody who is not the operator.  They are being
 * interpolated into a prompt the agent treats as trustworthy, so the one
 * thing that must not be possible is a description long enough to be the
 * rest of the prompt.
 *
 * g_utf8_offset_to_pointer() rather than a byte count: a cut in the
 * middle of a sequence produces a replacement character in the middle of
 * a name, which reads as data corruption rather than as a clip.
 *
 * Returns: (transfer full) (nullable): one clipped line, or %NULL when
 *   @value is empty
 */
gchar *
clawt_clip_line(const gchar *value, glong limit)
{
    g_autofree gchar *line = NULL;
    const gchar *newline;

    if (value == NULL || *value == '\0')
        return NULL;

    /*
     * One line each. A description with a newline in it would otherwise
     * break the one-agent-per-line shape the listing promises, and a
     * reader -- model or person -- would take the second line for
     * another agent.
     */
    newline = strchr(value, '\n');
    line = (newline != NULL) ? g_strndup(value, (gsize)(newline - value))
                             : g_strdup(value);
    g_strstrip(line);

    if (*line == '\0')
        return NULL;

    if (g_utf8_strlen(line, -1) <= limit)
        return g_steal_pointer(&line);

    {
        const gchar *end = g_utf8_offset_to_pointer(line, limit);
        g_autofree gchar *cut = g_strndup(line, (gsize)(end - line));

        g_strchomp(cut);
        return g_strdup_printf("%s...", cut);
    }
}
