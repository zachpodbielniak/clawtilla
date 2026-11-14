/*
 * clawt-busy-watch.h - A turn whose processes are using the CPU is working
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `agents.runtime.turn_timeout_seconds` counts activity: a step, a
 * message, a tool call.  A turn running one long shell command -- a test
 * suite, a build, a migration -- emits none of those until the command
 * returns, so the watchdog read forty silent minutes of compiling as a
 * wedged turn and stopped it.  The CLI reports a tool when it starts and
 * when it ends; nothing in between.
 *
 * This reads the other evidence there is: the CPU time of the agent's
 * process tree, from /proc.  A tree that gained CPU time since the last
 * sweep is doing something, and the daemon counts that as a sign of life.
 * A tree that sleeps on a network socket for an hour gains none, and is
 * still stopped -- which is the case the watchdog exists for.
 *
 * What is summed, and why:
 *
 *   - Every descendant's own user and system time, plus its reaped
 *     children's (`cutime` and `cstime`), so a compiler that ran and
 *     exited between two sweeps still shows up in its parent's total.
 *   - The root's *reaped children* only, never the root's own time.  The
 *     root is the libreclaw process, and its own CPU is bookkeeping, not
 *     the turn's work.
 *
 * A total that goes *down* -- a process exited and its parent had not
 * waited for it yet -- is not activity, and becomes the new baseline.
 * A first sample has nothing to compare with, so it is never busy.
 *
 * /proc is read through an injectable reader so the arithmetic can be
 * tested with a fake process table; a test cannot make a real process
 * tree burn a known number of ticks.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_BUSY_WATCH (clawt_busy_watch_get_type())

G_DECLARE_FINAL_TYPE(ClawtBusyWatch, clawt_busy_watch, CLAWT, BUSY_WATCH,
                     GObject)

/**
 * ClawtProcListFunc:
 * @user_data: what was passed to clawt_busy_watch_set_reader()
 *
 * Every process id there is, the way the numeric entries of /proc name
 * them.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the ids
 */
typedef gchar **(*ClawtProcListFunc)(gpointer user_data);

/**
 * ClawtProcReadFunc:
 * @pid: one id from a #ClawtProcListFunc
 * @user_data: what was passed to clawt_busy_watch_set_reader()
 *
 * The contents of `/proc/<pid>/stat`, or %NULL when the process has gone
 * -- which happens between a listing and a read all the time.
 *
 * Returns: (transfer full) (nullable): the line
 */
typedef gchar *(*ClawtProcReadFunc)(const gchar *pid, gpointer user_data);

/**
 * clawt_busy_watch_new:
 *
 * A watch reading the real /proc, at the kernel's tick rate.
 *
 * Returns: (transfer full): a new #ClawtBusyWatch
 */
ClawtBusyWatch *
clawt_busy_watch_new(
	void
);

/**
 * clawt_busy_watch_set_reader:
 * @self: a #ClawtBusyWatch
 * @list: (nullable) (scope notified) (closure user_data): lists process
 *   ids, or %NULL for /proc
 * @read: (nullable) (scope notified) (closure user_data): reads one stat
 *   line, or %NULL for /proc
 * @user_data: passed to both
 * @notify: (nullable): frees @user_data
 *
 * For tests: where the process table comes from.  Both or neither --
 * a fake listing read against the real /proc would describe somebody
 * else's processes.
 */
void
clawt_busy_watch_set_reader(
	ClawtBusyWatch    *self,
	ClawtProcListFunc  list,
	ClawtProcReadFunc  read,
	gpointer           user_data,
	GDestroyNotify     notify
);

/**
 * clawt_busy_watch_set_ticks_per_second:
 * @self: a #ClawtBusyWatch
 * @ticks: clock ticks per second, or 0 for the kernel's (`_SC_CLK_TCK`)
 *
 * For tests, so a fake table's numbers mean the same thing on every
 * machine.
 */
void
clawt_busy_watch_set_ticks_per_second(
	ClawtBusyWatch *self,
	guint           ticks
);

/**
 * clawt_busy_watch_refresh:
 * @self: a #ClawtBusyWatch
 *
 * Reads the process table once.  Every clawt_busy_watch_sample() until
 * the next refresh answers from this snapshot, so a sweep over ten agents
 * reads /proc once rather than ten times.
 *
 * Returns: how many processes were read
 */
guint
clawt_busy_watch_refresh(
	ClawtBusyWatch *self
);

/**
 * clawt_busy_watch_tree_ticks:
 * @self: a #ClawtBusyWatch
 * @pid: the root of the tree
 * @out_processes: (out) (optional): how many descendants it has
 *
 * The CPU time the tree under @pid has used, in clock ticks, from the
 * last clawt_busy_watch_refresh(): every descendant's own and reaped
 * children's time, and the root's reaped children's time but not its own.
 *
 * Returns: the ticks, or -1 when @pid is not in the snapshot -- a
 *   different answer from a tree that has used nothing
 */
gint64
clawt_busy_watch_tree_ticks(
	ClawtBusyWatch *self,
	gint            pid,
	guint          *out_processes
);

/**
 * clawt_busy_watch_sample:
 * @self: a #ClawtBusyWatch
 * @key: whose tree this is -- an agent id
 * @pid: the root of the tree
 * @now_usec: monotonic microseconds
 * @threshold_percent: how much of one core counts as working; 0 never does
 * @out_percent: (out) (optional): the measured use, in percent of one
 *   core, or -1 when there was nothing to compare with
 *
 * Compares the tree's CPU time with what it was at @key's last sample.
 *
 * Returns: %TRUE when the tree used at least @threshold_percent of a core
 *   since the last sample
 */
gboolean
clawt_busy_watch_sample(
	ClawtBusyWatch *self,
	const gchar    *key,
	gint            pid,
	gint64          now_usec,
	guint           threshold_percent,
	gint           *out_percent
);

/**
 * clawt_busy_watch_forget:
 * @self: a #ClawtBusyWatch
 * @key: whose turn ended
 *
 * Drops @key's baseline.  The next turn's first sample then has nothing
 * to compare with, rather than comparing with the end of the last turn
 * and calling the gap work.
 */
void
clawt_busy_watch_forget(
	ClawtBusyWatch *self,
	const gchar    *key
);

/**
 * clawt_busy_parse_stat:
 * @line: one `/proc/<pid>/stat` line
 * @out_ppid: (out): the parent's id
 * @out_own: (out): the process's own user + system ticks
 * @out_children: (out): its reaped children's user + system ticks
 *
 * Reads the four fields this needs.  The command name is in parentheses
 * and may itself hold spaces and parentheses, so the fields are counted
 * from the *last* closing parenthesis.
 *
 * Returns: %TRUE when the line had them all
 */
gboolean
clawt_busy_parse_stat(
	const gchar *line,
	gint        *out_ppid,
	gint64      *out_own,
	gint64      *out_children
);

G_END_DECLS
