/*
 * clawt-computer.h - What an agent can run commands on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Four backends behind one interface: nothing at all, the real host, a
 * container, or a virtual machine.  Desktop control is not a backend -- it
 * is an add-on that works alongside any of them.
 *
 * The interface is deliberately small.  Everything an agent does with a
 * computer is "run this and tell me what happened", "put this file there",
 * "fetch that one" -- and the backends differ enormously in how they do
 * that while agreeing completely on what it means.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "computer/clawt-exec-result.h"
#include "computer/clawt-mount.h"

G_BEGIN_DECLS

/**
 * CLAWT_WORKSPACE_MOUNT_POINT:
 *
 * Where an agent's own workspace appears inside its computer.
 *
 * Beside the exchange rather than anywhere more natural, so everything
 * clawtilla puts in a computer is under one directory an agent can be
 * told about once.
 */
#define CLAWT_WORKSPACE_MOUNT_POINT "/mnt/clawtilla/workspace"

#define CLAWT_TYPE_COMPUTER (clawt_computer_get_type())

G_DECLARE_DERIVABLE_TYPE(ClawtComputer, clawt_computer,
                         CLAWT, COMPUTER, GObject)

/**
 * ClawtComputerClass:
 * @provision: create it if it does not exist
 * @start: make it ready for commands
 * @stop: take it down
 * @teardown: destroy it entirely
 * @exec: run a command and wait for it
 * @put_file: copy a file in
 * @get_file: copy a file out
 * @describe: a sentence for the agent's prompt
 * @get_computer_type: which backend this is
 *
 * The vtable a backend implements.
 */
struct _ClawtComputerClass {
    GObjectClass parent_class;

    gboolean          (*provision)  (ClawtComputer        *self,
                                     GError              **error);
    gboolean          (*start)      (ClawtComputer        *self,
                                     GError              **error);
    gboolean          (*stop)       (ClawtComputer        *self,
                                     GError              **error);
    gboolean          (*teardown)   (ClawtComputer        *self,
                                     GError              **error);

    ClawtExecResult * (*exec)       (ClawtComputer        *self,
                                     const gchar * const  *argv,
                                     const gchar          *working_dir,
                                     guint                 timeout_seconds,
                                     GCancellable         *cancellable,
                                     GError              **error);

    gboolean          (*put_file)   (ClawtComputer        *self,
                                     const gchar          *local_path,
                                     const gchar          *remote_path,
                                     GError              **error);
    gboolean          (*get_file)   (ClawtComputer        *self,
                                     const gchar          *remote_path,
                                     const gchar          *local_path,
                                     GError              **error);

    gchar *           (*describe)   (ClawtComputer        *self);

    ClawtComputerType (*get_computer_type)(ClawtComputer   *self);

    gboolean          (*reconcile)  (ClawtComputer        *self,
                                     GError              **error);

    /*< private >*/
    gpointer _padding[7];
};

/**
 * clawt_computer_provision:
 * @self: a #ClawtComputer
 * @error: (out) (optional): return location for a #GError
 *
 * The lifecycle, in order: provision creates whatever does not exist
 * yet, start makes it usable, stop makes it unusable but keeps it, and
 * teardown destroys it.
 *
 * start() provisions first if it has to, so a caller that only ever
 * calls start() is correct.  teardown() is never called automatically:
 * destroying an agent's container because the daemon restarted would
 * lose whatever was in it.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_computer_provision(ClawtComputer *self, GError **error);
gboolean clawt_computer_start(ClawtComputer *self, GError **error);

/**
 * clawt_computer_reconcile:
 * @self: a #ClawtComputer
 * @error: (out) (optional): return location for a #GError
 *
 * Asks the backend what it actually has, and makes the remembered state
 * agree with the answer.
 *
 * The state was only ever written, never read back: whatever the last
 * operation set is what every surface reported until the daemon exited.
 * Remove an agent's container behind the daemon's back and it stayed
 * `running` for the life of the process, with the one honest surface
 * being an exec that failed by naming a container id nobody could
 * correlate with anything.
 *
 * A backend with nothing to ask answers %TRUE and changes nothing, so a
 * caller never has to know which kind of computer it holds.
 *
 * Called by clawt_computer_start(), which is the one path that has to
 * talk to the backend anyway, so the round trip costs nothing new there.
 *
 * Deliberately *not* called by `computer status`, which is a memory read
 * today: making it a round trip would put a blocking network call back
 * on the daemon's main context and, against a wedged backend, an
 * unbounded one. The consequence is worth stating plainly -- a status
 * read reports the remembered state until something starts the agent,
 * and it is the start that corrects it.
 *
 * Returns: %FALSE only when the backend could not be asked
 */
gboolean clawt_computer_reconcile(ClawtComputer *self, GError **error);

/**
 * clawt_computer_stop:
 * @self: a #ClawtComputer
 * @error: return location for a #GError
 *
 * Stops the machine, leaving it there to be started again.
 *
 * A backend that has not implemented one is **refused**, not answered
 * %TRUE -- the same rule as clawt_computer_teardown(), and for the same
 * reason: a Stop that reports success and leaves the machine running is
 * worse than one that is missing, because the person watching for it to
 * go has been told it went.
 *
 * Returns: %TRUE if the machine is stopped
 */
gboolean clawt_computer_stop(ClawtComputer *self, GError **error);
gboolean clawt_computer_teardown(ClawtComputer *self, GError **error);

/**
 * clawt_computer_restart:
 * @self: a #ClawtComputer
 * @error: return location for a #GError
 *
 * Stops the machine and starts it again.
 *
 * Composed here rather than in each caller: two clients sequencing a
 * stop and a start of their own would be two answers to what a restart
 * is, and they would differ on the case nobody drives -- what to do when
 * the stop fails. Here that is: give up, because every backend's stop
 * already answers %TRUE for a machine that is not running, so a failure
 * is a real one.
 *
 * Returns: %TRUE if the machine is running again
 */
gboolean clawt_computer_restart(ClawtComputer *self, GError **error);

/**
 * ClawtComputerLifecycle:
 * @CLAWT_COMPUTER_LIFECYCLE_START: bring the machine up
 * @CLAWT_COMPUTER_LIFECYCLE_STOP: take it down, leaving it there
 * @CLAWT_COMPUTER_LIFECYCLE_RESTART: both, in that order
 *
 * Which of the three clawt_computer_lifecycle_async() should run.
 */
typedef enum {
    CLAWT_COMPUTER_LIFECYCLE_START = 0,
    CLAWT_COMPUTER_LIFECYCLE_STOP,
    CLAWT_COMPUTER_LIFECYCLE_RESTART
} ClawtComputerLifecycle;

/**
 * clawt_computer_lifecycle_async:
 * @self: a #ClawtComputer
 * @op: which of the three to run
 * @context: (nullable): the context to answer on, or %NULL for the
 *   thread-default at the time of the call
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when it is done
 * @user_data: data for @callback
 *
 * Runs a lifecycle verb on a worker thread.
 *
 * The wait lives here rather than in each caller, for the reason
 * clawt_computer_exec_async() does. Starting a container is a blocking
 * request to podman that can take a minute against a socket that has
 * gone quiet, and an IPC handler that waits on it stalls every agent's
 * messages, every task delivery and every timer with it -- a rule this
 * tree has now had to apply at four separate call sites, each time
 * because it had been applied to a call site rather than to the
 * function.
 *
 * @context must be named. g_task_new() captures whatever is
 * thread-default on the calling thread, and dispatching a source does
 * not make that source's context thread-default -- so a task created
 * inside a handler completes on a loop the daemon never runs.
 */
void clawt_computer_lifecycle_async(ClawtComputer          *self,
                                    ClawtComputerLifecycle  op,
                                    GMainContext           *context,
                                    GCancellable           *cancellable,
                                    GAsyncReadyCallback     callback,
                                    gpointer                user_data);

/**
 * clawt_computer_lifecycle_finish:
 * @self: a #ClawtComputer
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: %TRUE if the verb succeeded
 */
gboolean clawt_computer_lifecycle_finish(ClawtComputer  *self,
                                         GAsyncResult   *result,
                                         GError        **error);

/**
 * clawt_computer_exec:
 * @self: a #ClawtComputer
 * @argv: (array zero-terminated=1): the command
 * @working_dir: (nullable): where to run it
 * @timeout_seconds: give up after this long, or 0 for no limit
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Runs a command and waits for it.
 *
 * A timeout is strongly advised: an agent that runs an interactive command
 * by mistake would otherwise wait for input that never comes, and the turn
 * never ends.
 *
 * Returns: (transfer full) (nullable): what happened, or %NULL if it could
 *   not be run at all
 */
ClawtExecResult *clawt_computer_exec(ClawtComputer        *self,
                                     const gchar * const  *argv,
                                     const gchar          *working_dir,
                                     guint                 timeout_seconds,
                                     GCancellable         *cancellable,
                                     GError              **error);

/**
 * clawt_computer_exec_async:
 * @self: a #ClawtComputer
 * @argv: (array zero-terminated=1): the command
 * @working_dir: (nullable): where to run it
 * @timeout_seconds: give up after this long, or 0 for no limit
 * @context: (nullable): the main context to answer on
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the command has finished
 * @user_data: for @callback
 *
 * The same command, run on a worker thread.
 *
 * clawt_computer_exec() holds whichever thread it is called on for the
 * length of the command, which is right for a caller with somebody
 * waiting on the answer and wrong for anything running a main loop.
 * Both of the places that had to wait -- the agent's tool call and the
 * operator's `computer exec` -- ran on the daemon's main context, so one
 * command blocked every other agent's messages, task delivery and timer
 * for as long as it took, up to the advertised 120 second default. The
 * operator's is the worse of the two: a person at a terminal is the
 * caller most likely to run something long on purpose, and a fleet that
 * appears to hang while a command they can see running is still going
 * reads as the fleet being broken.
 *
 * @context is named rather than taken from the thread-default, because
 * g_task_new() captures whatever is thread-default on the calling
 * thread and dispatching a source pushes nothing -- so a caller reached
 * from an idle of its own would have its answer delivered on a loop
 * nobody runs. That trap has now appeared behind four APIs in this tree.
 * %NULL means the thread-default, which is correct only for a caller
 * that knows it is on the loop it wants.
 *
 * The backend is held for the length of the call, so an agent stopped
 * mid-command does not leave the worker holding a computer the manager
 * has dropped.
 */
void clawt_computer_exec_async(ClawtComputer        *self,
                               const gchar * const  *argv,
                               const gchar          *working_dir,
                               guint                 timeout_seconds,
                               GMainContext         *context,
                               GCancellable         *cancellable,
                               GAsyncReadyCallback   callback,
                               gpointer              user_data);

/**
 * clawt_computer_exec_finish:
 * @self: a #ClawtComputer
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): what happened, or %NULL if it
 *   could not be run at all
 */
ClawtExecResult *clawt_computer_exec_finish(ClawtComputer  *self,
                                            GAsyncResult   *result,
                                            GError        **error);

gboolean clawt_computer_put_file(ClawtComputer  *self,
                                 const gchar    *local_path,
                                 const gchar    *remote_path,
                                 GError        **error);
gboolean clawt_computer_get_file(ClawtComputer  *self,
                                 const gchar    *remote_path,
                                 const gchar    *local_path,
                                 GError        **error);

/**
 * clawt_computer_describe:
 * @self: a #ClawtComputer
 *
 * A description for the agent's prompt: what it has and what it cannot
 * reach, so it does not discover the limits by trial.
 *
 * Returns: (transfer full): the description
 */
gchar *clawt_computer_describe(ClawtComputer *self);

ClawtComputerType  clawt_computer_get_computer_type(ClawtComputer *self);
ClawtComputerState clawt_computer_get_state(ClawtComputer *self);

/**
 * clawt_computer_get_agent_id:
 * @self: a #ClawtComputer
 *
 * Returns: (transfer none): the agent this belongs to
 */
const gchar *clawt_computer_get_agent_id(ClawtComputer *self);

/**
 * clawt_computer_add_mount:
 * @self: a #ClawtComputer
 * @mount: (transfer none): a host path to share
 *
 * Adds a mount.  Applied when the computer is provisioned, so calling this
 * afterwards has no effect until it is provisioned again.
 */
/**
 * clawt_computer_describe_mounts:
 * @self: a #ClawtComputer
 * @out: the description being built
 *
 * Appends every share as `host path = path inside`, and says which of
 * the two an agent's own tools can open.  Shared by the backends so the
 * wording cannot drift between them.
 */
void clawt_computer_describe_mounts(ClawtComputer *self, GString *out);

void clawt_computer_add_mount(ClawtComputer *self, ClawtMount *mount);

/**
 * clawt_computer_get_mounts:
 * @self: a #ClawtComputer
 *
 * Returns: (transfer none) (element-type ClawtMount): the mounts
 */
GPtrArray *clawt_computer_get_mounts(ClawtComputer *self);

/**
 * clawt_computer_get_last_error:
 * @self: a #ClawtComputer
 *
 * Returns: (transfer none) (nullable): why it last failed
 */
const gchar *clawt_computer_get_last_error(ClawtComputer *self);

/*< protected >*/

void clawt_computer_set_state(ClawtComputer      *self,
                              ClawtComputerState  state,
                              const gchar        *detail);

void clawt_computer_bind_agent(ClawtComputer *self,
                               const gchar   *agent_id);

/**
 * clawt_computer_truncate_output:
 * @text: (nullable): output to bound
 * @limit: maximum bytes
 * @out_truncated: (out) (optional): whether anything was removed
 *
 * Bounds command output.
 *
 * Unbounded output is a real failure mode: an agent that runs `find /` gets
 * a reply too large to send and too large to reason about, and the turn is
 * wasted either way.  Truncation is reported rather than hidden, because an
 * agent treating a cut-short listing as complete reaches confident wrong
 * conclusions.
 *
 * Returns: (transfer full): the bounded text
 */
gchar *clawt_computer_truncate_output(const gchar *text,
                                      gsize        limit,
                                      gboolean    *out_truncated);

G_END_DECLS
