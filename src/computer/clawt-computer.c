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
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    CALL_OR_TRUE(self, stop, error);
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
    g_return_if_fail(CLAWT_IS_COMPUTER(self));
    g_return_if_fail(mount != NULL);

    g_ptr_array_add(PRIV(self)->mounts, clawt_mount_copy(mount));
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
}
