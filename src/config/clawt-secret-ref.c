/*
 * clawt-secret-ref.c - A reference to a secret, never the secret itself
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "config/clawt-secret-ref.h"

#include <yaml-glib.h>
#include <string.h>

struct _ClawtSecretRef {
    ClawtSecretBackend  backend;
    gchar              *locator;
};

G_DEFINE_BOXED_TYPE(ClawtSecretRef, clawt_secret_ref,
                    clawt_secret_ref_copy, clawt_secret_ref_free)

ClawtSecretRef *
clawt_secret_ref_new(ClawtSecretBackend backend, const gchar *locator)
{
    ClawtSecretRef *self;

    g_return_val_if_fail(locator != NULL, NULL);

    self = g_new0(ClawtSecretRef, 1);
    self->backend = backend;
    self->locator = g_strdup(locator);

    return self;
}

ClawtSecretRef *
clawt_secret_ref_copy(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return clawt_secret_ref_new(self->backend, self->locator);
}

void
clawt_secret_ref_free(ClawtSecretRef *self)
{
    if (self == NULL)
        return;

    /*
     * The locator is not itself secret -- it is a path or a variable name --
     * but zeroing costs nothing and keeps the habit for the one place it
     * would matter if this type ever grew a cached value.
     */
    if (self->locator != NULL)
        memset(self->locator, 0, strlen(self->locator));

    g_free(self->locator);
    g_free(self);
}

ClawtSecretBackend
clawt_secret_ref_get_backend(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_SECRET_BACKEND_FILE);

    return self->backend;
}

const gchar *
clawt_secret_ref_get_locator(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->locator;
}

ClawtSecretRef *
clawt_secret_ref_parse(gpointer             spec,
                       ClawtSecretBackend   default_backend,
                       GError             **error)
{
    YamlNode *node = spec;
    YamlMapping *mapping;
    GList *members;
    GList *l;

    g_return_val_if_fail(node != NULL, NULL);

    /*
     * A bare string uses the default backend.  It exists so the common case
     * -- a file in the configured secrets directory -- does not need the
     * mapping form for every credential.
     */
    if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
        const gchar *value = yaml_node_get_string(node);

        if (value == NULL || *value == '\0') {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                                "secret reference is empty");
            return NULL;
        }

        return clawt_secret_ref_new(default_backend, value);
    }

    if (yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                            "secret reference must be a string or a "
                            "one-key mapping such as {env: NAME}");
        return NULL;
    }

    mapping = yaml_node_get_mapping(node);
    members = yaml_mapping_get_members(mapping);

    for (l = members; l != NULL; l = l->next) {
        const gchar *key = l->data;
        YamlNode *value_node = yaml_mapping_get_member(mapping, key);
        const gchar *value;
        gint backend = -1;

        if (!clawt_enum_from_nick(CLAWT_TYPE_SECRET_BACKEND, key, &backend))
            continue;

        value = yaml_node_get_string(value_node);
        if (value == NULL || *value == '\0') {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                        "secret reference '%s' has no value", key);
            g_list_free(members);
            return NULL;
        }

        g_list_free(members);
        return clawt_secret_ref_new((ClawtSecretBackend)backend, value);
    }

    g_list_free(members);

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                        "secret reference names no known backend; "
                        "expected one of file, env or command");
    return NULL;
}

/*
 * Reads a file, trimming exactly one trailing newline.
 *
 * Trimming matters: `echo secret > file` is how everybody creates these, and
 * a token with a newline on the end fails authentication in a way that looks
 * like a wrong token rather than a wrong file.
 */
static gchar *
resolve_file(const gchar *locator, const gchar *base_dir, GError **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;

    if (g_path_is_absolute(locator) || locator[0] == '~')
        path = clawt_expand_path(locator);
    else if (base_dir != NULL)
        path = g_build_filename(base_dir, locator, NULL);
    else
        path = g_strdup(locator);

    if (!g_file_get_contents(path, &contents, &length, error)) {
        g_prefix_error(error, "secret file %s: ", path);
        return NULL;
    }

    while (length > 0 &&
           (contents[length - 1] == '\n' || contents[length - 1] == '\r'))
        length--;

    return g_strndup(contents, length);
}

static gchar *
resolve_env(const gchar *locator, GError **error)
{
    const gchar *value = g_getenv(locator);

    if (value == NULL || *value == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "environment variable %s is not set", locator);
        return NULL;
    }

    return g_strdup(value);
}

typedef struct {
    GSubprocess *proc;
    GMainLoop   *loop;
    gboolean     timed_out;
    guint        timeout_id;
} CommandWait;

static gboolean
on_command_timeout(gpointer user_data)
{
    CommandWait *wait = user_data;

    wait->timed_out = TRUE;
    g_subprocess_force_exit(wait->proc);
    wait->timeout_id = 0;

    return G_SOURCE_REMOVE;
}

static void
on_command_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    CommandWait *wait = user_data;

    (void)source;
    (void)result;

    g_main_loop_quit(wait->loop);
}

/*
 * Runs a command and takes its stdout.
 *
 * The timeout is the whole point of doing this the long way rather than
 * with g_subprocess_communicate_utf8(): `pass show ...` against a locked
 * keyring waits for a passphrase that will never come on a daemon with no
 * terminal, and without a bound it would hang startup rather than failing.
 */
static gchar *
resolve_command(const gchar *locator,
                guint        timeout_seconds,
                GError     **error)
{
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GMainContext) context = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    g_autofree gchar *stdout_buf = NULL;
    g_auto(GStrv) argv = NULL;
    CommandWait wait;
    gsize length;

    if (!g_shell_parse_argv(locator, NULL, &argv, error)) {
        g_prefix_error(error, "secret command is not parseable: ");
        return NULL;
    }

    proc = g_subprocess_newv((const gchar * const *)argv,
                             G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                             G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                             error);
    if (proc == NULL) {
        g_prefix_error(error, "secret command failed to start: ");
        return NULL;
    }

    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    loop = g_main_loop_new(context, FALSE);

    wait.proc = proc;
    wait.loop = loop;
    wait.timed_out = FALSE;
    wait.timeout_id = 0;

    if (timeout_seconds > 0)
        wait.timeout_id = g_timeout_add_seconds(timeout_seconds,
                                                on_command_timeout, &wait);

    g_subprocess_communicate_utf8_async(proc, NULL, NULL,
                                        on_command_done, &wait);
    g_main_loop_run(loop);

    if (wait.timeout_id != 0)
        g_source_remove(wait.timeout_id);

    g_main_context_pop_thread_default(context);

    if (wait.timed_out) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                    "secret command did not finish within %u seconds: %s",
                    timeout_seconds, locator);
        return NULL;
    }

    if (!g_subprocess_communicate_utf8_finish(proc, NULL, &stdout_buf, NULL,
                                              error)) {
        g_prefix_error(error, "secret command failed: ");
        return NULL;
    }

    if (!g_subprocess_get_successful(proc)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "secret command exited non-zero: %s", locator);
        return NULL;
    }

    if (stdout_buf == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "secret command produced no output: %s", locator);
        return NULL;
    }

    length = strlen(stdout_buf);
    while (length > 0 &&
           (stdout_buf[length - 1] == '\n' || stdout_buf[length - 1] == '\r'))
        length--;

    if (length == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "secret command produced only whitespace: %s", locator);
        return NULL;
    }

    return g_strndup(stdout_buf, length);
}

gchar *
clawt_secret_ref_resolve(ClawtSecretRef  *self,
                         const gchar     *base_dir,
                         guint            command_timeout_seconds,
                         GError         **error)
{
    g_return_val_if_fail(self != NULL, NULL);

    switch (self->backend) {
    case CLAWT_SECRET_BACKEND_FILE:
        return resolve_file(self->locator, base_dir, error);

    case CLAWT_SECRET_BACKEND_ENV:
        return resolve_env(self->locator, error);

    case CLAWT_SECRET_BACKEND_COMMAND:
        return resolve_command(self->locator, command_timeout_seconds, error);

    default:
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                            "unknown secret backend");
        return NULL;
    }
}

gchar *
clawt_secret_ref_describe(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    /*
     * The locator is included because it is not the secret and it is what
     * makes an error actionable -- "env:ANTHROPIC_API_KEY is not set" tells
     * you what to do; "a secret could not be resolved" does not.
     */
    return g_strdup_printf("%s:%s",
                           clawt_enum_to_nick(CLAWT_TYPE_SECRET_BACKEND,
                                              self->backend),
                           self->locator);
}
