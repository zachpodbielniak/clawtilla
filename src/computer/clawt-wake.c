/*
 * clawt-wake.c - Waking a machine that sleeps, once per wave of callers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-wake.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * How much of a failed command's stderr goes into the error.
 *
 * The tail, not the head: a provider CLI prints its banner first and the
 * reason it gave up last.
 */
#define STDERR_TAIL_BYTES (400)

/* ── Running one bounded command ─────────────────────────────────── */

typedef struct {
	GSubprocess   *process;
	GMainLoop     *loop;
	GMainContext  *context;
	GCancellable  *read_cancel;
	GPid           group;
	gboolean       timed_out;
	gboolean       exited;     /* on_bounded_exited() has run */
	gchar         *stdout_text;
	gchar         *stderr_text;
	GError        *error;
} BoundedWait;

/*
 * In the child, between fork and exec.
 *
 * A process group of its own, so the deadline can kill everything the
 * command started and not only the command: a script that runs a cloud
 * CLI leaves that CLI holding the output pipes, and killing the script
 * alone left the wait running until the CLI gave up on its own.
 *
 * PR_SET_PDEATHSIG as well, so a daemon killed mid-wake does not leave a
 * wake script running against nobody. It is tied to the thread that
 * forked, and that thread is blocked in this wait until the command ends.
 */
static void
bounded_child_setup(
	gpointer user_data
){
	(void)user_data;

	(void)setpgid(0, 0);
	(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
}

/*
 * Kills the whole group, which is the command and everything it started.
 *
 * ESRCH is the group having gone already, which is the outcome wanted.
 */
static void
bounded_kill_group(
	BoundedWait *wait
){
	if (wait->group > 0 && kill(-wait->group, SIGKILL) != 0 && errno != ESRCH)
		g_debug("wake: could not signal process group %d: %s",
		        (gint)wait->group, g_strerror(errno));
}

static gboolean
on_bounded_deadline(
	gpointer user_data
){
	BoundedWait *wait;

	wait = (BoundedWait *)user_data;
	wait->timed_out = TRUE;

	/*
	 * The group first and the read second: cancelling the read alone
	 * would end the wait with the processes still running, and killing
	 * alone can leave a grandchild that left the group holding the pipe.
	 */
	bounded_kill_group(wait);
	g_cancellable_cancel(wait->read_cancel);

	return G_SOURCE_REMOVE;
}

/*
 * The command itself has exited. Whatever it left behind in its group
 * goes with it: a wake command is done when it returns, and a leftover
 * holding the pipes would otherwise keep this wait open to the deadline
 * and report a command that succeeded as one that timed out.
 */
static void
on_bounded_exited(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	BoundedWait *wait;

	wait = (BoundedWait *)user_data;
	(void)g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);

	bounded_kill_group(wait);
	wait->exited = TRUE;
}

static void
on_bounded_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	BoundedWait *wait;

	wait = (BoundedWait *)user_data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
	                                     &wait->stdout_text,
	                                     &wait->stderr_text,
	                                     &wait->error);
	g_main_loop_quit(wait->loop);
}

gboolean
clawt_run_argv_bounded(
	const gchar * const  *argv,
	guint                 timeout_seconds,
	gint                 *exit_status,
	gchar               **stdout_text,
	gchar               **stderr_text,
	GError              **error
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autoptr(GCancellable) read_cancel = NULL;
	g_auto(GStrv) environment = NULL;
	GSource *deadline = NULL;
	BoundedWait wait;
	const gchar *identifier;
	gboolean ok;

	g_return_val_if_fail(argv != NULL && argv[0] != NULL, FALSE);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);

	/*
	 * The allowlisted environment, never the daemon's: the rule every
	 * child of the daemon follows, and the reason a command named in a
	 * config file cannot read the operator's SSH agent socket.
	 */
	environment = clawt_build_child_environment(NULL);
	g_subprocess_launcher_set_environ(launcher, environment);
	g_subprocess_launcher_set_child_setup(launcher, bounded_child_setup,
	                                      NULL, NULL);

	memset(&wait, 0, sizeof(wait));

	wait.process = g_subprocess_launcher_spawnv(launcher, argv, error);

	if (wait.process == NULL)
		return FALSE;

	identifier = g_subprocess_get_identifier(wait.process);
	wait.group = (identifier != NULL)
	             ? (GPid)g_ascii_strtoll(identifier, NULL, 10) : 0;

	/*
	 * A context of our own, pushed for the length of the wait, so the
	 * child watch and the read both dispatch here -- a worker thread has
	 * no thread-default, and the global default is a loop somebody else
	 * runs.
	 */
	context = g_main_context_new();
	g_main_context_push_thread_default(context);
	loop = g_main_loop_new(context, FALSE);
	read_cancel = g_cancellable_new();

	wait.loop = loop;
	wait.context = context;
	wait.read_cancel = read_cancel;

	/*
	 * Milliseconds, not g_timeout_source_new_seconds(): that one rounds
	 * to a whole-second boundary shared with other timers and can fire
	 * most of a second early, which on a deadline that is the whole
	 * point is a deadline nobody set.
	 */
	if (timeout_seconds > 0) {
		deadline = g_timeout_source_new((guint)timeout_seconds * 1000);
		g_source_set_callback(deadline, on_bounded_deadline, &wait, NULL);
		g_source_attach(deadline, context);
	}

	g_subprocess_wait_async(wait.process, NULL, on_bounded_exited, &wait);

	/* Nothing to say on stdin; an empty write closes it at once. */
	g_subprocess_communicate_utf8_async(wait.process, NULL, read_cancel,
	                                    on_bounded_read, &wait);
	g_main_loop_run(loop);

	if (deadline != NULL) {
		g_source_destroy(deadline);
		g_source_unref(deadline);
	}

	/*
	 * Reaped before returning, so a timed-out command is gone rather than
	 * dying in the background. The group was sent SIGKILL, so this is
	 * short; it also lets the pending exit callback run here, on this
	 * context, before the stack it points at goes away.
	 */
	if (wait.timed_out)
		bounded_kill_group(&wait);

	g_subprocess_wait(wait.process, NULL, NULL);

	/*
	 * Until the exit callback has run, not merely until the context looks
	 * idle.  The child is reaped by now, but the wait_async completion is
	 * queued onto this context from GLib's worker thread and can arrive
	 * just after a non-blocking pass finds nothing: the pass ended, the
	 * completion was attached to a context nobody iterated again, and its
	 * task and the context leaked -- an intermittent LeakSanitizer report
	 * from /wake/once-per-wave.  Had it ever dispatched later, it would
	 * have written to this stack frame.  It is certain to come, since the
	 * process has exited, so blocking on it is bounded.
	 */
	while (!wait.exited)
		g_main_context_iteration(context, TRUE);

	while (g_main_context_iteration(context, FALSE))
		;

	g_main_context_pop_thread_default(context);

	ok = TRUE;

	if (wait.timed_out) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
		            "it was still running after %u seconds, so it was "
		            "stopped along with everything it started",
		            timeout_seconds);
		ok = FALSE;
	} else if (wait.error != NULL) {
		g_propagate_error(error, wait.error);
		wait.error = NULL;
		ok = FALSE;
	}

	if (ok && exit_status != NULL)
		*exit_status = g_subprocess_get_if_exited(wait.process)
		               ? g_subprocess_get_exit_status(wait.process)
		               : (128 + g_subprocess_get_term_sig(wait.process));

	if (ok && stdout_text != NULL)
		*stdout_text = g_steal_pointer(&wait.stdout_text);

	if (ok && stderr_text != NULL)
		*stderr_text = g_steal_pointer(&wait.stderr_text);

	g_clear_error(&wait.error);
	g_free(wait.stdout_text);
	g_free(wait.stderr_text);
	g_clear_object(&wait.process);

	return ok;
}

/* ── ClawtWaker ──────────────────────────────────────────────────── */

struct _ClawtWaker {
	GObject  parent_instance;

	GStrv    argv;
	guint    timeout_seconds;

	/*
	 * The wave. Everything below is read and written under @lock; a
	 * caller that finds @running set waits on @done for @generation to
	 * move, and then reads the outcome the run left behind.
	 */
	GMutex   lock;
	GCond    done;
	gboolean running;
	guint64  generation;
	guint    runs;
	gint64   wave_start;
	gint64   wave_end;
	gboolean wave_ok;
	GError  *wave_error;
};

G_DEFINE_FINAL_TYPE(ClawtWaker, clawt_waker, G_TYPE_OBJECT)

ClawtWaker *
clawt_waker_new(
	const gchar * const *argv,
	guint                timeout_seconds
){
	ClawtWaker *self;

	g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

	self = (ClawtWaker *)g_object_new(CLAWT_TYPE_WAKER, NULL);
	self->argv = g_strdupv((GStrv)argv);

	/*
	 * Capped rather than refused: a config asking for longer plainly
	 * means "as long as you will allow", and the allowance is fixed.
	 */
	self->timeout_seconds =
		(timeout_seconds == 0 || timeout_seconds > CLAWT_WAKE_MAX_SECONDS)
		? CLAWT_WAKE_MAX_SECONDS : timeout_seconds;

	return self;
}

const gchar * const *
clawt_waker_get_argv(
	ClawtWaker *self
){
	g_return_val_if_fail(CLAWT_IS_WAKER(self), NULL);

	return (const gchar * const *)self->argv;
}

guint
clawt_waker_get_timeout(
	ClawtWaker *self
){
	g_return_val_if_fail(CLAWT_IS_WAKER(self), 0);

	return self->timeout_seconds;
}

guint
clawt_waker_get_runs(
	ClawtWaker *self
){
	guint runs;

	g_return_val_if_fail(CLAWT_IS_WAKER(self), 0);

	g_mutex_lock(&self->lock);
	runs = self->runs;
	g_mutex_unlock(&self->lock);

	return runs;
}

/*
 * One run of the command, outside the lock, turned into a sentence that
 * says the wake failed and how. Returns %TRUE only for exit status 0.
 */
static gboolean
waker_run_once(
	ClawtWaker  *self,
	GError     **error
){
	g_autofree gchar *line = NULL;
	g_autofree gchar *errors = NULL;
	g_autoptr(GError) local = NULL;
	gint exit_status;

	line = clawt_command_line_join((const gchar * const *)self->argv);
	exit_status = -1;

	if (!clawt_run_argv_bounded((const gchar * const *)self->argv,
	                            self->timeout_seconds, &exit_status, NULL,
	                            &errors, &local)) {
		/*
		 * A spawn failure and a timeout read differently on purpose:
		 * the first is a typo or a missing package, the second is a
		 * machine or a provider that did not answer.
		 */
		if (g_error_matches(local, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT))
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
			            "the wake command (%s) failed: %s", line,
			            local->message);
		else
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
			            "the wake command (%s) failed: it could not be "
			            "started: %s", line, local->message);
		return FALSE;
	}

	if (exit_status == 0)
		return TRUE;

	if (errors != NULL)
		g_strstrip(errors);

	{
		const gchar *tail;
		gsize length;

		tail = (errors != NULL) ? errors : "";
		length = strlen(tail);

		if (length > STDERR_TAIL_BYTES)
			tail = g_utf8_find_next_char(
				tail + length - STDERR_TAIL_BYTES - 1, NULL);

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "the wake command (%s) failed: it exited with status "
		            "%d%s%s", line, exit_status,
		            (tail != NULL && *tail != '\0') ? ": " : " and wrote "
		            "nothing to say why",
		            (tail != NULL) ? tail : "");
	}

	return FALSE;
}

gboolean
clawt_waker_wake(
	ClawtWaker  *self,
	gint64      *waited_usec,
	GError     **error
){
	g_autoptr(GError) local = NULL;
	gint64 arrived;
	gint64 started;
	gint64 ended;
	guint64 joined;
	gboolean ok;

	g_return_val_if_fail(CLAWT_IS_WAKER(self), FALSE);

	if (waited_usec != NULL)
		*waited_usec = 0;

	arrived = g_get_monotonic_time();

	g_mutex_lock(&self->lock);

	/*
	 * A run is under way: join it. The answer is that run's, and the
	 * time charged is only what this caller spent waiting -- from when it
	 * arrived, not from when the run began.
	 */
	if (self->running) {
		joined = self->generation;

		while (self->generation == joined)
			g_cond_wait(&self->done, &self->lock);

		ok = self->wave_ok;

		if (!ok && self->wave_error != NULL)
			g_propagate_error(error, g_error_copy(self->wave_error));

		if (waited_usec != NULL)
			*waited_usec = MAX(0, self->wave_end -
			                      MAX(self->wave_start, arrived));

		g_mutex_unlock(&self->lock);

		return ok;
	}

	/* Nobody is running it: this caller starts the wave. */
	self->running = TRUE;
	self->runs++;
	started = arrived;
	self->wave_start = started;

	g_mutex_unlock(&self->lock);

	ok = waker_run_once(self, &local);
	ended = g_get_monotonic_time();

	g_mutex_lock(&self->lock);

	self->wave_end = ended;
	self->wave_ok = ok;
	g_clear_error(&self->wave_error);

	if (local != NULL)
		self->wave_error = g_error_copy(local);

	self->running = FALSE;
	self->generation++;
	g_cond_broadcast(&self->done);

	g_mutex_unlock(&self->lock);

	if (waited_usec != NULL)
		*waited_usec = ended - started;

	if (!ok)
		g_propagate_error(error, g_steal_pointer(&local));

	return ok;
}

static void
clawt_waker_finalize(
	GObject *object
){
	ClawtWaker *self;

	self = CLAWT_WAKER(object);

	g_clear_pointer(&self->argv, g_strfreev);
	g_clear_error(&self->wave_error);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->done);

	G_OBJECT_CLASS(clawt_waker_parent_class)->finalize(object);
}

static void
clawt_waker_class_init(
	ClawtWakerClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_waker_finalize;
}

static void
clawt_waker_init(
	ClawtWaker *self
){
	g_mutex_init(&self->lock);
	g_cond_init(&self->done);
}
