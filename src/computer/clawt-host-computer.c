/*
 * clawt-host-computer.c - The real machine clawtilla runs on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-host-computer.h"

#include <glib/gstdio.h>
#include <string.h>

/*
 * How much output one command may return.
 *
 * Unbounded output is a real failure mode rather than a theoretical one: an
 * agent that runs `find /` produces a reply too large to send and too large
 * to reason about, and the turn is wasted either way.
 */
#define MAX_OUTPUT_BYTES (256 * 1024)

struct _ClawtHostComputer {
    ClawtComputer parent_instance;

    ClawtSandbox *sandbox;
    gint          nice_level;
};

G_DEFINE_FINAL_TYPE(ClawtHostComputer, clawt_host_computer,
                    CLAWT_TYPE_COMPUTER)

ClawtComputer *
clawt_host_computer_new(const gchar *agent_id, ClawtSandbox *sandbox)
{
    ClawtHostComputer *self;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(sandbox), NULL);

    self = g_object_new(CLAWT_TYPE_HOST_COMPUTER, NULL);
    self->sandbox = g_object_ref(sandbox);

    clawt_computer_bind_agent(CLAWT_COMPUTER(self), agent_id);

    return CLAWT_COMPUTER(self);
}

ClawtSandbox *
clawt_host_computer_get_sandbox(ClawtHostComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_HOST_COMPUTER(self), NULL);

    return self->sandbox;
}

void
clawt_host_computer_set_nice(ClawtHostComputer *self, gint nice_level)
{
    g_return_if_fail(CLAWT_IS_HOST_COMPUTER(self));

    self->nice_level = nice_level;
}

/*
 * The host is already there, so provisioning is only a check that the
 * confinement asked for can actually be applied.  Doing it here rather than
 * at the first command means a missing bwrap surfaces when the agent
 * starts, not three turns into a conversation.
 */
static gboolean
host_provision(ClawtComputer *computer, GError **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    GPtrArray *mounts;
    guint i;

    /*
     * A declared mount is a statement that the agent may reach that
     * directory.  On a container it becomes a real mount and the kernel
     * enforces it; on the host there is nothing to enforce it, so the
     * sources are added to what the sandbox allows.  Without this an
     * agent is handed an exchange directory it is then refused access
     * to, which reads as a bug in the confinement rather than as the
     * mount never having been applied.
     */
    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        clawt_sandbox_add_mount_path(self->sandbox,
                                     clawt_mount_get_source(mount));
    }

    if (!clawt_sandbox_is_available(self->sandbox, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);

    return TRUE;
}

static gboolean
host_start(ClawtComputer *computer, GError **error)
{
    return host_provision(computer, error);
}

static gboolean
host_stop(ClawtComputer *computer, GError **error)
{
    (void)error;

    /*
     * Nothing to stop.  The host outlives the agent, which is exactly why
     * this backend needs confinement rather than isolation.
     */
    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    return TRUE;
}

typedef struct {
    GSubprocess *process;
    GMainLoop   *loop;
    gboolean     timed_out;
    gchar       *stdout_text;
    gchar       *stderr_text;
    GError      *error;
} ExecWait;

static gboolean
on_exec_timeout(gpointer user_data)
{
    ExecWait *wait = user_data;

    wait->timed_out = TRUE;
    g_subprocess_force_exit(wait->process);

    return G_SOURCE_REMOVE;
}

static void
on_exec_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExecWait *wait = user_data;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                         &wait->stdout_text,
                                         &wait->stderr_text,
                                         &wait->error);
    g_main_loop_quit(wait->loop);
}

/*
 * Translates an in-computer path to where it really is on this machine.
 *
 * A host computer has no mount namespace, so a mount is not a mount: it is
 * a promise that a path inside the agent's world means a directory
 * outside it.  Nothing enforces that promise for us, so the translation
 * happens here -- otherwise an agent told its exchange is at
 * /mnt/clawtilla/exchange would be refused for using the path it was
 * given, which is a maddening thing to debug.
 *
 * The result still goes through the confinement check: this maps a path,
 * it does not bless one.
 */
static gchar *
translate_mount_path(ClawtComputer *computer, const gchar *path)
{
    GPtrArray *mounts;
    guint i;

    if (path == NULL)
        return NULL;

    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        const gchar *target = clawt_mount_get_target(mount);
        gsize length;

        if (target == NULL || !g_str_has_prefix(path, target))
            continue;

        length = strlen(target);

        /*
         * A prefix match is not enough: "/mnt/clawtillax" starts with
         * "/mnt/clawtilla" and is somewhere else.
         */
        if (path[length] != '\0' && path[length] != G_DIR_SEPARATOR)
            continue;

        return g_build_filename(clawt_mount_get_source(mount),
                                path + length, NULL);
    }

    return clawt_expand_path(path);
}

/*
 * Rewrites every argument that names a mount target.
 *
 * Whole arguments only: rewriting inside a longer string would mean
 * guessing at quoting and would change text the agent meant literally.
 */
static GStrv
translate_argv(ClawtComputer *computer, const gchar * const *argv)
{
    g_autoptr(GPtrArray) out = g_ptr_array_new();
    gsize i;

    for (i = 0; argv != NULL && argv[i] != NULL; i++) {
        if (g_path_is_absolute(argv[i]))
            g_ptr_array_add(out, translate_mount_path(computer, argv[i]));
        else
            g_ptr_array_add(out, g_strdup(argv[i]));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

static ClawtExecResult *
host_exec(ClawtComputer        *computer,
          const gchar * const  *argv,
          const gchar          *working_dir,
          guint                 timeout_seconds,
          GCancellable         *cancellable,
          GError              **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_auto(GStrv) wrapped = NULL;
    g_auto(GStrv) translated = NULL;
    g_autoptr(GMainContext) context = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    g_autofree gchar *bounded_stdout = NULL;
    g_autofree gchar *bounded_stderr = NULL;
    ClawtExecResult *result;
    ExecWait wait;
    GSource *timeout_source = NULL;
    gboolean truncated = FALSE;
    gboolean stderr_truncated = FALSE;

    /*
     * Mount targets are rewritten to where they really are before
     * anything is checked or run.  On a container the kernel does this;
     * on the host nothing does, so an agent using the path it was given
     * would be refused for naming its own exchange directory.
     */
    translated = translate_argv(computer, argv);

    /*
     * Checked before anything is spawned.  A command that would reach
     * outside the agent's boundary must never start, not be killed
     * afterwards.
     */
    if (!clawt_sandbox_check_argv(self->sandbox,
                                  (const gchar * const *)translated, error))
        return NULL;

    wrapped = clawt_sandbox_wrap_argv(self->sandbox,
                                      (const gchar * const *)translated);

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_PIPE);

    if (working_dir != NULL) {
        g_autofree gchar *expanded = clawt_expand_path(working_dir);

        if (!clawt_sandbox_path_is_allowed(self->sandbox, expanded)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                        "'%s' is outside what this agent may reach",
                        expanded);
            return NULL;
        }

        g_subprocess_launcher_set_cwd(launcher, expanded);
    }

    memset(&wait, 0, sizeof(wait));

    wait.process = g_subprocess_launcher_spawnv(
        launcher, (const gchar * const *)wrapped, error);

    if (wait.process == NULL) {
        g_prefix_error(error, "running %s: ", argv[0]);
        return NULL;
    }

    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    loop = g_main_loop_new(context, FALSE);
    wait.loop = loop;

    /*
     * A timeout is not optional in practice.  An agent that runs an
     * interactive command by mistake waits for input that never arrives,
     * and without this the turn simply never ends.
     *
     * The source is attached to the context we pushed, not added with
     * g_timeout_add_seconds().  That helper attaches to the DEFAULT main
     * context, which this loop is not running -- so the timeout would never
     * fire and every hanging command would hang for ever.
     */
    if (timeout_seconds > 0) {
        timeout_source = g_timeout_source_new_seconds(timeout_seconds);
        g_source_set_callback(timeout_source, on_exec_timeout, &wait, NULL);
        g_source_attach(timeout_source, context);
    }

    g_subprocess_communicate_utf8_async(wait.process, NULL, cancellable,
                                        on_exec_done, &wait);
    g_main_loop_run(loop);

    if (timeout_source != NULL) {
        g_source_destroy(timeout_source);
        g_source_unref(timeout_source);
    }

    g_main_context_pop_thread_default(context);

    if (wait.error != NULL) {
        g_propagate_error(error, wait.error);
        g_clear_object(&wait.process);
        g_free(wait.stdout_text);
        g_free(wait.stderr_text);
        return NULL;
    }

    if (wait.timed_out) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                    "'%s' did not finish within %u seconds and was stopped",
                    argv[0], timeout_seconds);
        g_clear_object(&wait.process);
        g_free(wait.stdout_text);
        g_free(wait.stderr_text);
        return NULL;
    }

    bounded_stdout = clawt_computer_truncate_output(wait.stdout_text,
                                                    MAX_OUTPUT_BYTES,
                                                    &truncated);
    bounded_stderr = clawt_computer_truncate_output(wait.stderr_text,
                                                    MAX_OUTPUT_BYTES,
                                                    &stderr_truncated);

    result = clawt_exec_result_new(g_subprocess_get_exit_status(wait.process),
                                   bounded_stdout, bounded_stderr);
    clawt_exec_result_set_truncated(result, truncated || stderr_truncated);

    g_clear_object(&wait.process);
    g_free(wait.stdout_text);
    g_free(wait.stderr_text);

    return result;
}

/*
 * Copying on the host is a copy, not a transfer -- but it still goes
 * through the confinement check, or an agent could write anywhere simply by
 * calling put_file instead of running cp.
 */
static gboolean
host_put_file(ClawtComputer  *computer,
              const gchar    *local_path,
              const gchar    *remote_path,
              GError        **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autofree gchar *contents = NULL;
    g_autofree gchar *destination = translate_mount_path(computer,
                                                         remote_path);
    gsize length = 0;

    if (!clawt_sandbox_path_is_allowed(self->sandbox, destination)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach", destination);
        return FALSE;
    }

    if (!g_file_get_contents(local_path, &contents, &length, error))
        return FALSE;

    return g_file_set_contents(destination, contents, (gssize)length, error);
}

static gboolean
host_get_file(ClawtComputer  *computer,
              const gchar    *remote_path,
              const gchar    *local_path,
              GError        **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autofree gchar *contents = NULL;
    g_autofree gchar *source = translate_mount_path(computer, remote_path);
    gsize length = 0;

    if (!clawt_sandbox_path_is_allowed(self->sandbox, source)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach", source);
        return FALSE;
    }

    if (!g_file_get_contents(source, &contents, &length, error))
        return FALSE;

    return g_file_set_contents(local_path, contents, (gssize)length, error);
}

static gchar *
host_describe(ClawtComputer *computer)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autofree gchar *confinement = clawt_sandbox_describe(self->sandbox);
    g_autoptr(GString) out = g_string_new(NULL);
    GPtrArray *mounts;
    guint i;

    g_string_append_printf(
        out,
        "You can run commands on the machine clawtilla itself is running "
        "on. %s", confinement);

    /*
     * The mounts are listed by name, because an agent told only what it
     * may not reach spends turns discovering what it may.
     */
    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        if (i == 0)
            g_string_append(out, "\n\nYou can also reach:");

        g_string_append_printf(out, "\n  %s (%s on this machine)",
                               clawt_mount_get_target(mount),
                               clawt_mount_get_source(mount));
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static ClawtComputerType
host_get_computer_type(ClawtComputer *computer)
{
    (void)computer;
    return CLAWT_COMPUTER_HOST;
}

static void
clawt_host_computer_dispose(GObject *object)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(object);

    g_clear_object(&self->sandbox);

    G_OBJECT_CLASS(clawt_host_computer_parent_class)->dispose(object);
}

static void
clawt_host_computer_class_init(ClawtHostComputerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    object_class->dispose = clawt_host_computer_dispose;

    computer_class->provision = host_provision;
    computer_class->start = host_start;
    computer_class->stop = host_stop;
    computer_class->exec = host_exec;
    computer_class->put_file = host_put_file;
    computer_class->get_file = host_get_file;
    computer_class->describe = host_describe;
    computer_class->get_computer_type = host_get_computer_type;
}

static void
clawt_host_computer_init(ClawtHostComputer *self)
{
    self->nice_level = 0;
}
