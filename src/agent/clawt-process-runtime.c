/*
 * clawt-process-runtime.c - Supervising a libreclaw child process
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <signal.h>
#include <sys/prctl.h>
#include "agent/clawt-process-runtime.h"

#include <string.h>

struct _ClawtProcessRuntime {
    ClawtAgentRuntime parent_instance;

    gchar        *binary;
    gchar        *config_path;
    GHashTable   *environment;

    GSubprocess      *process;
    GCancellable     *cancellable;
    GDataInputStream *log_stream;
    gboolean          running;

    /*
     * Set only when the child has genuinely gone, which is not the same
     * as `running` -- that one was cleared the instant a SIGTERM was
     * *sent*, so a stop reported success while the process was still
     * there and the next start spawned a second one alongside it.
     */
    gboolean          exited;

    /*
     * The context the child's exit will arrive on.
     *
     * g_subprocess_wait_async() completes on whatever was thread-default
     * when it was called, and stop() has to iterate *that* one -- it used
     * to ask for the thread-default again from a different call stack and
     * get a different answer.  On clawtillad both are the process default
     * and it worked by luck; under an embedded daemon, or from a GTask
     * callback (which pushes its own context), the exit arrived somewhere
     * stop() was not looking, so every clean shutdown waited out the full
     * grace period and then SIGKILLed a child that had already gone.
     */
    GMainContext     *context;

    /*
     * Exits seen for a child this runtime had already let go of.  See
     * on_process_exited(); it is a diagnostic, not a control.
     */
    guint superseded_exits;
};

/*
 * How long a child gets to act on SIGTERM before it is killed, as a
 * number of short polls -- 5 seconds, in 50ms steps.  Long enough for
 * libreclaw to close its channels, short enough that stopping an agent is
 * not something you wait on.
 */
#define STOP_TICK_USEC   (50 * 1000)
#define STOP_GRACE_TICKS (100)

/*
 * How long to wait for a SIGKILLed child to be reaped.  Short, because
 * SIGKILL cannot be caught: anything that outlasts this is in
 * uninterruptible sleep and is not going to be helped by waiting longer.
 */
#define KILL_REAP_TICKS  (40)

G_DEFINE_FINAL_TYPE(ClawtProcessRuntime, clawt_process_runtime,
                    CLAWT_TYPE_AGENT_RUNTIME)

ClawtProcessRuntime *
clawt_process_runtime_new(ClawtAgentConfig *config, const gchar *config_path)
{
    ClawtProcessRuntime *self;

    g_return_val_if_fail(config != NULL, NULL);
    g_return_val_if_fail(config_path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_PROCESS_RUNTIME, NULL);
    self->config_path = g_strdup(config_path);

    clawt_agent_runtime_bind_config(CLAWT_AGENT_RUNTIME(self), config);

    return self;
}

void
clawt_process_runtime_set_binary(ClawtProcessRuntime *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_PROCESS_RUNTIME(self));

    g_free(self->binary);
    self->binary = g_strdup(path);
}

void
clawt_process_runtime_set_environment(ClawtProcessRuntime *self,
                                      GHashTable          *env)
{
    g_return_if_fail(CLAWT_IS_PROCESS_RUNTIME(self));

    g_clear_pointer(&self->environment, g_hash_table_unref);

    if (env != NULL)
        self->environment = g_hash_table_ref(env);
}

/* ── Reading the child's output ──────────────────────────────────── */

static void read_next_line(ClawtProcessRuntime *self,
                           GDataInputStream    *stream);

static void
on_log_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(ClawtProcessRuntime) self = user_data;
    g_autofree gchar *line = NULL;
    g_autoptr(GError) error = NULL;
    gsize length = 0;

    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                                result, &length, &error);

    if (line == NULL)
        return;

    clawt_agent_runtime_record_log_line(CLAWT_AGENT_RUNTIME(self), line);
    read_next_line(self, G_DATA_INPUT_STREAM(source));
}

static void
read_next_line(ClawtProcessRuntime *self, GDataInputStream *stream)
{
    /*
     * Deliberately not gated on `running`.  A short-lived agent exits
     * before its output has been drained, and stopping there loses exactly
     * the last thing it said -- which, when it died complaining about its
     * config, is the only line anybody wanted.  Reading continues until EOF
     * or cancellation.
     */
    if (stream == NULL || g_cancellable_is_cancelled(self->cancellable))
        return;

    /*
     * The read holds a reference, so a runtime released while its child is
     * still writing is not finalized under the callback.
     */
    g_data_input_stream_read_line_async(stream, G_PRIORITY_LOW,
                                        self->cancellable, on_log_line,
                                        g_object_ref(self));
}

static void
on_process_exited(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(ClawtProcessRuntime) self = user_data;
    g_autoptr(GError) error = NULL;
    GSubprocess *process = G_SUBPROCESS(source);
    g_autofree gchar *detail = NULL;
    gboolean clean;

    /*
     * Finished first, and for whichever child this turns out to be: an
     * async operation is completed by its own callback, not only on the
     * paths that turn out to be interesting.
     *
     * It reads like zombie avoidance and is not.  A GSubprocess reaps
     * through its own child watch whether or not anything ever waits on
     * it -- checked by letting one exit with no wait outstanding at all
     * and finding no entry left in /proc -- so the early return below
     * strands no process.  Ordering this first is about the operation,
     * not about the child, and a later edit that moves it on the belief
     * that a zombie depends on it will be reasoning from the wrong fact.
     */
    g_subprocess_wait_finish(process, result, &error);

    /*
     * And it is only *our* exit if it is the child we are still holding.
     *
     * One runtime object serves an agent for its whole life and replaces
     * its child underneath itself: process_runtime_start() clears
     * `process` and spawns another, and dispose() drops it while the
     * wait started for it is still outstanding.  So this callback can
     * arrive for a child that stopped being the current one some time
     * ago -- and it took `source` on trust, set `running` and `exited`
     * from it, and reported the exit as though the live child had died.
     *
     * clawt_agent's handler for that signal clears the link and moves
     * the agent to STOPPED.  The result was an agent that was running,
     * connected and idle while every surface said "stopped - exited with
     * status 0": it answered nothing, and work delegated to it failed
     * with "the agent handling this stopped before finishing" while ps
     * showed its process perfectly healthy.
     *
     * The way there in production is a child that will not die promptly.
     * process_runtime_stop() waits KILL_REAP_TICKS for the kill to be
     * observed, warns that the child is "probably in uninterruptible
     * sleep", and returns with `running` cleared -- leaving this wait
     * outstanding while the next start() installs a replacement.  When
     * the kernel finally lets the old one go, this runs.
     *
     * Counted rather than only dropped, because an exit arriving for a
     * child nobody is waiting on is worth being able to see: it is the
     * signature of the stop path giving up, and there was no way to
     * observe it from outside while it was corrupting an agent's state.
     */
    if (process != self->process) {
        self->superseded_exits++;

        g_debug("agent runtime: ignoring the exit of a child this runtime "
                "no longer holds (%u so far)", self->superseded_exits);

        return;
    }

    self->running = FALSE;
    self->exited = TRUE;

    clean = g_subprocess_get_successful(process);

    if (g_subprocess_get_if_signaled(process))
        detail = g_strdup_printf("killed by signal %d",
                                 g_subprocess_get_term_sig(process));
    else
        detail = g_strdup_printf("exited with status %d",
                                 g_subprocess_get_exit_status(process));

    clawt_agent_runtime_record_exit(CLAWT_AGENT_RUNTIME(self), clean, detail);
}

/*
 * Asks the kernel to signal the child if this process disappears.
 *
 * Runs in the child between fork and exec, so it must be
 * async-signal-safe -- prctl is.
 *
 * A daemon stopped cleanly stops its agents itself, but one that is
 * killed outright runs no handler at all, and its agents were then
 * reparented to init and left holding the ports, the session directory
 * and the sqlite database of an agent nothing was supervising. The next
 * daemon started a second copy alongside each of them, which is the
 * multi-instance collision libreclaw explicitly cannot survive. Three
 * such orphans accumulated in one afternoon of restarts.
 */
static void
die_with_parent(gpointer user_data)
{
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

/*
 * Gives the child its environment: the shared allowlist plus whatever the
 * agent's own `env:` block names.
 */
static void
apply_environment(ClawtProcessRuntime  *self,
                  GSubprocessLauncher  *launcher)
{
    g_auto(GStrv) environment = clawt_build_child_environment(self->environment);

    g_subprocess_launcher_set_environ(launcher, environment);
}

/*
 * Where clawtillad is, so the things built beside it can be found.
 *
 * NULL when /proc is not available, which only costs those entries.
 */
static gchar *
executable_dir(void)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe == NULL)
        return NULL;

    return g_path_get_dirname(exe);
}

/*
 * The libreclaw to run, best first.
 *
 * PATH used to be the only answer, which meant a fresh clone failed at
 * the first agent start with "not on PATH" -- while the binary sat in
 * deps/libreclaw/build/release, built minutes earlier by the same `make`
 * that produced the daemon doing the complaining. Anybody who had ever
 * installed libreclaw, or set defaults.libreclaw_binary once and
 * forgotten, never saw it.
 *
 * The same fix pod modules already have, and for the same reason: a
 * normal build and a normal install should both work without being
 * configured. `deps/libreclaw/build/release` is where clawtilla's own
 * config.mk points LIBRECLAW_OUTDIR, whatever build type the daemon
 * itself was built as.
 */
static gchar *
resolve_libreclaw(void)
{
    g_autofree gchar *exe_dir = executable_dir();

    if (exe_dir != NULL) {
        /* Installed: /usr/local/bin/clawtillad beside libreclaw. */
        g_autofree gchar *beside = g_build_filename(exe_dir, "libreclaw",
                                                    NULL);

        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);

        /* A checkout: build/<type>/clawtillad, two levels under the root. */
        {
            g_autofree gchar *in_tree = g_build_filename(
                exe_dir, "..", "..", "deps", "libreclaw", "build", "release",
                "libreclaw", NULL);

            if (g_file_test(in_tree, G_FILE_TEST_IS_EXECUTABLE))
                return g_canonicalize_filename(in_tree, NULL);
        }
    }

    return g_find_program_in_path("libreclaw");
}

static gboolean
process_runtime_start(ClawtAgentRuntime *runtime, GError **error)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autofree gchar *binary = NULL;
    const gchar *argv[4];

    if (self->running) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "the agent is already running");
        return FALSE;
    }

    binary = (self->binary != NULL)
             ? g_strdup(self->binary)
             : resolve_libreclaw();

    if (binary == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                            "cannot find the libreclaw binary. It was not "
                            "beside clawtillad, not in deps/libreclaw's "
                            "build, and not on PATH. Build it with `make -C "
                            "deps/libreclaw`, install it, or set "
                            "defaults.libreclaw_binary to where it is.");
        return FALSE;
    }

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE);

    /*
     * The daemon's own environment is discarded rather than extended.
     * g_subprocess_launcher_set_environ with an empty list gives a genuinely
     * clean slate; setenv then adds back exactly what belongs.
     */
    {
        const gchar *empty[] = { NULL };

        g_subprocess_launcher_set_environ(launcher, (gchar **)empty);
    }

    apply_environment(self, launcher);
    g_subprocess_launcher_set_child_setup(launcher, die_with_parent, NULL,
                                          NULL);

    argv[0] = binary;
    argv[1] = "-c";
    argv[2] = self->config_path;
    argv[3] = NULL;

    /*
     * The previous one is released first.  Every restart overwrote this
     * field while it still held the last run's GSubprocess, so a
     * long-lived agent that occasionally crashes leaked one per cycle.
     */
    g_clear_object(&self->process);
    self->exited = FALSE;
    self->process = g_subprocess_launcher_spawnv(launcher, argv, error);
    if (self->process == NULL) {
        g_prefix_error(error, "starting %s: ", binary);
        return FALSE;
    }

    self->running = TRUE;
    g_clear_object(&self->cancellable);
    self->cancellable = g_cancellable_new();

    {
        GInputStream *stdout_pipe =
            g_subprocess_get_stdout_pipe(self->process);

        g_clear_object(&self->log_stream);

        if (stdout_pipe != NULL) {
            /*
             * Held on the runtime rather than scoped to this function: the
             * reads outlive the call, and a stream freed when the block ends
             * takes the child's remaining output with it.
             */
            self->log_stream = g_data_input_stream_new(stdout_pipe);
            g_data_input_stream_set_newline_type(
                self->log_stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);
            read_next_line(self, self->log_stream);
        }
    }

    g_clear_pointer(&self->context, g_main_context_unref);
    self->context = g_main_context_ref_thread_default();

    g_subprocess_wait_async(self->process, NULL, on_process_exited,
                            g_object_ref(self));

    return TRUE;
}

guint
clawt_process_runtime_get_superseded_exits(ClawtProcessRuntime *self)
{
    g_return_val_if_fail(CLAWT_IS_PROCESS_RUNTIME(self), 0);

    return self->superseded_exits;
}

static void
process_runtime_stop(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);

    /*
     * The reads are NOT cancelled here.  Whatever the child says on its way
     * out -- the reason it is refusing to run, usually -- is the part worth
     * keeping, and cancelling now discards it.  They end on their own at
     * EOF when the process goes.
     */
    if (self->process == NULL)
        return;

    /*
     * SIGTERM, not SIGKILL.  libreclaw closes its channels and says goodbye
     * on SIGTERM, which lets Matrix and the clawtilla daemon see a clean
     * shutdown rather than a dropped connection to time out.
     */
    g_subprocess_send_signal(self->process, SIGTERM);

    /*
     * ...and then wait for it, which this did not do.
     *
     * `running` used to be cleared here, the instant the signal was
     * *sent*. So a restart -- which is a stop immediately followed by a
     * start -- found the runtime claiming to be stopped while the child
     * was still shutting down, and spawned a second libreclaw against the
     * same config: same ports, same session directory, same database.
     * The new one exited straight away and the daemon reported the agent
     * "stopped - exited with status 0" while the original went on running,
     * now tracked by nothing.
     *
     * The context is iterated rather than slept through, because the exit
     * arrives on it -- a plain sleep would wait the full grace period
     * every time and then kill a child that had gone in milliseconds.
     */
    {
        GMainContext *context = self->context;
        guint waited;

        if (context == NULL)
            context = g_main_context_default();

        for (waited = 0; waited < STOP_GRACE_TICKS && !self->exited;
             waited++) {
            g_main_context_iteration(context, FALSE);
            g_usleep(STOP_TICK_USEC);
        }
    }

    /*
     * A child that will not go is killed rather than left.  Leaving it is
     * how the orphans happened, and an agent that ignores SIGTERM is
     * exactly the one that most needs stopping.
     */
    if (!self->exited) {
        GMainContext *context = self->context;
        guint waited;

        g_warning("agent runtime: pid %d did not stop within %d seconds of "
                  "SIGTERM; killing it",
                  (gint)clawt_agent_runtime_get_pid(runtime),
                  STOP_GRACE_TICKS * STOP_TICK_USEC / 1000000);
        g_subprocess_force_exit(self->process);

        if (context == NULL)
            context = g_main_context_default();

        /*
         * And then wait for the kill to be *observed*, not merely sent.
         *
         * SIGKILL ends the process, but until someone reaps it the pid is
         * still there -- kill(pid, 0) succeeds on a zombie -- and
         * `exited` is only set by the wait callback, which runs on this
         * context.  Returning before that is the same shape of lie the
         * SIGTERM path above was fixed for: stop() reporting a child gone
         * while the kernel still has an entry for it.
         *
         * Bounded anyway.  SIGKILL cannot be caught, so this ends almost
         * at once; a child stuck in uninterruptible sleep is the kernel's
         * problem and must not become a hung daemon.
         */
        for (waited = 0; waited < KILL_REAP_TICKS && !self->exited; waited++) {
            g_main_context_iteration(context, FALSE);
            g_usleep(STOP_TICK_USEC);
        }

        if (!self->exited)
            g_warning("agent runtime: pid %d has not been reaped after "
                      "SIGKILL; it is probably in uninterruptible sleep",
                      (gint)clawt_agent_runtime_get_pid(runtime));
    }

    self->running = FALSE;
}

static gboolean
process_runtime_is_alive(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);

    /*
     * Asked of our own record rather than of GSubprocess.
     * g_subprocess_get_if_exited() may only be called once the wait has
     * returned, so asking it about a child that is still running is not
     * merely wrong, it is undefined.
     */
    return self->running && self->process != NULL && !self->exited;
}

static GPid
process_runtime_get_pid(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);
    const gchar *identifier;

    if (self->process == NULL)
        return 0;

    identifier = g_subprocess_get_identifier(self->process);

    return (identifier != NULL) ? (GPid)g_ascii_strtoll(identifier, NULL, 10)
                                : 0;
}

/*
 * Kills what the agent's libreclaw spawned, and nothing above it.
 *
 * The agent's turn is carried out by an AI CLI that libreclaw spawned,
 * and that CLI spawns whatever the model asked for. All of it is below
 * our own child in the process tree, and none of it is our child -- so
 * this walks /proc rather than using GSubprocess, which only knows about
 * the one process it started.
 *
 * Killing the CLI does not cancel libreclaw's wait on it: GLib's
 * g_subprocess_communicate_*_async() abandons the read when its
 * cancellable fires but leaves the process running, which is exactly why
 * `!stop` was never enough. Coming at it from the process side is the
 * half that actually ends the work; libreclaw then sees its child exit
 * and finishes the turn on its own.
 *
 * SIGTERM first and SIGKILL after a grace period, both re-checking that
 * the pid still descends from our child. A pid read a moment ago may
 * have exited and been reused by then, and what inherits a recycled pid
 * belongs to somebody else -- on a busy machine that is the difference
 * between stopping an agent and killing the operator's editor.
 */
static gboolean
process_runtime_interrupt(ClawtAgentRuntime *runtime, guint *out_killed,
                          GError **error)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);
    g_autoptr(GArray) descendants = NULL;
    GPid root;
    guint signalled = 0;
    guint i;

    root = process_runtime_get_pid(runtime);

    if (root <= 0 || !self->running) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "the agent is not running, so it has nothing "
                            "in flight to stop");
        return FALSE;
    }

    descendants = clawt_process_descendants(root);

    /*
     * Nothing to kill is a success, not a failure.
     *
     * An agent between turns has no CLI running, and pressing stop then
     * is a person confirming what they wanted rather than a mistake to
     * report. The count says what happened, so a caller that cares can
     * tell the two apart.
     */
    for (i = 0; i < descendants->len; i++) {
        GPid pid = g_array_index(descendants, GPid, i);

        if (!clawt_process_is_descendant_of(pid, root))
            continue;

        if (kill(pid, SIGTERM) == 0)
            signalled++;
    }

    if (out_killed != NULL)
        *out_killed = signalled;

    if (signalled > 0) {
        /*
         * A short grace period, then SIGKILL whatever ignored the first
         * signal. Blocking here at all is a deliberate exception: this
         * is the one IPC handler whose whole promise is that the work
         * has stopped by the time it answers, and 200ms is under the
         * threshold at which a button feels unresponsive. An AI CLI that
         * needs longer than that to die is one that is not going to.
         */
        g_usleep(200 * 1000);

        for (i = 0; i < descendants->len; i++) {
            GPid pid = g_array_index(descendants, GPid, i);

            if (clawt_process_is_descendant_of(pid, root))
                kill(pid, SIGKILL);
        }
    }

    g_info("runtime: interrupted %s, signalling %u process(es) below pid %d",
           clawt_agent_runtime_get_agent_id(runtime) != NULL
               ? clawt_agent_runtime_get_agent_id(runtime) : "an agent",
           signalled, (gint)root);

    return TRUE;
}

/*
 * What a separate process adds over and above what every agent has: a turn
 * that can be interrupted, because there is a process to signal.
 *
 * Tools and peer messaging are not listed here -- they come from the link
 * the daemon serves, which does not depend on the runtime.
 */
static ClawtAgentCaps
process_runtime_get_caps(ClawtAgentRuntime *runtime)
{
    (void)runtime;

    return CLAWT_AGENT_CAPS_INTERRUPT | CLAWT_AGENT_CAPS_STREAMING;
}

static void
clawt_process_runtime_dispose(GObject *object)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(object);

    self->running = FALSE;

    if (self->cancellable != NULL)
        g_cancellable_cancel(self->cancellable);

    if (self->process != NULL) {
        g_subprocess_send_signal(self->process, SIGTERM);
        g_clear_object(&self->process);
    }

    g_clear_object(&self->cancellable);
    g_clear_object(&self->log_stream);
    g_clear_pointer(&self->environment, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_process_runtime_parent_class)->dispose(object);
}

static void
clawt_process_runtime_finalize(GObject *object)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(object);

    g_clear_pointer(&self->binary, g_free);
    g_clear_pointer(&self->config_path, g_free);
    g_clear_pointer(&self->context, g_main_context_unref);

    G_OBJECT_CLASS(clawt_process_runtime_parent_class)->finalize(object);
}

static void
clawt_process_runtime_class_init(ClawtProcessRuntimeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtAgentRuntimeClass *runtime_class = CLAWT_AGENT_RUNTIME_CLASS(klass);

    object_class->dispose = clawt_process_runtime_dispose;
    object_class->finalize = clawt_process_runtime_finalize;

    runtime_class->start = process_runtime_start;
    runtime_class->stop = process_runtime_stop;
    runtime_class->is_alive = process_runtime_is_alive;
    runtime_class->get_pid = process_runtime_get_pid;
    runtime_class->get_caps = process_runtime_get_caps;
    runtime_class->interrupt = process_runtime_interrupt;
}

static void
clawt_process_runtime_init(ClawtProcessRuntime *self)
{
    self->running = FALSE;
}
