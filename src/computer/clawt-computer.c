/*
 * clawt-computer.c - What an agent can run commands on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-computer.h"

#include <string.h>

enum {
    SIGNAL_STATE_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct {
    gchar              *agent_id;
    GPtrArray          *mounts;
    ClawtComputerState  state;
    gchar              *last_error;

    /*
     * computer.start: on_first_use, and the one start a wave of first
     * uses shares. Everything from start_lock down is read and written
     * under it: a caller finding @starting set waits on @start_done for
     * @start_generation to move and takes the outcome left behind.
     * @starter is the thread doing the start, so a backend that reaches
     * clawt_computer_exec() from inside its own start is not made to wait
     * on itself.
     */
    gboolean            first_use;
    GMutex              start_lock;
    GCond               start_done;
    gboolean            starting;
    GThread            *starter;
    guint64             start_generation;
    gboolean            start_ok;
    GError             *start_error;
} ClawtComputerPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(ClawtComputer, clawt_computer,
                                    G_TYPE_OBJECT)

#define PRIV(self) \
    ((ClawtComputerPrivate *) \
     clawt_computer_get_instance_private(CLAWT_COMPUTER(self)))

#define CALL_OR_TRUE(self, method, error)                                  \
    do {                                                                   \
        ClawtComputerClass *klass = CLAWT_COMPUTER_GET_CLASS(self);        \
        if (klass->method == NULL)                                         \
            return TRUE;                                                   \
        return klass->method(self, error);                                 \
    } while (0)

gboolean
clawt_computer_provision(ClawtComputer *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    CALL_OR_TRUE(self, provision, error);
}

gboolean
clawt_computer_start(ClawtComputer *self, GError **error)
{
    ClawtComputerClass *klass;
    g_autoptr(GError) stale = NULL;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    /*
     * Ask what is really there before replaying what we remember.
     *
     * Here rather than inside each backend's start(), which is where it
     * began: the container backend called its own static reconcile and
     * the public entry point had no caller at all, so a backend that
     * grew a reconcile later would have had it registered, documented,
     * and never invoked.  This tree has had that shape three times --
     * the factory nothing called, the signal nothing connected, the
     * limit nothing incremented -- and it is always found by grepping
     * for the caller rather than by reading the implementation.
     *
     * A backend that cannot be asked is not a reason to refuse the
     * start: whatever is wrong is about to produce a better message from
     * start() itself than anything that could be said here.
     */
    if (!clawt_computer_reconcile(self, &stale))
        g_debug("computer: state could not be reconciled: %s",
                stale != NULL ? stale->message : "no reason given");

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    if (klass->start == NULL)
        return TRUE;

    return klass->start(self, error);
}

gboolean
clawt_computer_reconcile(ClawtComputer *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    CALL_OR_TRUE(self, reconcile, error);
}

gboolean
clawt_computer_stop(ClawtComputer *self, GError **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    /*
     * Refused rather than answered TRUE, for the reason teardown is --
     * and it became worth doing the moment a person could press Stop.
     * A backend with no stop() would have reported the machine as
     * stopped and left it running, which is the same lie one function
     * down, in front of somebody watching for it to go.
     *
     * A backend with genuinely nothing to stop says so itself: the null
     * computer has no machine and the host computer's machine is the one
     * clawtilla is running on, and both are two lines.
     */
    if (klass->stop == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "a %s computer does not know how to stop itself, so "
                    "whatever it started is still running.",
                    clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE,
                                       clawt_computer_get_computer_type(self)));
        return FALSE;
    }

    return klass->stop(self, error);
}

gboolean
clawt_computer_restart(ClawtComputer *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    /*
     * Stop, then start.  Composed here rather than in each caller,
     * because a client sequencing two round trips of its own would be a
     * second answer to what a restart is -- and the two would differ on
     * the case nobody drives: what to do when the stop fails.
     *
     * Which is: give up. Every backend's stop() already answers TRUE for
     * a machine that is not running, so a failure here is a real one,
     * and starting on top of it would either fail again with a worse
     * message or leave two of something.
     */
    if (!clawt_computer_stop(self, error))
        return FALSE;

    return clawt_computer_start(self, error);
}

gboolean
clawt_computer_teardown(ClawtComputer *self, GError **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    /*
     * Refused rather than answered TRUE, unlike every other vfunc here.
     *
     * This one used to go through CALL_OR_TRUE, so a backend that had not
     * implemented it reported the computer as destroyed and destroyed
     * nothing. ClawtVmComputer was in exactly that state: removing a VM
     * agent said "removed", left the libvirt domain defined and the disk
     * on disk, and the only way to find out was to go looking in
     * virt-manager for a VM that should not have been there.
     *
     * A backend that genuinely has nothing to destroy says so with a
     * teardown of its own -- see the null and host computers, which are
     * two lines each. That way the next backend added inherits a loud
     * failure rather than a quiet lie.
     */
    if (klass->teardown == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "a %s computer does not know how to destroy itself, so "
                    "anything it created is still there. Remove it by hand.",
                    clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE,
                                       clawt_computer_get_computer_type(self)));
        return FALSE;
    }

    return klass->teardown(self, error);
}

void
clawt_computer_set_start_on_first_use(ClawtComputer *self,
                                      gboolean       first_use)
{
    ClawtComputerPrivate *priv;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));

    priv = PRIV(self);

    g_mutex_lock(&priv->start_lock);
    priv->first_use = first_use;
    g_mutex_unlock(&priv->start_lock);
}

gboolean
clawt_computer_get_start_on_first_use(ClawtComputer *self)
{
    ClawtComputerPrivate *priv;
    gboolean first_use;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    priv = PRIV(self);

    g_mutex_lock(&priv->start_lock);
    first_use = priv->first_use;
    g_mutex_unlock(&priv->start_lock);

    return first_use;
}

gboolean
clawt_computer_ensure_started(ClawtComputer *self, GError **error)
{
    ClawtComputerPrivate *priv;
    g_autoptr(GError) local = NULL;
    guint64 joined;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    priv = PRIV(self);

    g_mutex_lock(&priv->start_lock);

    /*
     * A machine that started with its agent is not this function's to
     * start. A stopped one stays stopped and its command says so -- what
     * every computer did before on_first_use existed, and what somebody
     * who pressed Stop on it is entitled to.
     */
    if (!priv->first_use) {
        g_mutex_unlock(&priv->start_lock);
        return TRUE;
    }

    if (priv->starting) {
        /*
         * The start itself asking: let it through rather than have it
         * wait for its own end.
         */
        if (priv->starter == g_thread_self()) {
            g_mutex_unlock(&priv->start_lock);
            return TRUE;
        }

        /* Another first use is starting it: share that start. */
        joined = priv->start_generation;

        while (priv->start_generation == joined)
            g_cond_wait(&priv->start_done, &priv->start_lock);

        ok = priv->start_ok;

        if (!ok && priv->start_error != NULL)
            g_propagate_error(error, g_error_copy(priv->start_error));

        g_mutex_unlock(&priv->start_lock);

        return ok;
    }

    /*
     * Read under the lock with the flag, so a wave that has just ended
     * is seen as the RUNNING it left rather than started a second time.
     */
    if (priv->state == CLAWT_COMPUTER_STATE_RUNNING) {
        g_mutex_unlock(&priv->start_lock);
        return TRUE;
    }

    priv->starting = TRUE;
    priv->starter = g_thread_self();

    g_mutex_unlock(&priv->start_lock);

    ok = clawt_computer_start(self, &local);

    if (!ok && local == NULL)
        g_set_error_literal(&local, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                            "it did not start, and gave no reason");

    g_mutex_lock(&priv->start_lock);

    priv->start_ok = ok;
    g_clear_error(&priv->start_error);

    if (local != NULL)
        priv->start_error = g_error_copy(local);

    priv->starting = FALSE;
    priv->starter = NULL;
    priv->start_generation++;
    g_cond_broadcast(&priv->start_done);

    g_mutex_unlock(&priv->start_lock);

    if (!ok)
        g_propagate_error(error, g_steal_pointer(&local));

    return ok;
}

ClawtExecResult *
clawt_computer_exec(ClawtComputer        *self,
                    const gchar * const  *argv,
                    const gchar          *working_dir,
                    guint                 timeout_seconds,
                    GCancellable         *cancellable,
                    GError              **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);
    g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    if (klass->exec == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this agent has no computer to run commands on");
        return NULL;
    }

    /*
     * Here, at the one door every command comes through, rather than at
     * the tool, the IPC handler and the wait runner separately -- three
     * copies of "start it first" would differ exactly once. Before the
     * command is handed its timeout, so a two-minute boot is not taken
     * out of a two-minute command.
     */
    if (!clawt_computer_ensure_started(self, error)) {
        g_prefix_error(error, "this computer starts on first use, and it "
                              "did not start for this command: ");
        return NULL;
    }

    return klass->exec(self, argv, working_dir, timeout_seconds, cancellable,
                       error);
}

/*
 * One command, on its way to a thread.
 *
 * Copied rather than borrowed: the caller's argv is on its stack in most
 * of the callers here, and the worker outlives the return.
 */
typedef struct {
    GStrv   argv;
    gchar  *working_dir;
    guint   timeout_seconds;
} ExecRequest;

static void
exec_request_free(gpointer data)
{
    ExecRequest *request = data;

    g_clear_pointer(&request->argv, g_strfreev);
    g_clear_pointer(&request->working_dir, g_free);
    g_free(request);
}

static void
exec_worker(GTask *task, gpointer source, gpointer data,
            GCancellable *cancellable)
{
    ExecRequest *request = data;
    ClawtExecResult *result;
    GError *error = NULL;

    result = clawt_computer_exec(CLAWT_COMPUTER(source),
                                 (const gchar * const *)request->argv,
                                 request->working_dir,
                                 request->timeout_seconds, cancellable,
                                 &error);

    if (result == NULL)
        g_task_return_error(task, error);
    else
        g_task_return_pointer(task, result,
                              (GDestroyNotify)clawt_exec_result_free);
}

/*
 * The machine a command was waiting on is up, or is not. Takes the
 * reference to the command's task that clawt_computer_exec_async() held
 * across the start.
 */
static void
on_exec_first_use_started(GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
    GTask *task = G_TASK(user_data);
    GError *local = NULL;

    if (!clawt_computer_lifecycle_finish(CLAWT_COMPUTER(source), result,
                                         &local)) {
        g_prefix_error(&local, "this computer starts on first use, and it "
                               "did not start for this command: ");
        g_task_return_error(task, local);
        g_object_unref(task);
        return;
    }

    g_task_run_in_thread(task, exec_worker);
    g_object_unref(task);
}

void
clawt_computer_exec_async(ClawtComputer        *self,
                          const gchar * const  *argv,
                          const gchar          *working_dir,
                          guint                 timeout_seconds,
                          GMainContext         *context,
                          GCancellable         *cancellable,
                          GAsyncReadyCallback   callback,
                          gpointer              user_data)
{
    ExecRequest *request;
    GTask *task;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));

    /*
     * Pushed around g_task_new() and nothing else: that call is the one
     * that captures the context the callback will be dispatched on, and
     * a source's own context is not thread-default inside its dispatch.
     */
    if (context != NULL)
        g_main_context_push_thread_default(context);

    task = g_task_new(self, cancellable, callback, user_data);

    if (context != NULL)
        g_main_context_pop_thread_default(context);

    g_task_set_source_tag(task, clawt_computer_exec_async);

    /*
     * Refused here rather than on the thread.  An empty argv is a caller
     * mistake, and answering it through the callback would spend a
     * thread to say so -- but it must still answer, because a caller
     * that has already deferred an IPC frame has no other way to reply.
     */
    if (argv == NULL || argv[0] == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "no command given");
        g_object_unref(task);
        return;
    }

    request = g_new0(ExecRequest, 1);
    request->argv = g_strdupv((GStrv)argv);
    request->working_dir = g_strdup(working_dir);
    request->timeout_seconds = timeout_seconds;

    g_task_set_task_data(task, request, exec_request_free);

    /*
     * A machine waiting for its first use is started through the
     * lifecycle verb first, and the command follows once it is up -- so
     * the start is the same one an operator's Start would run, shared
     * with any other first use already starting it, and the command's
     * worker is not the thread spent waiting on a boot.
     * clawt_computer_exec() checks again on the worker, which by then
     * finds the machine running and costs nothing.
     */
    if (clawt_computer_get_start_on_first_use(self) &&
        clawt_computer_get_state(self) != CLAWT_COMPUTER_STATE_RUNNING) {
        clawt_computer_lifecycle_async(self,
                                       CLAWT_COMPUTER_LIFECYCLE_FIRST_USE,
                                       context, cancellable,
                                       on_exec_first_use_started, task);
        return;
    }

    g_task_run_in_thread(task, exec_worker);
    g_object_unref(task);
}

ClawtExecResult *
clawt_computer_exec_finish(ClawtComputer  *self,
                           GAsyncResult   *result,
                           GError        **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/*
 * Starting, stopping and restarting a machine, off the main thread.
 *
 * Here rather than in the daemon for the reason exec_async is here: the
 * rule that an IPC handler must not wait on the network has now been
 * applied at four call sites in this tree, and every time it was applied
 * at the site rather than to the function, the next caller inherited the
 * bug. `clawt_computer_start()` alone can take a minute against a podman
 * socket that has gone quiet, and the daemon's autostart path already
 * had to move it to a thread -- a second copy of that, written for the
 * menu, is how the two would come to differ.
 */
static void
lifecycle_worker(GTask *task, gpointer source, gpointer data,
                 GCancellable *cancellable)
{
    ClawtComputer *self = CLAWT_COMPUTER(source);
    ClawtComputerLifecycle op = (ClawtComputerLifecycle)GPOINTER_TO_INT(data);
    GError *local = NULL;
    gboolean ok;

    (void)cancellable;

    switch (op) {
    case CLAWT_COMPUTER_LIFECYCLE_START:
        ok = clawt_computer_start(self, &local);
        break;
    case CLAWT_COMPUTER_LIFECYCLE_STOP:
        ok = clawt_computer_stop(self, &local);
        break;
    case CLAWT_COMPUTER_LIFECYCLE_FIRST_USE:
        ok = clawt_computer_ensure_started(self, &local);
        break;
    case CLAWT_COMPUTER_LIFECYCLE_RESTART:
    default:
        ok = clawt_computer_restart(self, &local);
        break;
    }

    if (ok)
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, local);
}

void
clawt_computer_lifecycle_async(ClawtComputer          *self,
                               ClawtComputerLifecycle  op,
                               GMainContext           *context,
                               GCancellable           *cancellable,
                               GAsyncReadyCallback     callback,
                               gpointer                user_data)
{
    GTask *task;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));

    /*
     * Pushed around g_task_new() and nothing else: that call captures
     * the context the callback is dispatched on, and a source's own
     * context is not thread-default inside its dispatch.
     */
    if (context != NULL)
        g_main_context_push_thread_default(context);

    task = g_task_new(self, cancellable, callback, user_data);

    if (context != NULL)
        g_main_context_pop_thread_default(context);

    g_task_set_source_tag(task, clawt_computer_lifecycle_async);
    g_task_set_task_data(task, GINT_TO_POINTER(op), NULL);
    g_task_run_in_thread(task, lifecycle_worker);
    g_object_unref(task);
}

gboolean
clawt_computer_lifecycle_finish(ClawtComputer  *self,
                                GAsyncResult   *result,
                                GError        **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean
clawt_computer_put_file(ClawtComputer  *self,
                        const gchar    *local_path,
                        const gchar    *remote_path,
                        GError        **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    if (klass->put_file == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this computer cannot receive files");
        return FALSE;
    }

    return klass->put_file(self, local_path, remote_path, error);
}

gboolean
clawt_computer_get_file(ClawtComputer  *self,
                        const gchar    *remote_path,
                        const gchar    *local_path,
                        GError        **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    if (klass->get_file == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this computer cannot provide files");
        return FALSE;
    }

    return klass->get_file(self, remote_path, local_path, error);
}

gchar *
clawt_computer_describe(ClawtComputer *self)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    return (klass->describe != NULL) ? klass->describe(self)
                                     : g_strdup("You have no computer.");
}

ClawtComputerType
clawt_computer_get_computer_type(ClawtComputer *self)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), CLAWT_COMPUTER_NONE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    return (klass->get_computer_type != NULL)
           ? klass->get_computer_type(self)
           : CLAWT_COMPUTER_NONE;
}

ClawtComputerState
clawt_computer_get_state(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), CLAWT_COMPUTER_STATE_ABSENT);

    return PRIV(self)->state;
}

gchar *
clawt_computer_shared_owner(const gchar *computer_id)
{
    g_return_val_if_fail(computer_id != NULL, NULL);

    /* A dot: no agent id may hold one, so no agent's machine can share the name. */
    return g_strconcat(CLAWT_SHARED_OWNER_PREFIX, computer_id, NULL);
}

gboolean
clawt_computer_is_shared(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    return g_str_has_prefix(PRIV(self)->agent_id, CLAWT_SHARED_OWNER_PREFIX);
}

gchar *
clawt_computer_shared_workspace_path(const gchar *agent_id)
{
    g_return_val_if_fail(agent_id != NULL, NULL);

    return g_build_filename(CLAWT_SHARED_WORKSPACES_ROOT, agent_id, NULL);
}

const gchar *
clawt_computer_get_agent_id(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    return PRIV(self)->agent_id;
}

const gchar *
clawt_computer_get_last_error(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    return PRIV(self)->last_error;
}

/*
 * Both paths for every share, host first.
 *
 * An agent's `read`, `write` and `bash` run on the *host*; only
 * clawtilla_computer_exec goes inside. So a share has two names and the
 * agent needs both -- the host one to open the file, the guest one to
 * pass as an argument to a command running in there.
 *
 * This said only the target, which is the one that is no use to the
 * tools an agent reaches for first. Two of them worked out that
 * something was shared, went looking on the host at the *guest's* path,
 * found nothing, and concluded the share did not exist. It did. The same
 * lesson the attachment path learned, arrived at from the other end.
 */
void
clawt_computer_describe_mounts(ClawtComputer *self, GString *out)
{
    GPtrArray *mounts;
    guint i;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));
    g_return_if_fail(out != NULL);

    mounts = clawt_computer_get_mounts(self);

    if (mounts == NULL || mounts->len == 0) {
        g_string_append(out,
            " No host directories are shared with it, so nothing you write "
            "in there is visible to your own read and write tools.");
        return;
    }

    g_string_append(out,
        " Shared with the host, as host path = the path inside:");

    for (i = 0; i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        g_autofree gchar *source = clawt_mount_resolved_source(mount);

        g_string_append_printf(out, "%s %s = %s (%s)",
                               i > 0 ? "," : "",
                               source != NULL ? source : "?",
                               clawt_mount_get_target(mount),
                               clawt_mount_get_mode(mount) ==
                                   CLAWT_MOUNT_MODE_RO
                               ? "read-only" : "read-write");
    }

    g_string_append(out,
        ". Your read and write tools run on the host, so use the host path "
        "with those and the inside path only as an argument to a command "
        "you run in there.");
}

void
clawt_computer_add_mount(ClawtComputer *self, ClawtMount *mount)
{
    ClawtMount *copy;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));
    g_return_if_fail(mount != NULL);

    copy = clawt_mount_copy(mount);

    /*
     * A mount the config did not type is typed by the backend it is
     * being added to -- here, because this is the one door every mount
     * comes through.
     *
     * It was done in clawt_computer_factory_create() instead, which
     * types the ones built from the agent's config and cannot see the
     * ones the daemon adds afterwards. The exchange is added there, so
     * its three mounts stayed CLAWT_MOUNT_BIND -- the default -- on a
     * VM. The domain XML does not filter on the type and emitted a
     * <filesystem> for each, so the guest had five virtiofs devices;
     * the seed's render_mounts() does filter, so the guest's fstab had
     * two entries. Every VM agent in a fleet therefore had a working
     * exchange device it never mounted, and /mnt/clawtilla/exchange was
     * simply absent -- which is the return path every qa persona tells
     * an agent to hand screenshots back through.
     *
     * Both halves were correct in isolation and had tests. What nobody
     * had was the wire between the mount list and the type.
     */
    if (clawt_mount_get_mount_type(copy) == CLAWT_MOUNT_BIND &&
        clawt_computer_get_computer_type(self) == CLAWT_COMPUTER_VM)
        clawt_mount_set_mount_type(copy, CLAWT_MOUNT_VIRTIOFS);

    g_ptr_array_add(PRIV(self)->mounts, copy);
}

GPtrArray *
clawt_computer_get_mounts(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    return PRIV(self)->mounts;
}

void
clawt_computer_set_state(ClawtComputer      *self,
                         ClawtComputerState  state,
                         const gchar        *detail)
{
    ClawtComputerPrivate *priv;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));

    priv = PRIV(self);

    if (detail != NULL) {
        g_free(priv->last_error);
        priv->last_error = g_strdup(detail);
    }

    if (priv->state == state)
        return;

    priv->state = state;
    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state, detail);
}

void
clawt_computer_bind_agent(ClawtComputer *self, const gchar *agent_id)
{
    ClawtComputerPrivate *priv;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));

    priv = PRIV(self);
    g_free(priv->agent_id);
    priv->agent_id = g_strdup(agent_id);
}

gchar *
clawt_computer_truncate_output(const gchar *text,
                               gsize        limit,
                               gboolean    *out_truncated)
{
    gsize length;

    if (out_truncated != NULL)
        *out_truncated = FALSE;

    if (text == NULL)
        return g_strdup("");

    length = strlen(text);

    if (limit == 0 || length <= limit)
        return g_strdup(text);

    if (out_truncated != NULL)
        *out_truncated = TRUE;

    /*
     * The marker matters as much as the cut.  An agent handed a silently
     * shortened directory listing treats it as the whole thing and reaches
     * a confident wrong conclusion; one told it was cut short asks a
     * narrower question instead.
     */
    return g_strdup_printf(
        "%.*s\n\n[... output truncated at %" G_GSIZE_FORMAT " bytes; "
        "%" G_GSIZE_FORMAT " bytes were produced. Narrow the command to see "
        "the rest.]",
        (int)limit, text, limit, length);
}

static void
clawt_computer_finalize(GObject *object)
{
    ClawtComputerPrivate *priv = PRIV(object);

    g_clear_pointer(&priv->agent_id, g_free);
    g_clear_pointer(&priv->last_error, g_free);
    g_clear_pointer(&priv->mounts, g_ptr_array_unref);
    g_clear_error(&priv->start_error);
    g_mutex_clear(&priv->start_lock);
    g_cond_clear(&priv->start_done);

    G_OBJECT_CLASS(clawt_computer_parent_class)->finalize(object);
}

static void
clawt_computer_class_init(ClawtComputerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_computer_finalize;

    /**
     * ClawtComputer::state-changed:
     * @self: the computer
     * @state: the new state
     * @detail: (nullable): what happened
     */
    signals[SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed", CLAWT_TYPE_COMPUTER, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_INT, G_TYPE_STRING);
}

static void
clawt_computer_init(ClawtComputer *self)
{
    ClawtComputerPrivate *priv = PRIV(self);

    priv->mounts = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_mount_free);
    priv->state = CLAWT_COMPUTER_STATE_ABSENT;
    g_mutex_init(&priv->start_lock);
    g_cond_init(&priv->start_done);
}
