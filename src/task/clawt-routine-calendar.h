/*
 * clawt-routine-calendar.h - The fleet's routines laid out in time
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the Schedule page in every client draws, worked out once.  Each
 * routine's schedule is expanded into the times it fires in a window,
 * and every time that has passed is joined with what actually happened
 * -- the runner's history for the run, its task for how the run went.
 * A libreclaw cron in an agent's `libreclaw:` passthrough is laid out
 * beside them, labelled as such.
 *
 * The answer is the `routine.occurrences` and `routine.upcoming` reply,
 * built here rather than in the daemon so it can be asserted on without
 * one: a calendar tested by starting a fleet is a calendar tested once.
 * docs/ipc-protocol.org is the contract for every field.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "task/clawt-routine-runner.h"

G_BEGIN_DECLS

/**
 * CLAWT_CALENDAR_SOURCE_ROUTINE:
 *
 * An occurrence's `source` for a clawtilla routine.
 */
#define CLAWT_CALENDAR_SOURCE_ROUTINE "routine"

/**
 * CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON:
 *
 * An occurrence's `source` for a cron job in an agent's `libreclaw:`
 * passthrough.  clawtilla lays these out and does not run them, so it
 * cannot say how a past one went.
 */
#define CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON "libreclaw-cron"

/**
 * CLAWT_CALENDAR_COLLAPSE_MIN:
 *
 * How many runs of one routine on one local day make a group when a
 * request asks for `collapse`.  Three, so a twice-a-day routine is still
 * two cards and an hourly one is one.
 */
#define CLAWT_CALENDAR_COLLAPSE_MIN (3)

/**
 * CLAWT_CALENDAR_UPCOMING_MAX:
 *
 * The most runs one `routine.upcoming` returns.
 */
#define CLAWT_CALENDAR_UPCOMING_MAX (200)

/**
 * ClawtCalendarTaskFunc:
 * @task_id: a task a routine run became
 * @out_state: (out): the task's state
 * @out_room: (out) (transfer full) (nullable): the room it runs in
 * @out_reason: (out) (transfer full) (nullable): why it failed, stalled
 *   or was cancelled
 * @user_data: data passed to clawt_routine_calendar_set_task_func()
 *
 * Looks a run's task up, so a past occurrence can say whether the run
 * finished, failed or is still going.
 *
 * Returns: %TRUE when the task is known
 */
typedef gboolean (*ClawtCalendarTaskFunc)(const gchar     *task_id,
                                          ClawtTaskState  *out_state,
                                          gchar          **out_room,
                                          gchar          **out_reason,
                                          gpointer         user_data);

/**
 * ClawtCalendarAgentFunc:
 * @agent_id: an agent
 * @user_data: data passed to clawt_routine_calendar_set_agent_func()
 *
 * Whether the agent is up -- starting, running or degraded -- so a run
 * waiting in its mailbox is being worked towards rather than held.
 *
 * Returns: %TRUE when it is up
 */
typedef gboolean (*ClawtCalendarAgentFunc)(const gchar *agent_id,
                                           gpointer     user_data);

#define CLAWT_TYPE_ROUTINE_CALENDAR (clawt_routine_calendar_get_type())

G_DECLARE_FINAL_TYPE(ClawtRoutineCalendar, clawt_routine_calendar,
                     CLAWT, ROUTINE_CALENDAR, GObject)

/**
 * clawt_routine_calendar_new:
 * @config: the fleet configuration
 * @runner: (nullable): the routine runner, for history, pending jitter
 *   and next runs; without one every past occurrence is unknown
 *
 * Returns: (transfer full): a new #ClawtRoutineCalendar
 */
ClawtRoutineCalendar *clawt_routine_calendar_new(ClawtConfig        *config,
                                                 ClawtRoutineRunner *runner);

/**
 * clawt_routine_calendar_set_task_func:
 * @self: a #ClawtRoutineCalendar
 * @func: (scope notified) (nullable): how to look a task up
 * @user_data: data for @func
 * @notify: (nullable): frees @user_data
 *
 * Without one, a run that started is reported `ok`: it started, and
 * nothing here can say otherwise.
 */
void clawt_routine_calendar_set_task_func(ClawtRoutineCalendar  *self,
                                          ClawtCalendarTaskFunc  func,
                                          gpointer               user_data,
                                          GDestroyNotify         notify);

/**
 * clawt_routine_calendar_set_agent_func:
 * @self: a #ClawtRoutineCalendar
 * @func: (scope notified) (nullable): whether an agent is up
 * @user_data: data for @func
 * @notify: (nullable): frees @user_data
 *
 * Without one, every agent counts as up.
 */
void clawt_routine_calendar_set_agent_func(ClawtRoutineCalendar   *self,
                                           ClawtCalendarAgentFunc  func,
                                           gpointer                user_data,
                                           GDestroyNotify          notify);

/**
 * clawt_routine_calendar_set_limits:
 * @self: a #ClawtRoutineCalendar
 * @max_days: the longest window, in days; 0 for
 *   `daemon.calendar_max_days`
 * @max_occurrences: the most occurrences; 0 for
 *   `daemon.calendar_max_occurrences`
 *
 * Overrides the configured bounds, for a test that would otherwise need
 * five thousand runs to reach one.
 */
void clawt_routine_calendar_set_limits(ClawtRoutineCalendar *self,
                                       guint                 max_days,
                                       guint                 max_occurrences);

/**
 * clawt_routine_calendar_occurrences:
 * @self: a #ClawtRoutineCalendar
 * @from: the start of the window, inclusive
 * @to: the end of the window, exclusive
 * @now: what counts as now: before it is past, after it upcoming
 * @agent: (nullable): only this agent's
 * @team: (nullable): only the agents of this team (`agents.team`)
 * @include_disabled: include routines and crons that are turned off,
 *   and the crons of disabled agents
 * @collapse: fold a routine's runs on one local day into a group when
 *   there are %CLAWT_CALENDAR_COLLAPSE_MIN or more
 * @error: (out) (optional): return location for a #GError
 *
 * The `routine.occurrences` reply: every occurrence in the window, the
 * routines they belong to, and the daemon's zone.
 *
 * Refused, with %CLAWT_ERROR_INVALID_ARGUMENT, for a window that ends
 * before it starts or is longer than the limit, and for one holding more
 * occurrences than the limit -- naming the routine that holds most.
 *
 * Returns: (transfer full) (nullable): the reply object, or %NULL
 */
JsonNode *clawt_routine_calendar_occurrences(ClawtRoutineCalendar  *self,
                                             GDateTime             *from,
                                             GDateTime             *to,
                                             GDateTime             *now,
                                             const gchar           *agent,
                                             const gchar           *team,
                                             gboolean               include_disabled,
                                             gboolean               collapse,
                                             GError               **error);

/**
 * clawt_routine_calendar_upcoming:
 * @self: a #ClawtRoutineCalendar
 * @now: what counts as now
 * @limit: how many runs, at most %CLAWT_CALENDAR_UPCOMING_MAX; 0 for 10
 * @agent: (nullable): only this agent's
 * @team: (nullable): only the agents of this team
 * @error: (out) (optional): return location for a #GError
 *
 * The `routine.upcoming` reply: the next @limit runs of every enabled
 * routine and libreclaw cron, soonest first.  A routine's first run here
 * is clawt_routine_runner_next_run_after() at @now, which is what
 * `routine.list` reports as `next_run`.
 *
 * Returns: (transfer full) (nullable): the reply object, or %NULL
 */
JsonNode *clawt_routine_calendar_upcoming(ClawtRoutineCalendar  *self,
                                          GDateTime             *now,
                                          guint                  limit,
                                          const gchar           *agent,
                                          const gchar           *team,
                                          GError               **error);

/**
 * clawt_routine_calendar_zone_name:
 *
 * The name of the zone the daemon's local time is in -- what a client
 * prints as "Times in America/Detroit".  The `TZ` the process was given
 * when it names a zone, else the zone /etc/localtime points into, else
 * "UTC".
 *
 * Returns: (transfer full): the name
 */
gchar *clawt_routine_calendar_zone_name(void);

/**
 * clawt_routine_calendar_to_ics:
 * @reply: a `routine.occurrences` reply
 * @stamp: (nullable): the DTSTAMP to write, or %NULL for now
 *
 * The occurrences of a reply as an iCalendar file (RFC 5545): one
 * read-only VEVENT each, with a UID that is the same for the same
 * routine at the same time however often it is exported, DTSTART in
 * UTC, the routine's title as SUMMARY and its agent and status in
 * DESCRIPTION.  Grouped occurrences are written one by one.
 *
 * Lines end in CRLF and are folded at 75 octets, never inside a UTF-8
 * character.
 *
 * Returns: (transfer full): the calendar
 */
gchar *clawt_routine_calendar_to_ics(JsonObject *reply,
                                     GDateTime  *stamp);

G_END_DECLS
