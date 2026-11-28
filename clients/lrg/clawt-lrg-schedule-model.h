/*
 * clawt-lrg-schedule-model.h - Rows the Schedule page draws
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The Day, Week, Month and Upcoming views in clawtilla-gtk and
 * clawtilla-lrg both call this.  It includes no libregnum and no GTK
 * header: tests/test-lrg-models-schedule.c feeds it a canned
 * routine.occurrences reply and reads the rows back.  What a card
 * says, which zone a time is shown in, and the sentence a refused
 * window keeps, are decided here once.
 */

#pragma once

#include "clawt-lrg-model.h"

G_BEGIN_DECLS

/**
 * ClawtLrgSchedView:
 * @CLAWT_LRG_SCHED_DAY: one local day
 * @CLAWT_LRG_SCHED_WEEK: Monday through the following Monday
 * @CLAWT_LRG_SCHED_MONTH: the month grid, leading and trailing weeks
 * @CLAWT_LRG_SCHED_UPCOMING: the next runs, from routine.upcoming
 *
 * Which Schedule view is on screen.  Month is the one that asks for
 * collapse.
 */
typedef enum {
    CLAWT_LRG_SCHED_DAY = 0,
    CLAWT_LRG_SCHED_WEEK,
    CLAWT_LRG_SCHED_MONTH,
    CLAWT_LRG_SCHED_UPCOMING
} ClawtLrgSchedView;

/**
 * ClawtLrgSchedAction:
 * @CLAWT_LRG_SCHED_ACT_NONE: nothing to open
 * @CLAWT_LRG_SCHED_ACT_TASK: a past run that became a task
 * @CLAWT_LRG_SCHED_ACT_ROUTINE: a future routine, opened in the editor
 * @CLAWT_LRG_SCHED_ACT_DETAIL: show the tooltip; no task and no editor
 * @CLAWT_LRG_SCHED_ACT_EXPAND: a collapsed day-group
 *
 * What activating the card does.  The view does not decide this again.
 */
typedef enum {
    CLAWT_LRG_SCHED_ACT_NONE = 0,
    CLAWT_LRG_SCHED_ACT_TASK,
    CLAWT_LRG_SCHED_ACT_ROUTINE,
    CLAWT_LRG_SCHED_ACT_DETAIL,
    CLAWT_LRG_SCHED_ACT_EXPAND
} ClawtLrgSchedAction;

/**
 * ClawtLrgSchedRow:
 * @title: the card's title
 * @agent: the agent id
 * @when: the local time, in the daemon's zone
 * @day: the local date, YYYY-MM-DD, in that zone
 * @status: the wire nick
 * @status_label: the nick in words, with the libreclaw note when that is what it is
 * @tone: a badge tone: success, error, warning, accent, neutral, dim
 * @source_note: "libreclaw cron", or %NULL for a routine
 * @tooltip: jitter window, reason and detail
 * @task: (nullable): the task to open
 * @room: (nullable): the task's room
 * @routine: the routine id, or the libreclaw job name
 * @summary: (nullable): a group's label ("every hour, 08:00-18:00")
 * @counts: (nullable): "ok 5, upcoming 6"
 * @action: what a click does
 * @group: %TRUE for a collapsed day
 * @count: how many runs a group folds
 * @sort_key: unix time of @at, or of the group's first run
 * @children: (element-type ClawtLrgSchedRow): a group's runs
 *
 * One card.  Freed with clawt_lrg_schedule_row_free().
 */
typedef struct {
    gchar               *title;
    gchar               *agent;
    gchar               *when;
    gchar               *day;
    gchar               *status;
    gchar               *status_label;
    gchar               *tone;
    gchar               *source_note;
    gchar               *tooltip;
    gchar               *task;
    gchar               *room;
    gchar               *routine;
    gchar               *summary;
    gchar               *counts;
    ClawtLrgSchedAction  action;
    gboolean             group;
    guint                count;
    gint64               sort_key;
    GPtrArray           *children;
} ClawtLrgSchedRow;

void clawt_lrg_schedule_row_free(ClawtLrgSchedRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtLrgSchedRow, clawt_lrg_schedule_row_free)

/**
 * clawt_lrg_schedule_view_count:
 *
 * Returns: how many views there are
 */
guint clawt_lrg_schedule_view_count(void);

/**
 * clawt_lrg_schedule_view_label:
 * @view: a #ClawtLrgSchedView
 *
 * Returns: (transfer none): "Day", "Week", "Month" or "Upcoming"
 */
const gchar *clawt_lrg_schedule_view_label(ClawtLrgSchedView view);

/**
 * clawt_lrg_schedule_wants_collapse:
 * @view: a #ClawtLrgSchedView
 *
 * Month asks for collapse so a 5-minute routine is one card per day.
 * Day and week ask for every run.
 *
 * Returns: whether the occurrences request sets collapse
 */
gboolean clawt_lrg_schedule_wants_collapse(ClawtLrgSchedView view);

/**
 * clawt_lrg_schedule_error_text:
 * @error: (nullable): the request's error
 *
 * The daemon's sentence, unchanged.  A window past calendar_max_days
 * or calendar_max_occurrences is refused with a message naming the
 * bound; cutting it, or replacing it with a shorter one, is the
 * failure this exists to prevent.
 *
 * Returns: (transfer full) (nullable): the message, or %NULL when
 *   there is nothing to say
 */
gchar *clawt_lrg_schedule_error_text(const GError *error);

/**
 * clawt_lrg_schedule_today:
 * @zone: (nullable): a zone name, or %NULL for the viewer's zone
 * @year: (out): the year
 * @month: (out): the month, 1-12
 * @day: (out): the day
 *
 * Today in @zone.  The first request uses the viewer's zone; once a
 * reply has named the daemon's, later moves use that.
 */
void clawt_lrg_schedule_today(const gchar *zone, gint *year, gint *month,
                              gint *day);

/**
 * clawt_lrg_schedule_move:
 * @view: which view the buttons move
 * @direction: -1 previous, 0 today, +1 next
 * @today_y: today's year, used when @direction is 0
 * @today_m: today's month
 * @today_d: today's day
 * @year: (inout): the anchor
 * @month: (inout): the anchor
 * @day: (inout): the anchor
 *
 * Previous, today and next.  Upcoming moves the same anchor a day
 * view would, so leaving Upcoming does not jump.
 */
void clawt_lrg_schedule_move(ClawtLrgSchedView view, gint direction,
                             gint today_y, gint today_m, gint today_d,
                             gint *year, gint *month, gint *day);

/**
 * clawt_lrg_schedule_range:
 * @view: Day, Week or Month
 * @year: the anchor year
 * @month: the anchor month
 * @day: the anchor day
 * @from: (out) (transfer full): YYYY-MM-DD, inclusive
 * @to: (out) (transfer full): YYYY-MM-DD, exclusive
 *
 * The window routine.occurrences is asked for.  A bare date is local
 * midnight in the daemon's zone, which is what the frame wants.
 * Month is at most 42 days.
 */
void clawt_lrg_schedule_range(ClawtLrgSchedView view, gint year, gint month,
                              gint day, gchar **from, gchar **to);

/**
 * clawt_lrg_schedule_occurrences_payload:
 * @from: (nullable): window start
 * @to: (nullable): window end
 * @agent: (nullable): one agent, or %NULL / "" for all
 * @team: (nullable): one team, or %NULL / "" for all
 * @include_disabled: the "show disabled" toggle
 * @collapse: clawt_lrg_schedule_wants_collapse()
 *
 * Returns: (transfer full): a routine.occurrences body
 */
JsonNode *clawt_lrg_schedule_occurrences_payload(const gchar *from,
                                                 const gchar *to,
                                                 const gchar *agent,
                                                 const gchar *team,
                                                 gboolean     include_disabled,
                                                 gboolean     collapse);

/**
 * clawt_lrg_schedule_upcoming_payload:
 * @agent: (nullable): one agent, or %NULL / "" for all
 * @team: (nullable): one team, or %NULL / "" for all
 *
 * limit 10, the panel the page is specified to ask for.
 *
 * Returns: (transfer full): a routine.upcoming body
 */
JsonNode *clawt_lrg_schedule_upcoming_payload(const gchar *agent,
                                              const gchar *team);

/**
 * clawt_lrg_schedule_zone_line:
 * @reply: (nullable): an occurrences or upcoming reply
 *
 * Returns: (transfer full): "Times in America/Detroit", or a line
 *   saying the zone was not named
 */
gchar *clawt_lrg_schedule_zone_line(JsonNode *reply);

/**
 * clawt_lrg_schedule_rows:
 * @reply: (nullable): a routine.occurrences or routine.upcoming reply
 *
 * Cards in time order.  With collapse, a group is one row whose
 * children are the runs.  Times are formatted in the reply's
 * time_zone, never the viewer's.  A libreclaw cron past run is
 * labelled unknown, "not tracked by clawtilla".
 *
 * Returns: (transfer full) (element-type ClawtLrgSchedRow): the cards.
 *   Empty for %NULL, never a silently shortened window.
 */
GPtrArray *clawt_lrg_schedule_rows(JsonNode *reply);

/**
 * clawt_lrg_schedule_unscheduled:
 * @reply: (nullable): a routine.occurrences reply
 *
 * libreclaw interval jobs and schedules that do not parse.  They are
 * not on the grid.
 *
 * Returns: (transfer full) (element-type ClawtLrgSchedRow): the rows
 */
GPtrArray *clawt_lrg_schedule_unscheduled(JsonNode *reply);

G_END_DECLS
