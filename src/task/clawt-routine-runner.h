/*
 * clawt-routine-runner.h - Running the standing work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The runner knows about time and about the config, and nothing about
 * agents: it decides *that* a routine is due and hands it to a callback.
 * That is what makes "did this fire when it should" answerable without a
 * fleet, and it keeps the daemon's idea of how to start work in one
 * place instead of two.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * ClawtRoutineRunFunc:
 * @routine_id: which routine
 * @agent_id: who should do it
 * @prompt: what to ask for
 * @user_data: data passed to clawt_routine_runner_set_run_func()
 * @error: (out) (optional): return location for a #GError
 *
 * Starts one run.
 *
 * Returns: (transfer none) (nullable): the task id, or %NULL with @error
 *   set
 */
typedef const gchar *(*ClawtRoutineRunFunc)(const gchar  *routine_id,
                                            const gchar  *agent_id,
                                            const gchar  *prompt,
                                            gpointer      user_data,
                                            GError      **error);

#define CLAWT_TYPE_ROUTINE_RUNNER (clawt_routine_runner_get_type())

G_DECLARE_FINAL_TYPE(ClawtRoutineRunner, clawt_routine_runner,
                     CLAWT, ROUTINE_RUNNER, GObject)

/**
 * clawt_routine_runner_new:
 * @config: the fleet configuration
 * @state_path: where to remember what has run
 *
 * Returns: (transfer full): a new #ClawtRoutineRunner
 */
ClawtRoutineRunner *clawt_routine_runner_new(ClawtConfig *config,
                                             const gchar *state_path);

/**
 * clawt_routine_runner_set_run_func:
 * @self: a #ClawtRoutineRunner
 * @func: (scope forever): how to start a run
 * @user_data: data for @func
 *
 * Set once, by the daemon, and never replaced -- hence `forever`.
 */
void clawt_routine_runner_set_run_func(ClawtRoutineRunner  *self,
                                       ClawtRoutineRunFunc  func,
                                       gpointer             user_data);

/**
 * clawt_routine_runner_set_config:
 * @self: a #ClawtRoutineRunner
 * @config: the fleet configuration
 *
 * Points it at a reloaded config.
 */
void clawt_routine_runner_set_config(ClawtRoutineRunner *self,
                                     ClawtConfig        *config);

/**
 * clawt_routine_runner_start:
 * @self: a #ClawtRoutineRunner
 * @context: (nullable): the main context to tick on
 *
 * Begins ticking, once a minute.
 *
 * A minute is the resolution cron has, so anything finer would be work
 * done to arrive at the same answer.  The tick is cheap: it is a
 * comparison per routine.
 */
void clawt_routine_runner_start(ClawtRoutineRunner *self,
                                GMainContext       *context);

void clawt_routine_runner_stop(ClawtRoutineRunner *self);

/**
 * clawt_routine_runner_tick:
 * @self: a #ClawtRoutineRunner
 *
 * Considers every routine once, exactly as the timer does.
 *
 * This is what the timer calls, named rather than hidden so that "did
 * this fire when it should" stays answerable without waiting a minute
 * for a tick -- which is the whole reason the runner knows nothing about
 * agents.
 *
 * A routine with `jitter_seconds` is *held* rather than started: it is
 * armed here and runs when its delay elapses, so a tick that arms one
 * has done its job and started nothing.
 */
void clawt_routine_runner_tick(ClawtRoutineRunner *self);

/**
 * clawt_routine_runner_next_run:
 * @self: a #ClawtRoutineRunner
 * @routine_id: which routine
 *
 * When it is next due.
 *
 * Returns: (transfer full) (nullable): the time, or %NULL for a manual
 *   or disabled routine
 */
GDateTime *clawt_routine_runner_next_run(ClawtRoutineRunner *self,
                                         const gchar        *routine_id);

/**
 * clawt_routine_runner_last_run:
 * @self: a #ClawtRoutineRunner
 * @routine_id: which routine
 * @out_state: (out) (optional): how it went
 * @out_detail: (out) (optional) (transfer none): why, when it failed
 *
 * Returns: when it last ran, as a Unix time, or 0
 */
gint64 clawt_routine_runner_last_run(ClawtRoutineRunner  *self,
                                     const gchar         *routine_id,
                                     ClawtRunState       *out_state,
                                     const gchar        **out_detail);

/**
 * clawt_routine_runner_run_now:
 * @self: a #ClawtRoutineRunner
 * @routine_id: which routine
 * @error: (out) (optional): return location for a #GError
 *
 * Runs one immediately, whatever its schedule says and whether or not it
 * is enabled.
 *
 * Being able to run a disabled routine is the point: it is how you try
 * one before trusting it with a schedule.
 *
 * Returns: (transfer none) (nullable): the task id
 */
const gchar *clawt_routine_runner_run_now(ClawtRoutineRunner  *self,
                                          const gchar         *routine_id,
                                          GError             **error);

/**
 * clawt_routine_runner_catch_up:
 * @self: a #ClawtRoutineRunner
 *
 * Deals with everything whose time passed while the daemon was down.
 *
 * A routine with `catch_up` runs once, however many it missed -- a
 * laptop opened after a long weekend should not deliver a stack of good
 * mornings at once.  Everything else is recorded as missed, which is
 * deliberately not the same as failed: a routine that did not run
 * because the machine was asleep is not broken, and showing it as broken
 * would train somebody to ignore the one that is.
 */
void clawt_routine_runner_catch_up(ClawtRoutineRunner *self);

/**
 * clawt_routine_runner_note_start:
 * @self: a #ClawtRoutineRunner
 * @routine_id: which routine
 * @run_at_us: when the run began, as g_get_real_time() microseconds
 * @computer_error: (nullable): why the agent's computer did not start,
 *   or %NULL when it did
 *
 * Records whether one run's computer came up.
 *
 * A run whose computer started sets the routine's count of failed
 * starts back to zero; one whose computer did not adds one.  A run that
 * began before the routine was last edited counts for nothing either
 * way, because the edit may well have been the fix -- a new agent, a
 * different computer -- and a failure from the old shape must not count
 * against the new one.
 *
 * When the count reaches `routines.max_start_failures` (0 is never) the
 * routine is remembered as turned off, with the error, and this returns
 * %TRUE.  The runner does not change the config itself: the caller turns
 * the routine off through the same save every client uses, so the change
 * is announced, reloaded and visible like any other edit.
 *
 * Returns: %TRUE when this failure reached the limit
 */
gboolean clawt_routine_runner_note_start(
	ClawtRoutineRunner *self,
	const gchar        *routine_id,
	gint64              run_at_us,
	const gchar        *computer_error
);

/**
 * clawt_routine_runner_get_start_failures:
 * @self: a #ClawtRoutineRunner
 * @routine_id: which routine
 *
 * Runs in a row whose computer did not start, since the last one that
 * did or the last edit.
 *
 * Returns: the count
 */
guint clawt_routine_runner_get_start_failures(
	ClawtRoutineRunner *self,
	const gchar        *routine_id
);

/**
 * clawt_routine_runner_get_turned_off:
 * @self: a #ClawtRoutineRunner
 * @routine_id: which routine
 * @out_failures: (out) (optional): how many failed starts turned it off
 * @out_error: (out) (optional) (transfer none): the computer's last error
 *
 * Whether the routine was turned off for failing to start, and is still
 * off.  Turning it back on -- `enabled: true`, from any client -- clears
 * this along with the count.
 *
 * Returns: %TRUE when it was turned off for that reason
 */
gboolean clawt_routine_runner_get_turned_off(
	ClawtRoutineRunner  *self,
	const gchar         *routine_id,
	guint               *out_failures,
	const gchar        **out_error
);

/**
 * clawt_routine_max_start_failures:
 * @routine: a #ClawtRoutine
 *
 * The routine's `max_start_failures`, with a negative value read as the
 * schema default -- a typo must not quietly turn the guard off.
 *
 * Returns: the limit, 0 for never
 */
guint clawt_routine_max_start_failures(ClawtRoutine *routine);

/**
 * clawt_routine_turned_off_note:
 * @failures: how many failed starts turned it off
 * @error: (nullable): the computer's error
 *
 * What a routine turned off for failing to start says about itself, in
 * every client's list: "turned off after 3 failed starts: <error>".
 *
 * Returns: (transfer full): the sentence
 */
gchar *clawt_routine_turned_off_note(
	guint        failures,
	const gchar *error
);

/**
 * clawt_routine_turned_off_alert:
 * @routine_id: which routine
 * @failures: how many failed starts turned it off
 * @error: (nullable): the computer's error
 *
 * The alert raised when a routine is turned off, naming the routine, the
 * count and the computer's error, and what to do about it.
 *
 * Returns: (transfer full): the sentence
 */
gchar *clawt_routine_turned_off_alert(
	const gchar *routine_id,
	guint        failures,
	const gchar *error
);

/**
 * clawt_routine_start_failures_hint:
 *
 * The short explanation every routine editor shows under the
 * `max_start_failures` field.  One copy, so three clients cannot
 * describe the setting three ways.
 *
 * Returns: (transfer none): the sentence
 */
const gchar *clawt_routine_start_failures_hint(void);

G_END_DECLS
