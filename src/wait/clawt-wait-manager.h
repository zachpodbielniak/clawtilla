/*
 * clawt-wait-manager.h - The waits the fleet has set, and when to look
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Private to the library: not in <clawtilla.h>, not scanned into the GIR.
 *
 * Holds every pending wait, one file each under `<state_dir>/waits/`,
 * and decides which is due a look.  It does not look itself: the probe
 * is a function the daemon supplies, which fetches the facts and hands
 * them back through clawt_wait_manager_report().  That split is what
 * lets the scheduling, the expiry, the limit, the persistence and the
 * ending be tested with a probe that is a table of answers -- no
 * network, no computer, no clock.
 *
 * Per wait, never per agent.  An agent that is waiting on three things
 * has three records, each with its own schedule, its own backoff and its
 * own room to report into; a field on the agent saying "waiting" would
 * describe the first of them and lose the other two.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "wait/clawt-wait.h"
#include "mcp/clawt-mcp-tools.h"

G_BEGIN_DECLS

/**
 * CLAWT_WAIT_TICK_SECONDS:
 *
 * How often the manager asks which waits are due.  The waits' own gaps
 * are longer; this only bounds how late a look can be.
 */
#define CLAWT_WAIT_TICK_SECONDS (5)

#define CLAWT_TYPE_WAIT_MANAGER (clawt_wait_manager_get_type())

G_DECLARE_FINAL_TYPE(ClawtWaitManager, clawt_wait_manager, CLAWT,
                     WAIT_MANAGER, GObject)

/**
 * ClawtWaitProbeFunc:
 * @manager: the manager
 * @wait: (transfer none): the wait due a look, marked in flight
 * @user_data: what was registered with the function
 *
 * Takes one look at @wait's condition and reports it, now or later,
 * through clawt_wait_manager_report().  Whatever is kept past the return
 * must hold its own reference to @wait.
 */
typedef void (*ClawtWaitProbeFunc)(ClawtWaitManager *manager,
                                   ClawtWait        *wait,
                                   gpointer          user_data);

/**
 * ClawtWaitEndFunc:
 * @manager: the manager
 * @wait: (transfer none): the wait, in the state it ended in
 * @user_data: what was registered with the function
 *
 * Tells the agent its wait has ended.  Called for every end but one the
 * agent asked for itself -- an agent that cancelled its own wait knows.
 */
typedef void (*ClawtWaitEndFunc)(ClawtWaitManager *manager,
                                 ClawtWait        *wait,
                                 gpointer          user_data);

/**
 * clawt_wait_manager_new:
 * @dir: the directory the waits live in, created if missing
 * @error: (out) (optional): return location for a #GError
 *
 * Opens the waits a previous daemon left, and the directory for new
 * ones.  A record that cannot be read is reported and skipped rather
 * than failing the rest: one corrupt file must not cost every other
 * agent its waits.
 *
 * Returns: (transfer full) (nullable): the manager
 */
ClawtWaitManager *
clawt_wait_manager_new(
	const gchar  *dir,
	GError      **error
);

/**
 * clawt_wait_manager_set_probe_func:
 * @self: a #ClawtWaitManager
 * @func: (nullable) (scope notified): the probe
 * @user_data: for @func
 * @notify: (nullable): frees @user_data
 */
void
clawt_wait_manager_set_probe_func(
	ClawtWaitManager   *self,
	ClawtWaitProbeFunc  func,
	gpointer            user_data,
	GDestroyNotify      notify
);

/**
 * clawt_wait_manager_set_end_func:
 * @self: a #ClawtWaitManager
 * @func: (nullable) (scope notified): what tells the agent
 * @user_data: for @func
 * @notify: (nullable): frees @user_data
 */
void
clawt_wait_manager_set_end_func(
	ClawtWaitManager *self,
	ClawtWaitEndFunc  func,
	gpointer          user_data,
	GDestroyNotify    notify
);

/**
 * clawt_wait_manager_add:
 * @self: a #ClawtWaitManager
 * @wait: (transfer none): a pending wait with its agent, room, label
 *   and condition filled in
 * @hours: how long it lives; clawt_wait_clamp_hours() is applied
 * @limit: how many pending waits the agent may have, or 0 for no limit
 * @now: the current time, in µs
 * @error: (out) (optional): return location for a #GError
 *
 * Gives @wait its id, its timestamps and its first look, writes it to
 * disk and starts watching it.  Refused past @limit, with the count in
 * the message; refused, and nothing kept, when it cannot be written --
 * a wait that would not survive a restart is not the wait the agent was
 * told it had.
 *
 * Returns: %TRUE if it was added
 */
gboolean
clawt_wait_manager_add(
	ClawtWaitManager  *self,
	ClawtWait         *wait,
	gint64             hours,
	guint              limit,
	gint64             now,
	GError           **error
);

/**
 * clawt_wait_manager_get:
 * @self: a #ClawtWaitManager
 * @id: (nullable): which wait
 *
 * Returns: (transfer none) (nullable): the pending wait
 */
ClawtWait *
clawt_wait_manager_get(
	ClawtWaitManager *self,
	const gchar      *id
);

/**
 * clawt_wait_manager_list:
 * @self: a #ClawtWaitManager
 * @agent_id: (nullable): only this agent's, or %NULL for all
 *
 * Oldest first.
 *
 * Returns: (transfer container) (element-type ClawtWait): the pending
 *   waits, each with a reference of its own
 */
GPtrArray *
clawt_wait_manager_list(
	ClawtWaitManager *self,
	const gchar      *agent_id
);

/**
 * clawt_wait_manager_count:
 * @self: a #ClawtWaitManager
 * @agent_id: (nullable): only this agent's, or %NULL for all
 *
 * What the per-agent limit is compared against.  A count of what is
 * pending, kept by nothing but the table itself, so there is no counter
 * to forget to decrement when a wait ends some other way.
 *
 * Returns: how many are pending
 */
guint
clawt_wait_manager_count(
	ClawtWaitManager *self,
	const gchar      *agent_id
);

/**
 * clawt_wait_manager_cancel:
 * @self: a #ClawtWaitManager
 * @id: which wait
 * @agent_id: (nullable): the agent asking, which must own it; %NULL
 *   for the operator, who may cancel any
 * @now: the current time, in µs
 * @error: (out) (optional): return location for a #GError
 *
 * Ends a wait as cancelled.  An agent cancelling its own is not told
 * about it; an operator's cancel is delivered to the agent, which would
 * otherwise go on believing it was waiting.
 *
 * Returns: %TRUE if it was cancelled
 */
gboolean
clawt_wait_manager_cancel(
	ClawtWaitManager  *self,
	const gchar       *id,
	const gchar       *agent_id,
	gint64             now,
	GError           **error
);

/**
 * clawt_wait_manager_drop_agent:
 * @self: a #ClawtWaitManager
 * @agent_id: an agent that no longer exists
 *
 * Forgets every wait an agent had, telling nobody -- there is nobody to
 * tell.
 *
 * Returns: how many were dropped
 */
guint
clawt_wait_manager_drop_agent(
	ClawtWaitManager *self,
	const gchar      *agent_id
);

/**
 * clawt_wait_manager_tick:
 * @self: a #ClawtWaitManager
 * @now: the current time, in µs
 *
 * Ends every wait past its expiry and starts a look at every wait that
 * is due one and has none out.  The timer calls this; a test calls it
 * with whatever @now it likes.
 */
void
clawt_wait_manager_tick(
	ClawtWaitManager *self,
	gint64            now
);

/**
 * clawt_wait_manager_report:
 * @self: a #ClawtWaitManager
 * @wait: the wait the look was for
 * @verdict: what the look found
 * @seen: (nullable): what it saw, in a line
 * @quoted: (nullable): output worth quoting if this ends the wait
 * @now: the current time, in µs
 *
 * Records one look.  %CLAWT_WAIT_MET ends the wait satisfied and
 * %CLAWT_WAIT_IMPOSSIBLE ends it failed, both delivered; the other two
 * schedule the next look, %CLAWT_WAIT_UNANSWERED with backoff.  A report
 * for a wait that has meanwhile ended is ignored.
 */
void
clawt_wait_manager_report(
	ClawtWaitManager *self,
	ClawtWait        *wait,
	ClawtWaitVerdict  verdict,
	const gchar      *seen,
	const gchar      *quoted,
	gint64            now
);

/**
 * clawt_wait_manager_start:
 * @self: a #ClawtWaitManager
 * @context: (nullable): the context the timer runs on
 *
 * Arms the tick on @context, which is pushed around the attach here
 * rather than trusted to be thread-default at the caller.
 */
void
clawt_wait_manager_start(
	ClawtWaitManager *self,
	GMainContext     *context
);

/**
 * clawt_wait_manager_stop:
 * @self: a #ClawtWaitManager
 *
 * Disarms the tick.  The waits stay on disk for the next daemon.
 */
void
clawt_wait_manager_stop(
	ClawtWaitManager *self
);

/**
 * clawt_mcp_tools_set_waits:
 * @self: a #ClawtMcpTools
 * @waits: (nullable): the fleet's waits, or %NULL to withhold the tools
 *
 * Gives the orchestration tools the manager clawtilla_wait_until,
 * _wait_list and _wait_cancel act on.  Declared here rather than in the
 * public tools header, because #ClawtWaitManager is private to the
 * library.  Unowned: the daemon clears it before the manager goes.
 */
void
clawt_mcp_tools_set_waits(
	ClawtMcpTools    *self,
	ClawtWaitManager *waits
);

G_END_DECLS
