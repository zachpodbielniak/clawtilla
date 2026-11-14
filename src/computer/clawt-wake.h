/*
 * clawt-wake.h - Waking a machine that sleeps, once per wave of callers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A remote machine that suspends when idle refuses every connection until
 * something wakes it. The thing that wakes it -- a Wake-on-LAN packet, a
 * provider's resume call -- is a program the operator names, and it has
 * three properties that are easy to get wrong: it must run once however
 * many commands are waiting, it must not run for ever, and the time it
 * takes belongs to nobody's command timeout.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * CLAWT_WAKE_MAX_SECONDS:
 *
 * The longest a wake command may run before it is killed, along with
 * every process it started. Long enough to resume a paused cloud server;
 * short enough that a hung provider CLI does not hold a turn for ever.
 */
#define CLAWT_WAKE_MAX_SECONDS (120)

/**
 * clawt_run_argv_bounded:
 * @argv: (array zero-terminated=1): the program and its arguments; never
 *   run through a shell
 * @timeout_seconds: kill it after this long; 0 for no limit
 * @exit_status: (out) (optional): its exit status, when it exited
 * @stdout_text: (out) (optional) (transfer full): what it printed
 * @stderr_text: (out) (optional) (transfer full): what it complained
 * @error: return location for a #GError
 *
 * Runs a program in a process group of its own and waits for it, on a
 * private main context so it can be called from any worker thread.
 *
 * At the deadline the **whole group** is sent SIGKILL and the read of its
 * output is cancelled. Killing only the child is not enough: a wake
 * script that runs a provider's CLI leaves that CLI holding the pipes,
 * and the wait would go on until it exited on its own.
 *
 * The child gets the daemon's allowlisted environment, not the daemon's
 * own.
 *
 * Returns: %TRUE if it ran and exited (whatever the status); %FALSE with
 *   %CLAWT_ERROR_TIMEOUT at the deadline, or another error if it could not
 *   be started
 */
gboolean
clawt_run_argv_bounded(
	const gchar * const  *argv,
	guint                 timeout_seconds,
	gint                 *exit_status,
	gchar               **stdout_text,
	gchar               **stderr_text,
	GError              **error
);

#define CLAWT_TYPE_WAKER (clawt_waker_get_type())

G_DECLARE_FINAL_TYPE(ClawtWaker, clawt_waker, CLAWT, WAKER, GObject)

/**
 * clawt_waker_new:
 * @argv: (array zero-terminated=1): the wake command
 * @timeout_seconds: how long one run may take; 0 or anything over
 *   %CLAWT_WAKE_MAX_SECONDS means %CLAWT_WAKE_MAX_SECONDS
 *
 * Returns: (transfer full): a new #ClawtWaker
 */
ClawtWaker *
clawt_waker_new(
	const gchar * const *argv,
	guint                timeout_seconds
);

/**
 * clawt_waker_get_argv:
 * @self: a #ClawtWaker
 *
 * Returns: (transfer none) (array zero-terminated=1): the wake command
 */
const gchar * const *
clawt_waker_get_argv(
	ClawtWaker *self
);

/**
 * clawt_waker_get_timeout:
 * @self: a #ClawtWaker
 *
 * Returns: the seconds one run may take, after the cap
 */
guint
clawt_waker_get_timeout(
	ClawtWaker *self
);

/**
 * clawt_waker_get_runs:
 * @self: a #ClawtWaker
 *
 * How many times the command has actually been run. Every caller that
 * joined a run already under way shares that one, so this is the number
 * of waves, not of callers.
 *
 * Returns: the number of runs so far
 */
guint
clawt_waker_get_runs(
	ClawtWaker *self
);

/**
 * clawt_waker_wake:
 * @self: a #ClawtWaker
 * @waited_usec: (out) (optional): how long this caller waited, in
 *   microseconds
 * @error: return location for a #GError
 *
 * Runs the wake command, or waits for the run already under way.
 *
 * One run per concurrent wave: a caller arriving while the command runs
 * does not start a second one; it waits, and gets that run's answer. A
 * caller arriving after a run has ended starts the next wave.
 *
 * @waited_usec is this caller's own share: from when it arrived to when
 * the run ended -- not the run's whole length, which a caller that joined
 * late did not spend. It is what a caller keeps out of its own timeout,
 * and what it may report.
 *
 * Blocking. Call it from a worker thread, never from a main loop.
 *
 * Returns: %TRUE if the wake command exited 0; otherwise %FALSE with an
 *   error saying the wake failed and how -- the exit status and what it
 *   wrote, the timeout, or why it could not be started
 */
gboolean
clawt_waker_wake(
	ClawtWaker  *self,
	gint64      *waited_usec,
	GError     **error
);

G_END_DECLS
