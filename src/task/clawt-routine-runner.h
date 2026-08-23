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

G_END_DECLS
