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
#include <signal.h>
#include <unistd.h>

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
    GSource     *timeout_source;
	GAsyncResult *result;
	GCancellable *cancellable;
	pid_t process_group;
	gboolean result_released;
} CommandWait;

/**
 * command_child_setup:
 * @user_data: unused
 *
 * Isolate the command and its children for deadline cleanup. Only
 * async-signal-safe operations are permitted between fork and exec.
 */
static void
command_child_setup(gpointer user_data)
{
	(void)user_data;
	if (setpgid(0, 0) != 0)
		_exit(127);
}

static gboolean
on_command_timeout(gpointer user_data)
{
    CommandWait *wait = user_data;

    wait->timed_out = TRUE;
	/* Kill descendants too, including when the direct child already exited. */
	if (wait->process_group > 0)
		(void)kill(-wait->process_group, SIGKILL);
    g_subprocess_force_exit(wait->proc);
	/* An escaped descendant may retain stdout; cancellation bounds the read. */
	g_cancellable_cancel(wait->cancellable);

    return G_SOURCE_REMOVE;
}

static void
on_command_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    CommandWait *wait = user_data;

    (void)source;
	/* The callback's borrowed result must outlive this callback. */
	wait->result = g_object_ref(result);

    g_main_loop_quit(wait->loop);
}

/**
 * on_command_result_released:
 * @user_data: command whose cancelled operations are settling
 * @object: finalized completion result
 *
 * Communicate may report its first error before its sibling read/wait
 * callbacks finish. Those callbacks retain the result; keep their private
 * context running until the final reference is released. Cancellation
 * covers both operations, so this never waits for a descendant's pipe EOF.
 */
static void
on_command_result_released(gpointer user_data, GObject *object)
{
	CommandWait *wait = user_data;

	(void)object;
	wait->result_released = TRUE;
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
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GCancellable) cancellable = NULL;
    g_autoptr(GMainContext) context = NULL;
    g_autoptr(GMainLoop) loop = NULL;
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) command_error = NULL;
    g_autofree gchar *stdout_buf = NULL;
    g_auto(GStrv) argv = NULL;
    CommandWait wait;
    gsize length;
	gboolean communicated;
	const gchar *identifier;

    if (!g_shell_parse_argv(locator, NULL, &argv, error)) {
        g_prefix_error(error, "secret command is not parseable: ");
        return NULL;
    }

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_subprocess_launcher_set_child_setup(launcher, command_child_setup,
		NULL, NULL);
	proc = g_subprocess_launcher_spawnv(launcher,
		(const gchar * const *)argv, error);
    if (proc == NULL) {
        g_prefix_error(error, "secret command failed to start: ");
        return NULL;
    }

    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    loop = g_main_loop_new(context, FALSE);

    wait.proc = proc;
	cancellable = g_cancellable_new();
	wait.cancellable = cancellable;
	/* An already-reaped child has no identifier; never signal group zero. */
	identifier = g_subprocess_get_identifier(proc);
	wait.process_group = identifier != NULL ?
		(pid_t)g_ascii_strtoll(identifier, NULL, 10) : 0;
    wait.loop = loop;
    wait.timed_out = FALSE;
    wait.timeout_source = NULL;
	wait.result = NULL;
	wait.result_released = FALSE;

    /*
     * Attached to the context we pushed, not added with
     * g_timeout_add_seconds().  That helper attaches to the DEFAULT main
     * context, which this loop is not running -- so the timeout would never
     * fire, and a locked password manager would hang startup exactly as it
     * would have with no timeout at all.
     */
    if (timeout_seconds > 0) {
		/* Use an exact monotonic deadline without seconds-source coalescing. */
		wait.timeout_source = g_timeout_source_new(0);
		g_source_set_ready_time(wait.timeout_source,
			g_get_monotonic_time() + (gint64)timeout_seconds * G_USEC_PER_SEC);
        g_source_set_callback(wait.timeout_source, on_command_timeout,
                              &wait, NULL);
        g_source_attach(wait.timeout_source, context);
    }

    g_subprocess_communicate_utf8_async(proc, NULL, cancellable,
                                        on_command_done, &wait);
    g_main_loop_run(loop);

    if (wait.timeout_source != NULL) {
        g_source_destroy(wait.timeout_source);
        g_source_unref(wait.timeout_source);
    }

	result = g_steal_pointer(&wait.result);
	/* Finish every completed operation once, including timed-out commands. */
	communicated = g_subprocess_communicate_utf8_finish(proc, result,
		&stdout_buf, NULL, &command_error);
	/* Do not abandon the context while cancelled sibling tasks still own it. */
	g_object_weak_ref(G_OBJECT(result), on_command_result_released, &wait);
	g_clear_object(&result);
	if (!wait.result_released)
		g_main_loop_run(loop);
	g_main_context_pop_thread_default(context);

    if (wait.timed_out) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                    "secret command did not finish within %u seconds: %s",
                    timeout_seconds, locator);
        return NULL;
    }

    if (!communicated) {
		g_propagate_error(error, g_steal_pointer(&command_error));
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
