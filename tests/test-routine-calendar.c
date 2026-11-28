/*
 * test-routine-calendar.c - The fleet's routines laid out in time
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the Schedule page draws is worked out by ClawtRoutineCalendar and
 * answered as routine.occurrences and routine.upcoming (VENTURE #35).
 * Held to account here, in America/Detroit throughout:
 *
 *   - cron expansion: hourly and daily across both 2026 clock changes,
 *     the day-of-month OR day-of-week rule, steps, weekdays, and the
 *     window's edges;
 *   - the runner's history: a scheduled run is put down against its own
 *     time, the times a late tick or a down daemon stood in for are kept
 *     as missed (or folded into a catch-up), and it survives a restart;
 *   - the past-run join: ok, failed, running, skipped for each reason,
 *     unknown, due, and a Run now off the schedule;
 *   - the bounds, the agent and team filters, libreclaw crons;
 *   - routine.upcoming agreeing with routine.list's next_run, in the
 *     library and over the daemon's own frames;
 *   - collapse grouping, and the .ics export's folding and stable UIDs.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

#define ZONE "America/Detroit"

/* ── Helpers ─────────────────────────────────────────────────────── */

/* A local wall time in Detroit as a GDateTime. */
static GDateTime *
local(
	gint year,
	gint month,
	gint day,
	gint hour,
	gint minute
){
	return g_date_time_new_local(year, month, day, hour, minute, 0.0);
}

/* How many times @expression fires in [@from, @to). */
static guint
count_between(
	const gchar *expression,
	GDateTime   *from,
	GDateTime   *to
){
	g_autoptr(ClawtCron) cron = clawt_cron_parse(expression, NULL);
	g_autoptr(GPtrArray) runs = NULL;

	g_assert_nonnull(cron);
	runs = clawt_cron_between(cron, from, to, 10000, NULL);

	return runs->len;
}

/* ── Cron expansion ──────────────────────────────────────────────── */

/*
 * Hourly across both changes: the spring day has 23 hours and so 23
 * runs, the autumn one 25 -- 01:00 happens twice, as two instants an
 * hour apart, and both are runs.
 */
static void
test_hourly_across_dst(void)
{
	g_autoptr(GDateTime) spring = local(2026, 3, 8, 0, 0);
	g_autoptr(GDateTime) spring_end = local(2026, 3, 9, 0, 0);
	g_autoptr(GDateTime) autumn = local(2026, 11, 1, 0, 0);
	g_autoptr(GDateTime) autumn_end = local(2026, 11, 2, 0, 0);
	g_autoptr(ClawtCron) cron = clawt_cron_parse("0 * * * *", NULL);
	g_autoptr(GPtrArray) runs = NULL;
	guint at_one = 0;
	guint i;

	g_assert_cmpuint(count_between("0 * * * *", spring, spring_end), ==, 23);
	g_assert_cmpuint(count_between("0 * * * *", autumn, autumn_end), ==, 25);

	runs = clawt_cron_between(cron, autumn, autumn_end, 100, NULL);

	for (i = 0; i < runs->len; i++) {
		GDateTime *run = g_ptr_array_index(runs, i);

		if (g_date_time_get_hour(run) == 1)
			at_one++;

		/* every run strictly after the last, an hour apart */
		if (i > 0)
			g_assert_cmpint(g_date_time_difference(run,
				g_ptr_array_index(runs, i - 1)), ==, G_TIME_SPAN_HOUR);
	}

	g_assert_cmpuint(at_one, ==, 2);
}

/*
 * Daily at a time the spring change skips is skipped that day, and at
 * one the autumn change repeats fires twice -- what the scheduler does
 * (clawt_cron_next()), so what the calendar shows.
 */
static void
test_daily_across_dst(void)
{
	g_autoptr(GDateTime) march_7 = local(2026, 3, 7, 0, 0);
	g_autoptr(GDateTime) march_10 = local(2026, 3, 10, 0, 0);
	g_autoptr(GDateTime) october_31 = local(2026, 10, 31, 0, 0);
	g_autoptr(GDateTime) november_2 = local(2026, 11, 2, 0, 0);

	/* 7th and 9th; the 8th has no 02:30 */
	g_assert_cmpuint(count_between("30 2 * * *", march_7, march_10), ==, 2);
	/* 31st once, 1st twice */
	g_assert_cmpuint(count_between("30 1 * * *", october_31, november_2), ==,
	                 3);
	/* an ordinary time is once a day either side */
	g_assert_cmpuint(count_between("0 9 * * *", march_7, march_10), ==, 3);
	g_assert_cmpuint(count_between("0 9 * * *", october_31, november_2), ==,
	                 2);
}

/*
 * Both day fields restricted is day-of-month OR day-of-week.  December
 * 2026 starts on a Tuesday, so "the 1st, and Mondays" is the 1st plus
 * four Mondays -- five -- where an AND would be none.
 */
static void
test_day_of_month_or_day_of_week(void)
{
	g_autoptr(GDateTime) december = local(2026, 12, 1, 0, 0);
	g_autoptr(GDateTime) january = local(2027, 1, 1, 0, 0);

	g_assert_cmpuint(count_between("0 9 1 * 1", december, january), ==, 5);
	/* one restricted: that one decides */
	g_assert_cmpuint(count_between("0 9 1 * *", december, january), ==, 1);
	g_assert_cmpuint(count_between("0 9 * * mon", december, january), ==, 4);
	/* the thirteenth or a Friday: Fridays 4, 11, 18, 25 and the 13th */
	g_assert_cmpuint(count_between("0 9 13 * 5", december, january), ==, 5);
}

static void
test_steps(void)
{
	g_autoptr(GDateTime) day = local(2026, 11, 24, 0, 0);
	g_autoptr(GDateTime) next_day = local(2026, 11, 25, 0, 0);

	g_assert_cmpuint(count_between("*/15 9-10 * * *", day, next_day), ==, 8);
	g_assert_cmpuint(count_between("0 */6 * * *", day, next_day), ==, 4);
	g_assert_cmpuint(count_between("5-50/20 9 * * *", day, next_day), ==, 3);
	/* a step that does not divide the hour restarts at each hour */
	g_assert_cmpuint(count_between("*/7 9 * * *", day, next_day), ==, 9);
}

/* The weekdays preset: Monday 23 November to Sunday 29th is five. */
static void
test_weekdays_and_hourly_presets(void)
{
	g_autoptr(GDateTime) monday = local(2026, 11, 23, 0, 0);
	g_autoptr(GDateTime) next_monday = local(2026, 11, 30, 0, 0);
	g_autofree gchar *weekdays = clawt_cron_from_preset("weekdays", "09:00",
	                                                    NULL, NULL, NULL);
	g_autofree gchar *hourly = clawt_cron_from_preset("hourly", "00:20",
	                                                  NULL, NULL, NULL);

	g_assert_cmpuint(count_between(weekdays, monday, next_monday), ==, 5);
	g_assert_cmpuint(count_between(hourly, monday, next_monday), ==, 7 * 24);
}

/* [from, to): a run at @from is in, one at @to is out, and max is kept. */
static void
test_window_edges(void)
{
	g_autoptr(GDateTime) nine = local(2026, 11, 24, 9, 0);
	g_autoptr(GDateTime) nine_next = local(2026, 11, 25, 9, 0);
	g_autoptr(GDateTime) half_past = NULL;
	g_autoptr(ClawtCron) cron = clawt_cron_parse("* * * * *", NULL);
	g_autoptr(GPtrArray) runs = NULL;
	gboolean truncated = FALSE;

	g_assert_cmpuint(count_between("0 9 * * *", nine, nine_next), ==, 1);

	/* a fraction of a second past the run is past the run */
	half_past = g_date_time_add_seconds(nine, 0.5);
	g_assert_cmpuint(count_between("0 9 * * *", half_past, nine_next), ==, 0);

	runs = clawt_cron_between(cron, nine, nine_next, 10, &truncated);
	g_assert_cmpuint(runs->len, ==, 10);
	g_assert_true(truncated);
	g_clear_pointer(&runs, g_ptr_array_unref);

	runs = clawt_cron_between(cron, nine, nine_next, 2000, &truncated);
	g_assert_cmpuint(runs->len, ==, 1440);
	g_assert_false(truncated);
}

/* ── A fixture: a config, a runner and its state ─────────────────── */

typedef struct {
	gchar              *dir;
	gchar              *state_path;
	ClawtConfig        *config;
	ClawtRoutineRunner *runner;
	ClawtRoutineCalendar *calendar;
	GHashTable         *tasks;      /* task id -> state, as a pointer */
	gboolean            agents_up;
	guint               runs;
} Fixture;

static void
fixture_setup(
	Fixture     *fixture,
	const gchar *yaml_body,
	const gchar *state
){
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-calendar-XXXXXX", NULL);
	fixture->state_path = g_build_filename(fixture->dir, "routines.yaml",
	                                       NULL);

	if (state != NULL)
		g_assert_true(g_file_set_contents(fixture->state_path, state, -1,
		                                  NULL));

	yaml = g_strdup_printf("daemon:\n  state_dir: \"%s/state\"\n%s",
	                       fixture->dir, yaml_body);
	fixture->config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);

	fixture->runner = clawt_routine_runner_new(fixture->config,
	                                           fixture->state_path);
	fixture->calendar = clawt_routine_calendar_new(fixture->config,
	                                               fixture->runner);
	fixture->tasks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                       NULL);
}

static void
fixture_teardown(
	Fixture *fixture
){
	g_clear_object(&fixture->calendar);
	g_clear_object(&fixture->runner);
	g_clear_object(&fixture->config);
	g_clear_pointer(&fixture->tasks, g_hash_table_unref);
	g_clear_pointer(&fixture->state_path, g_free);

	if (fixture->dir != NULL)
		clawt_test_remove_tree(fixture->dir);

	g_clear_pointer(&fixture->dir, g_free);
}

/* The task table the join reads: state, a room, and a reason. */
static gboolean
fake_task(
	const gchar     *task_id,
	ClawtTaskState  *out_state,
	gchar          **out_room,
	gchar          **out_reason,
	gpointer         user_data
){
	Fixture *fixture = user_data;
	gpointer value;

	if (!g_hash_table_lookup_extended(fixture->tasks, task_id, NULL, &value))
		return FALSE;

	*out_state = (ClawtTaskState)GPOINTER_TO_INT(value);
	*out_room = g_strdup_printf("room-of-%s", task_id);
	*out_reason = (*out_state == CLAWT_TASK_FAILED)
		? g_strdup("the agent said it could not") : NULL;

	return TRUE;
}

static gboolean
fake_agent_up(
	const gchar *agent_id,
	gpointer     user_data
){
	return ((Fixture *)user_data)->agents_up;
}

static const gchar *
fake_run(
	const gchar  *routine_id,
	const gchar  *agent_id,
	const gchar  *prompt,
	gboolean      by_hand,
	gpointer      user_data,
	GError      **error
){
	((Fixture *)user_data)->runs++;

	return "task-run";
}

/* The occurrence in @reply for @routine at @at (an ISO UTC string). */
static JsonObject *
find_occurrence(
	JsonObject  *reply,
	const gchar *routine,
	const gchar *at
){
	JsonArray *list = json_object_get_array_member(reply, "occurrences");
	guint i;

	for (i = 0; i < json_array_get_length(list); i++) {
		JsonObject *occurrence = json_array_get_object_element(list, i);

		if (g_strcmp0(json_object_get_string_member(occurrence, "routine"),
		              routine) == 0 &&
		    g_strcmp0(json_object_get_string_member(occurrence, "at"),
		              at) == 0)
			return occurrence;
	}

	return NULL;
}

static const gchar *
text_of(
	JsonObject  *object,
	const gchar *key
){
	return json_object_has_member(object, key)
		? json_object_get_string_member(object, key) : NULL;
}

static guint
count_in(
	JsonObject  *reply,
	const gchar *key,
	const gchar *member,
	const gchar *value
){
	JsonArray *list = json_object_get_array_member(reply, key);
	guint count = 0;
	guint i;

	for (i = 0; i < json_array_get_length(list); i++)
		if (g_strcmp0(text_of(json_array_get_object_element(list, i),
		                      member), value) == 0)
			count++;

	return count;
}

/* ── The runner's history ────────────────────────────────────────── */

static const gchar MINUTELY[] =
	"agents:\n  - id: watcher\n"
	"routines:\n"
	"  - id: minutely\n    agent: watcher\n    instructions: \"Look.\"\n"
	"    schedule: custom\n    cron: \"* * * * *\"\n";

/*
 * A tick that comes late finds a routine with several times behind it.
 * It runs once, against the latest of them, and the ones before are in
 * the history as missed -- not lost, and not runs.
 */
static void
test_a_late_tick_keeps_what_it_missed(void)
{
	Fixture fixture = { 0 };
	g_autofree gchar *state = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(ClawtRoutineRunner) again = NULL;
	g_autoptr(GPtrArray) reread = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 last_run = now - 200;
	gint64 first_due = (last_run / 60 + 1) * 60;
	ClawtRunEntry *last;
	guint i;

	state = g_strdup_printf("minutely:\n  last_run: %" G_GINT64_FORMAT "\n"
	                        "  state: ok\n", last_run);
	fixture_setup(&fixture, MINUTELY, state);
	clawt_routine_runner_set_run_func(fixture.runner, fake_run, &fixture);

	clawt_routine_runner_tick(fixture.runner);
	g_assert_cmpuint(fixture.runs, ==, 1);

	history = clawt_routine_runner_get_history(fixture.runner, "minutely", 0,
	                                           G_MAXINT64);
	g_assert_cmpuint(history->len, >=, 3);

	last = g_ptr_array_index(history, history->len - 1);
	g_assert_cmpint(last->outcome, ==, CLAWT_RUN_OUTCOME_STARTED);
	g_assert_cmpstr(last->task_id, ==, "task-run");
	g_assert_false(last->by_hand);
	/* the latest minute at or before the tick */
	g_assert_cmpint(last->slot % 60, ==, 0);
	g_assert_cmpint(last->slot, <=, g_get_real_time() / G_USEC_PER_SEC);
	g_assert_cmpint(last->slot, >, now - 60);

	for (i = 0; i + 1 < history->len; i++) {
		ClawtRunEntry *entry = g_ptr_array_index(history, i);

		g_assert_cmpint(entry->outcome, ==, CLAWT_RUN_OUTCOME_MISSED);
		g_assert_cmpint(entry->slot, ==, first_due + 60 * (gint64)i);
		g_assert_null(entry->task_id);
	}

	/* and it is in the state file, for the next daemon */
	again = clawt_routine_runner_new(fixture.config, fixture.state_path);
	reread = clawt_routine_runner_get_history(again, "minutely", 0,
	                                          G_MAXINT64);
	g_assert_cmpuint(reread->len, ==, history->len);
	g_assert_cmpint(((ClawtRunEntry *)g_ptr_array_index(reread,
		reread->len - 1))->slot, ==, last->slot);
	g_assert_cmpstr(((ClawtRunEntry *)g_ptr_array_index(reread,
		reread->len - 1))->task_id, ==, "task-run");

	fixture_teardown(&fixture);
}

/*
 * A Run now is in the history at the moment it started, with no
 * scheduled time and marked by hand.
 */
static void
test_run_now_is_history_too(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) history = NULL;
	ClawtRunEntry *entry;

	fixture_setup(&fixture, MINUTELY, NULL);
	clawt_routine_runner_set_run_func(fixture.runner, fake_run, &fixture);

	g_assert_cmpstr(clawt_routine_runner_run_now(fixture.runner, "minutely",
	                                             NULL), ==, "task-run");

	history = clawt_routine_runner_get_history(fixture.runner, "minutely", 0,
	                                           G_MAXINT64);
	g_assert_cmpuint(history->len, ==, 1);
	entry = g_ptr_array_index(history, 0);
	g_assert_cmpint(entry->slot, ==, 0);
	g_assert_true(entry->by_hand);
	g_assert_cmpint(entry->outcome, ==, CLAWT_RUN_OUTCOME_STARTED);

	fixture_teardown(&fixture);
}

static const gchar HOURLY_CATCH_UP[] =
	"agents:\n  - id: watcher\n"
	"routines:\n"
	"  - id: hourly\n    agent: watcher\n    instructions: \"Look.\"\n"
	"    schedule: custom\n    cron: \"0 * * * *\"\n    catch_up: %s\n";

/*
 * After the daemon was down three and a half hours: a catch_up routine
 * runs once for the latest hour and keeps the others as folded into it;
 * one without catch_up keeps every hour as missed.
 */
static void
check_catch_up(
	gboolean catch_up
){
	Fixture fixture = { 0 };
	g_autofree gchar *yaml = g_strdup_printf(HOURLY_CATCH_UP,
	                                         catch_up ? "true" : "false");
	g_autofree gchar *state = NULL;
	g_autoptr(GPtrArray) history = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 last_run = now - 3 * 3600 - 1800;
	ClawtRunEntry *last;
	guint i;

	state = g_strdup_printf("hourly:\n  last_run: %" G_GINT64_FORMAT "\n"
	                        "  state: ok\n", last_run);
	fixture_setup(&fixture, yaml, state);
	clawt_routine_runner_set_run_func(fixture.runner, fake_run, &fixture);

	clawt_routine_runner_catch_up(fixture.runner);
	g_assert_cmpuint(fixture.runs, ==, catch_up ? 1 : 0);

	history = clawt_routine_runner_get_history(fixture.runner, "hourly", 0,
	                                           G_MAXINT64);
	g_assert_cmpuint(history->len, >=, 3);
	g_assert_cmpuint(history->len, <=, 4);

	last = g_ptr_array_index(history, history->len - 1);
	g_assert_cmpint(last->outcome, ==, catch_up ? CLAWT_RUN_OUTCOME_STARTED
	                                            : CLAWT_RUN_OUTCOME_MISSED);
	g_assert_cmpint(last->slot % 3600, ==, 0);
	g_assert_cmpint(now - last->slot, <, 3600);

	for (i = 0; i + 1 < history->len; i++)
		g_assert_cmpint(((ClawtRunEntry *)g_ptr_array_index(history,
			i))->outcome, ==, catch_up ? CLAWT_RUN_OUTCOME_CAUGHT_UP
		                               : CLAWT_RUN_OUTCOME_MISSED);

	fixture_teardown(&fixture);
}

static void
test_catch_up_history(void)
{
	check_catch_up(TRUE);
	check_catch_up(FALSE);
}

/* ── The past-run join ───────────────────────────────────────────── */

static const gchar DAILY[] =
	"agents:\n  - id: hornbill\n"
	"routines:\n"
	"  - id: deals\n    agent: hornbill\n    instructions: \"Check deals.\"\n"
	"    description: \"Daily deal check\"\n"
	"    schedule: daily\n    at: \"09:00\"\n";

/* 09:00 Detroit on a November 2026 day, as a Unix time. */
static gint64
nine_on(
	gint day
){
	g_autoptr(GDateTime) when = local(2026, 11, day, 9, 0);

	return g_date_time_to_unix(when);
}

/*
 * One recorded line per kind of outcome, on consecutive days, and a
 * Run now; then the window Nov 1-15 seen from the 20th.
 */
static void
test_past_runs_are_joined(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GString) state = g_string_new("deals:\n  last_run: 1\n"
	                                        "  state: ok\n  history:\n");
	g_autoptr(GDateTime) from = local(2026, 11, 1, 0, 0);
	g_autoptr(GDateTime) to = local(2026, 11, 15, 0, 0);
	g_autoptr(GDateTime) now = local(2026, 11, 20, 12, 0);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *reply;
	JsonObject *occurrence;
	struct {
		gint         day;
		const gchar *outcome;
		const gchar *task;
		const gchar *status;
		const gchar *reason;
	} lines[] = {
		{ 1, "started", "t-ok", "ok", NULL },
		{ 2, "started", "t-fail", "failed", NULL },
		{ 3, "started", "t-run", "running", NULL },
		{ 4, "started", "t-queued", "running", NULL },
		{ 5, "failed", NULL, "failed", NULL },
		{ 6, "missed", NULL, "skipped", "daemon_down" },
		{ 7, "caught-up", NULL, "skipped", "catch_up" },
		{ 8, "refused", NULL, "skipped", "refused" },
		{ 9, "started", "t-cancel", "skipped", "cancelled" },
		{ 10, "started", "t-gone", "ok", NULL },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(lines); i++) {
		g_string_append_printf(state,
			"    - slot: %" G_GINT64_FORMAT "\n      at: %" G_GINT64_FORMAT
			"\n      outcome: %s\n", nine_on(lines[i].day),
			nine_on(lines[i].day) + 3, lines[i].outcome);

		if (lines[i].task != NULL)
			g_string_append_printf(state, "      task: %s\n", lines[i].task);
		else
			g_string_append(state, "      detail: \"why it went so\"\n");
	}

	/* a Run now on the 12th at 14:05 */
	g_string_append_printf(state,
		"    - slot: 0\n      at: %" G_GINT64_FORMAT "\n"
		"      outcome: started\n      by_hand: true\n      task: t-hand\n",
		nine_on(12) + 5 * 3600 + 300);

	fixture_setup(&fixture, DAILY, state->str);
	g_hash_table_insert(fixture.tasks, g_strdup("t-ok"),
	                    GINT_TO_POINTER(CLAWT_TASK_COMPLETED));
	g_hash_table_insert(fixture.tasks, g_strdup("t-fail"),
	                    GINT_TO_POINTER(CLAWT_TASK_FAILED));
	g_hash_table_insert(fixture.tasks, g_strdup("t-run"),
	                    GINT_TO_POINTER(CLAWT_TASK_RUNNING));
	g_hash_table_insert(fixture.tasks, g_strdup("t-queued"),
	                    GINT_TO_POINTER(CLAWT_TASK_PENDING));
	g_hash_table_insert(fixture.tasks, g_strdup("t-cancel"),
	                    GINT_TO_POINTER(CLAWT_TASK_CANCELLED));
	g_hash_table_insert(fixture.tasks, g_strdup("t-hand"),
	                    GINT_TO_POINTER(CLAWT_TASK_COMPLETED));
	clawt_routine_calendar_set_task_func(fixture.calendar, fake_task,
	                                     &fixture, NULL);
	clawt_routine_calendar_set_agent_func(fixture.calendar, fake_agent_up,
	                                      &fixture, NULL);
	fixture.agents_up = TRUE;

	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to, now,
	                                          NULL, NULL, FALSE, FALSE,
	                                          &error);
	g_assert_no_error(error);
	reply = json_node_get_object(node);

	/* fourteen scheduled days and the Run now */
	g_assert_cmpint(json_object_get_int_member(reply, "count"), ==, 15);
	g_assert_cmpstr(text_of(reply, "time_zone"), ==, ZONE);

	for (i = 0; i < G_N_ELEMENTS(lines); i++) {
		g_autoptr(GDateTime) slot = g_date_time_new_from_unix_utc(
			nine_on(lines[i].day));
		g_autofree gchar *at = g_date_time_format(slot,
		                                          "%Y-%m-%dT%H:%M:%SZ");

		occurrence = find_occurrence(reply, "deals", at);
		g_assert_nonnull(occurrence);
		g_assert_cmpstr(text_of(occurrence, "status"), ==, lines[i].status);
		g_assert_cmpstr(text_of(occurrence, "reason"), ==, lines[i].reason);
		g_assert_cmpstr(text_of(occurrence, "task"), ==, lines[i].task);
		g_assert_cmpstr(text_of(occurrence, "kind"), ==, "scheduled");
		g_assert_cmpstr(text_of(occurrence, "agent"), ==, "hornbill");
		g_assert_cmpstr(text_of(occurrence, "title"), ==,
		                "Daily deal check");
		g_assert_cmpstr(text_of(occurrence, "repeat"), ==, "daily");
	}

	/* a failed task brings its reason, its state and its room */
	{
		g_autoptr(GDateTime) slot = g_date_time_new_from_unix_utc(nine_on(2));
		g_autofree gchar *at = g_date_time_format(slot,
		                                          "%Y-%m-%dT%H:%M:%SZ");

		occurrence = find_occurrence(reply, "deals", at);
		g_assert_cmpstr(text_of(occurrence, "detail"), ==,
		                "the agent said it could not");
		g_assert_cmpstr(text_of(occurrence, "task_state"), ==, "failed");
		g_assert_cmpstr(text_of(occurrence, "room"), ==, "room-of-t-fail");
		g_assert_nonnull(text_of(occurrence, "started_at"));
	}
	/* the 11th to the 14th have passed with nothing recorded */
	g_assert_cmpuint(count_in(reply, "occurrences", "reason", "no_record"), ==,
	                 4);

	/* the Run now, off the schedule, at the time it started */
	{
		g_autoptr(GDateTime) started = g_date_time_new_from_unix_utc(
			nine_on(12) + 5 * 3600 + 300);
		g_autofree gchar *at = g_date_time_format(started,
		                                          "%Y-%m-%dT%H:%M:%SZ");

		occurrence = find_occurrence(reply, "deals", at);
		g_assert_nonnull(occurrence);
		g_assert_cmpstr(text_of(occurrence, "kind"), ==, "manual");
		g_assert_true(json_object_get_boolean_member(occurrence, "by_hand"));
		g_assert_cmpstr(text_of(occurrence, "status"), ==, "ok");
	}

	/*
	 * With the agent stopped, an open run -- pending, or running as the
	 * daemon marks a routine's task before delivering it -- is held in
	 * its mailbox: skipped, queued, and still its task's state.
	 */
	g_clear_pointer(&node, json_node_unref);
	fixture.agents_up = FALSE;
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to, now,
	                                          NULL, NULL, FALSE, FALSE,
	                                          &error);
	g_assert_no_error(error);
	{
		gint days[] = { 3, 4 };
		const gchar *states[] = { "running", "pending" };
		gsize d;

		for (d = 0; d < G_N_ELEMENTS(days); d++) {
			g_autoptr(GDateTime) slot = g_date_time_new_from_unix_utc(
				nine_on(days[d]));
			g_autofree gchar *at = g_date_time_format(slot,
			                                          "%Y-%m-%dT%H:%M:%SZ");

			occurrence = find_occurrence(json_node_get_object(node), "deals",
			                             at);
			g_assert_cmpstr(text_of(occurrence, "status"), ==, "skipped");
			g_assert_cmpstr(text_of(occurrence, "reason"), ==,
			                "agent_stopped");
			g_assert_true(json_object_get_boolean_member(occurrence,
			                                             "queued"));
			g_assert_cmpstr(text_of(occurrence, "task_state"), ==,
			                states[d]);
		}

		/* a finished run is finished whoever is up now */
		occurrence = find_occurrence(json_node_get_object(node), "deals",
			"2026-11-01T14:00:00Z");
		g_assert_cmpstr(text_of(occurrence, "status"), ==, "ok");
	}

	fixture_teardown(&fixture);
}

/*
 * Future times are upcoming, a time just passed with nothing recorded
 * is due (the tick may not have come yet), and the jitter window is
 * [at, at + jitter_seconds].
 */
static void
test_upcoming_due_and_jitter(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GDateTime) from = local(2026, 11, 24, 0, 0);
	g_autoptr(GDateTime) to = local(2026, 11, 25, 0, 0);
	g_autoptr(GDateTime) now = local(2026, 11, 24, 9, 0);
	g_autoptr(GDateTime) just_after = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *reply;
	JsonObject *occurrence;

	fixture_setup(&fixture,
		"agents:\n  - id: hornbill\n"
		"routines:\n"
		"  - id: spread\n    agent: hornbill\n    instructions: \"x\"\n"
		"    schedule: custom\n    cron: \"0 9,17 * * *\"\n"
		"    jitter_seconds: 300\n", NULL);

	just_after = g_date_time_add_seconds(now, 30);
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to,
	                                          just_after, NULL, NULL, FALSE,
	                                          FALSE, &error);
	g_assert_no_error(error);
	reply = json_node_get_object(node);

	occurrence = find_occurrence(reply, "spread", "2026-11-24T14:00:00Z");
	g_assert_nonnull(occurrence);
	g_assert_cmpstr(text_of(occurrence, "status"), ==, "due");
	g_assert_cmpstr(text_of(occurrence, "reason"), ==, "next_tick");
	g_assert_cmpstr(text_of(occurrence, "earliest"), ==,
	                "2026-11-24T14:00:00Z");
	g_assert_cmpstr(text_of(occurrence, "latest"), ==,
	                "2026-11-24T14:05:00Z");

	occurrence = find_occurrence(reply, "spread", "2026-11-24T22:00:00Z");
	g_assert_cmpstr(text_of(occurrence, "status"), ==, "upcoming");

	fixture_teardown(&fixture);
}

/* ── Bounds and filters ──────────────────────────────────────────── */

static void
test_bounds(void)
{
	Fixture fixture = { 0 };
	/* the 3rd, not the 1st: the 1st has 25 hours in Detroit */
	g_autoptr(GDateTime) from = local(2026, 11, 3, 0, 0);
	g_autoptr(GDateTime) far = local(2027, 1, 5, 0, 0);
	g_autoptr(GDateTime) day = local(2026, 11, 4, 0, 0);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;

	fixture_setup(&fixture,
		"agents:\n  - id: a\n"
		"routines:\n"
		"  - id: busy\n    agent: a\n    instructions: \"x\"\n"
		"    schedule: custom\n    cron: \"*/5 * * * *\"\n"
		"  - id: calm\n    agent: a\n    instructions: \"x\"\n"
		"    schedule: daily\n    at: \"09:00\"\n", NULL);

	/* longer than daemon.calendar_max_days (62) */
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, far,
	                                          from, NULL, NULL, FALSE, FALSE,
	                                          &error);
	g_assert_null(node);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "daemon.calendar_max_days"));
	g_clear_error(&error);

	/* backwards */
	node = clawt_routine_calendar_occurrences(fixture.calendar, day, from,
	                                          from, NULL, NULL, FALSE, FALSE,
	                                          &error);
	g_assert_null(node);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* more occurrences than allowed names the routine holding most */
	clawt_routine_calendar_set_limits(fixture.calendar, 0, 100);
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, day,
	                                          from, NULL, NULL, FALSE, FALSE,
	                                          &error);
	g_assert_null(node);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "'busy'"));
	g_assert_nonnull(strstr(error->message,
	                        "daemon.calendar_max_occurrences"));
	g_clear_error(&error);

	/* within it: 288 + 1 */
	clawt_routine_calendar_set_limits(fixture.calendar, 0, 0);
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, day,
	                                          from, NULL, NULL, FALSE, FALSE,
	                                          &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node),
	                                           "count"), ==, 289);

	fixture_teardown(&fixture);
}

static const gchar FLEET[] =
	"teams:\n  - id: forge\n"
	"agents:\n"
	"  - id: hornbill\n"
	"  - id: oxpecker\n    team: forge\n"
	"  - id: matrixer\n"
	"    libreclaw:\n"
	"      cron:\n"
	"        - name: morning-ping\n"
	"          schedule: \"0 7 * * *\"\n"
	"          payload_kind: ai_prompt\n"
	"          prompt: \"Good morning.\"\n"
	"        - name: heartbeat\n"
	"          interval_seconds: 300\n"
	"          command: \"true\"\n"
	"        - name: quiet\n"
	"          schedule: \"0 8 * * *\"\n"
	"          enabled: false\n"
	"          command: \"true\"\n"
	"routines:\n"
	"  - id: deals\n    agent: hornbill\n    instructions: \"x\"\n"
	"    schedule: daily\n    at: \"09:00\"\n"
	"  - id: review\n    agent: oxpecker\n    instructions: \"x\"\n"
	"    schedule: weekdays\n    at: \"10:30\"\n"
	"  - id: rotate\n    agent: oxpecker\n    instructions: \"x\"\n"
	"    schedule: weekly\n    weekday: friday\n    at: \"16:00\"\n"
	"  - id: parked\n    agent: hornbill\n    instructions: \"x\"\n"
	"    schedule: daily\n    at: \"11:00\"\n    enabled: false\n"
	"  - id: by-hand\n    agent: hornbill\n    instructions: \"x\"\n"
	"    schedule: manual\n";

/*
 * Agent and team filters, disabled routines, and libreclaw crons: laid
 * out and labelled, the interval one listed as unscheduled, the past
 * ones unknown because clawtilla does not run them.
 */
static void
test_filters_and_libreclaw_crons(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GDateTime) from = local(2026, 11, 23, 0, 0);
	g_autoptr(GDateTime) to = local(2026, 11, 30, 0, 0);
	g_autoptr(GDateTime) now = local(2026, 11, 25, 12, 0);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *reply;
	JsonObject *occurrence;
	JsonArray *unscheduled;

	fixture_setup(&fixture, FLEET, NULL);

	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to, now,
	                                          NULL, NULL, FALSE, FALSE,
	                                          &error);
	g_assert_no_error(error);
	reply = json_node_get_object(node);

	/* 7 deals, 5 review, 1 rotate, 7 morning-ping; nothing parked or manual */
	g_assert_cmpuint(count_in(reply, "occurrences", "routine", "deals"), ==, 7);
	g_assert_cmpuint(count_in(reply, "occurrences", "routine", "review"), ==,
	                 5);
	g_assert_cmpuint(count_in(reply, "occurrences", "routine", "rotate"), ==,
	                 1);
	g_assert_cmpuint(count_in(reply, "occurrences", "routine", "parked"), ==,
	                 0);
	g_assert_cmpuint(count_in(reply, "occurrences", "source",
	                          CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON), ==, 7);
	g_assert_cmpuint(count_in(reply, "occurrences", "routine", "quiet"), ==, 0);
	/* the manual routine is in the index, with nothing to lay out */
	g_assert_cmpuint(count_in(reply, "routines", "id", "by-hand"), ==, 1);

	occurrence = find_occurrence(reply, "morning-ping", "2026-11-23T12:00:00Z");
	g_assert_nonnull(occurrence);
	g_assert_cmpstr(text_of(occurrence, "status"), ==, "unknown");
	g_assert_cmpstr(text_of(occurrence, "reason"), ==, "not_tracked");
	g_assert_cmpstr(text_of(occurrence, "agent"), ==, "matrixer");
	occurrence = find_occurrence(reply, "morning-ping", "2026-11-28T12:00:00Z");
	g_assert_cmpstr(text_of(occurrence, "status"), ==, "upcoming");

	unscheduled = json_object_get_array_member(reply, "unscheduled");
	g_assert_cmpuint(json_array_get_length(unscheduled), ==, 1);
	g_assert_cmpstr(text_of(json_array_get_object_element(unscheduled, 0),
	                        "id"), ==, "heartbeat");
	g_assert_cmpint(json_object_get_int_member(
		json_array_get_object_element(unscheduled, 0), "interval_seconds"),
		==, 300);

	/* team forge is oxpecker alone */
	g_clear_pointer(&node, json_node_unref);
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to, now,
	                                          NULL, "forge", FALSE, FALSE,
	                                          &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node),
	                                           "count"), ==, 6);

	/* one agent; with the disabled ones */
	g_clear_pointer(&node, json_node_unref);
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to, now,
	                                          "hornbill", NULL, TRUE, FALSE,
	                                          &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node),
	                                           "count"), ==, 14);

	g_clear_pointer(&node, json_node_unref);
	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to, now,
	                                          "matrixer", NULL, TRUE, FALSE,
	                                          &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node),
	                                           "count"), ==, 14);

	fixture_teardown(&fixture);
}

/* ── Upcoming == next_run ────────────────────────────────────────── */

/*
 * The panel's first run of every routine is the list's next_run, at the
 * same moment, and the panel is soonest first.
 */
static void
test_upcoming_matches_next_run(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GDateTime) now = local(2026, 11, 27, 9, 17);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	JsonArray *runs;
	gint64 previous = 0;
	guint i;

	fixture_setup(&fixture, FLEET, NULL);

	node = clawt_routine_calendar_upcoming(fixture.calendar, now, 50, NULL,
	                                       NULL, &error);
	g_assert_no_error(error);
	runs = json_object_get_array_member(json_node_get_object(node), "runs");
	g_assert_cmpuint(json_array_get_length(runs), ==, 50);

	for (i = 0; i < json_array_get_length(runs); i++) {
		JsonObject *run = json_array_get_object_element(runs, i);
		const gchar *routine = text_of(run, "routine");
		g_autoptr(GDateTime) at = g_date_time_new_from_iso8601(
			text_of(run, "at"), NULL);

		g_assert_cmpstr(text_of(run, "status"), ==, "upcoming");
		g_assert_cmpint(g_date_time_to_unix(at), >=, previous);
		g_assert_cmpint(g_date_time_compare(at, now), >, 0);
		previous = g_date_time_to_unix(at);

		/* off and by-hand routines have nothing coming */
		g_assert_cmpstr(routine, !=, "parked");
		g_assert_cmpstr(routine, !=, "by-hand");

		if (g_strcmp0(text_of(run, "source"),
		              CLAWT_CALENDAR_SOURCE_ROUTINE) == 0 &&
		    !g_hash_table_contains(seen, routine)) {
			g_autoptr(GDateTime) next =
				clawt_routine_runner_next_run_after(fixture.runner, routine,
				                                    now);

			g_assert_nonnull(next);
			g_assert_true(g_date_time_equal(next, at));
			g_hash_table_add(seen, (gpointer)routine);
		}
	}

	/* deals, review and rotate each had a first run to compare */
	g_assert_cmpuint(g_hash_table_size(seen), ==, 3);

	/* a limit past the bound is refused */
	g_clear_pointer(&node, json_node_unref);
	node = clawt_routine_calendar_upcoming(fixture.calendar, now,
	                                       CLAWT_CALENDAR_UPCOMING_MAX + 1,
	                                       NULL, NULL, &error);
	g_assert_null(node);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

	fixture_teardown(&fixture);
}

/* ── Collapse ────────────────────────────────────────────────────── */

static void
test_collapse_folds_a_busy_day(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GDateTime) from = local(2026, 11, 23, 0, 0);
	g_autoptr(GDateTime) to = local(2026, 11, 30, 0, 0);
	g_autoptr(GDateTime) now = local(2026, 11, 25, 12, 30);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *reply;
	JsonArray *groups;
	guint i;

	fixture_setup(&fixture,
		"agents:\n  - id: a\n"
		"routines:\n"
		"  - id: poll\n    agent: a\n    instructions: \"x\"\n"
		"    schedule: custom\n    cron: \"0 8-18 * * 1-5\"\n"
		"  - id: twice\n    agent: a\n    instructions: \"x\"\n"
		"    schedule: custom\n    cron: \"0 9,17 * * *\"\n", NULL);

	node = clawt_routine_calendar_occurrences(fixture.calendar, from, to, now,
	                                          NULL, NULL, FALSE, TRUE, &error);
	g_assert_no_error(error);
	reply = json_node_get_object(node);

	/* every run is still counted, folded or not */
	g_assert_cmpint(json_object_get_int_member(reply, "count"), ==,
	                5 * 11 + 7 * 2);

	/* twice a day stays as cards; hourly does not */
	g_assert_cmpuint(count_in(reply, "occurrences", "routine", "twice"), ==,
	                 14);
	g_assert_cmpuint(count_in(reply, "occurrences", "routine", "poll"), ==, 0);

	groups = json_object_get_array_member(reply, "groups");
	g_assert_cmpuint(json_array_get_length(groups), ==, 5);

	for (i = 0; i < json_array_get_length(groups); i++) {
		JsonObject *group = json_array_get_object_element(groups, i);
		JsonObject *statuses = json_object_get_object_member(group,
		                                                     "statuses");

		g_assert_cmpstr(text_of(group, "routine"), ==, "poll");
		g_assert_cmpint(json_object_get_int_member(group, "count"), ==, 11);
		g_assert_cmpint(json_object_get_int_member(group, "every_seconds"),
		                ==, 3600);
		g_assert_cmpstr(text_of(group, "label"), ==,
		                "every hour, 08:00-18:00");
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			group, "occurrences")), ==, 11);
		g_assert_nonnull(statuses);
	}

	/* Wednesday the 25th is half past and half to come */
	{
		JsonObject *wednesday = json_array_get_object_element(groups, 2);
		JsonObject *statuses = json_object_get_object_member(wednesday,
		                                                     "statuses");

		g_assert_cmpstr(text_of(wednesday, "day"), ==, "2026-11-25");
		g_assert_cmpint(json_object_get_int_member(statuses, "unknown"), ==,
		                5);
		g_assert_cmpint(json_object_get_int_member(statuses, "upcoming"), ==,
		                6);
		g_assert_cmpstr(text_of(wednesday, "status"), ==, "unknown");
	}

	fixture_teardown(&fixture);
}

/* ── iCalendar ───────────────────────────────────────────────────── */

/* Every UID in @ics, in order. */
static GPtrArray *
uids_of(
	const gchar *ics
){
	GPtrArray *uids = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) lines = g_strsplit(ics, "\r\n", -1);
	guint i;

	for (i = 0; lines[i] != NULL; i++)
		if (g_str_has_prefix(lines[i], "UID:"))
			g_ptr_array_add(uids, g_strdup(lines[i] + 4));

	return uids;
}

static gchar *
export(
	Fixture   *fixture,
	GDateTime *from,
	GDateTime *to,
	GDateTime *now
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;

	node = clawt_routine_calendar_occurrences(fixture->calendar, from, to, now,
	                                          NULL, NULL, FALSE, TRUE, &error);
	g_assert_no_error(error);

	return clawt_routine_calendar_to_ics(json_node_get_object(node), now);
}

static void
test_ics_folds_and_keeps_its_uids(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GDateTime) from = local(2026, 11, 23, 0, 0);
	g_autoptr(GDateTime) to = local(2026, 11, 30, 0, 0);
	g_autoptr(GDateTime) now = local(2026, 11, 25, 12, 0);
	g_autoptr(GDateTime) later = local(2026, 11, 26, 8, 0);
	g_autofree gchar *ics = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *unfolded = NULL;
	g_autoptr(GPtrArray) uids = NULL;
	g_autoptr(GPtrArray) uids_again = NULL;
	g_autoptr(GHashTable) unique = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GRegex) folds = NULL;
	g_auto(GStrv) physical = NULL;
	const gchar *p;
	guint events = 0;
	guint i;

	fixture_setup(&fixture,
		"agents:\n  - id: gnuisaince\n"
		"routines:\n"
		"  - id: rotate\n    agent: gnuisaince\n    instructions: \"x\"\n"
		"    description: \"Rotate the tokens; renew the leases, check the "
		"vault, and write it up \xe2\x80\x94 \xc3\xa9t\xc3\xa9 caf\xc3\xa9 "
		"\xe2\x9c\x93 \xe2\x9c\x93 \xe2\x9c\x93 \xe2\x9c\x93 for whoever reads "
		"it next\"\n"
		"    schedule: daily\n    at: \"06:15\"\n    jitter_seconds: 120\n"
		"  - id: poll\n    agent: gnuisaince\n    instructions: \"x\"\n"
		"    schedule: custom\n    cron: \"0 8-18 * * 1-5\"\n", NULL);

	ics = export(&fixture, from, to, now);

	/* CRLF everywhere: no bare LF, and the file ends with one */
	for (p = ics; *p != '\0'; p++)
		if (*p == '\n')
			g_assert_true(p > ics && p[-1] == '\r');
	g_assert_true(g_str_has_suffix(ics, "END:VCALENDAR\r\n"));
	g_assert_true(g_str_has_prefix(ics, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"));

	/* 75 octets a physical line, whole UTF-8 characters only */
	physical = g_strsplit(ics, "\r\n", -1);
	for (i = 0; physical[i] != NULL; i++) {
		g_assert_cmpuint(strlen(physical[i]), <=, 75);
		g_assert_true(g_utf8_validate(physical[i], -1, NULL));
		if (g_str_has_prefix(physical[i], "BEGIN:VEVENT"))
			events++;
	}

	/* the long title was folded and unfolds to itself, escaped */
	folds = g_regex_new("\r\n ", 0, 0, NULL);
	unfolded = g_regex_replace_literal(folds, ics, -1, 0, "", 0, NULL);
	g_assert_nonnull(strstr(unfolded,
		"SUMMARY:Rotate the tokens\\; renew the leases\\, check the vault\\, "
		"and write it up \xe2\x80\x94 \xc3\xa9t\xc3\xa9 caf\xc3\xa9 "
		"\xe2\x9c\x93 \xe2\x9c\x93 \xe2\x9c\x93 \xe2\x9c\x93 for whoever reads "
		"it next\r\n"));
	g_assert_null(strstr(ics,
		"SUMMARY:Rotate the tokens\\; renew the leases\\, check the vault\\, "
		"and write it up \xe2\x80\x94 \xc3\xa9t\xc3\xa9 caf\xc3\xa9 "
		"\xe2\x9c\x93 \xe2\x9c\x93 \xe2\x9c\x93 \xe2\x9c\x93 for whoever reads "
		"it next\r\n"));

	/* UTC start, the jitter as the event's end */
	g_assert_nonnull(strstr(unfolded, "DTSTART:20261123T111500Z\r\n"
	                                  "DTEND:20261123T111700Z\r\n"));
	g_assert_nonnull(strstr(unfolded, "DESCRIPTION:Agent: gnuisaince\\n"
	                                  "Status: "));

	/* grouped runs are exported one by one: 7 rotate + 55 poll */
	g_assert_cmpuint(events, ==, 7 + 55);

	/* one UID an event, unique, and the same on a later export */
	uids = uids_of(unfolded);
	g_assert_cmpuint(uids->len, ==, events);
	for (i = 0; i < uids->len; i++)
		g_assert_true(g_hash_table_add(unique, g_ptr_array_index(uids, i)));

	again = export(&fixture, from, to, later);
	{
		g_autofree gchar *unfolded_again =
			g_regex_replace_literal(folds, again, -1, 0, "", 0, NULL);

		uids_again = uids_of(unfolded_again);
	}
	g_assert_cmpuint(uids_again->len, ==, uids->len);
	for (i = 0; i < uids_again->len; i++)
		g_assert_true(g_hash_table_contains(unique,
		                                    g_ptr_array_index(uids_again, i)));

	fixture_teardown(&fixture);
}

/* ── Over the daemon's own frames ────────────────────────────────── */

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(
	DaemonFixture *fixture,
	const gchar   *extra
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-calendar-d-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, extra);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_teardown(
	DaemonFixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);
	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_clear_pointer(&fixture->context, g_main_context_unref);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

static JsonNode *
request(
	DaemonFixture *fixture,
	const gchar   *kind,
	JsonNode      *payload
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

	if (payload != NULL)
		clawt_ipc_frame_set_payload(frame, payload);

	return clawt_daemon_handle_request(fixture->daemon, frame);
}

static JsonObject *
reply_payload(
	JsonNode *reply
){
	g_assert_nonnull(reply);
	g_assert_false(clawt_ipc_frame_is_error(reply));

	return clawt_ipc_frame_get_payload(reply);
}

static const gchar DAEMON_FLEET[] =
	"agents:\n"
	"  - id: watcher\n"
	"    model:\n      provider: claude-code\n"
	"    runtime:\n      autostart: false\n"
	"routines:\n"
	"  - id: brief\n    agent: watcher\n    instructions: \"Summarise.\"\n"
	"    schedule: manual\n"
	"  - id: deals\n    agent: watcher\n    instructions: \"Deals.\"\n"
	"    schedule: daily\n    at: \"09:00\"\n"
	"  - id: sweep\n    agent: watcher\n    instructions: \"Sweep.\"\n"
	"    schedule: custom\n    cron: \"*/20 * * * *\"\n"
	"  - id: weekly\n    agent: watcher\n    instructions: \"Weekly.\"\n"
	"    schedule: weekly\n    weekday: tuesday\n    at: \"07:45\"\n";

/*
 * routine.upcoming's first run of each routine is routine.list's
 * next_run.  Asked within one minute; a minute boundary between the two
 * frames moves both answers, so the pair is asked again.
 */
static void
test_frames_upcoming_matches_list(void)
{
	DaemonFixture fixture = { 0 };
	guint attempt;
	gboolean compared = FALSE;

	daemon_setup(&fixture, DAEMON_FLEET);

	for (attempt = 0; attempt < 3 && !compared; attempt++) {
		gint64 minute = g_get_real_time() / G_USEC_PER_SEC / 60;
		g_autoptr(JsonNode) list = request(&fixture, "routine.list", NULL);
		g_autoptr(JsonNode) upcoming = request(&fixture, "routine.upcoming",
			clawt_ipc_payload_new("limit", "200", NULL));
		g_autoptr(GHashTable) firsts = g_hash_table_new(g_str_hash,
		                                                g_str_equal);
		JsonArray *routines;
		JsonArray *runs;
		gboolean compared_one = FALSE;
		guint i;

		if (g_get_real_time() / G_USEC_PER_SEC / 60 != minute)
			continue;

		routines = json_object_get_array_member(reply_payload(list),
		                                        "routines");
		runs = json_object_get_array_member(reply_payload(upcoming), "runs");
		g_assert_nonnull(text_of(reply_payload(upcoming), "time_zone"));

		for (i = 0; i < json_array_get_length(runs); i++) {
			JsonObject *run = json_array_get_object_element(runs, i);

			if (!g_hash_table_contains(firsts, text_of(run, "routine")))
				g_hash_table_insert(firsts, (gpointer)text_of(run, "routine"),
				                    (gpointer)text_of(run, "at"));
		}

		for (i = 0; i < json_array_get_length(routines); i++) {
			JsonObject *routine = json_array_get_object_element(routines, i);
			const gchar *id = text_of(routine, "id");
			const gchar *next = text_of(routine, "next_run");
			const gchar *first = g_hash_table_lookup(firsts, id);
			g_autoptr(GDateTime) a = NULL;
			g_autoptr(GDateTime) b = NULL;

			if (next == NULL) {
				/* the manual routine: nothing in either */
				g_assert_cmpstr(id, ==, "brief");
				g_assert_null(first);
				continue;
			}

			a = g_date_time_new_from_iso8601(next, NULL);

			/*
			 * Not among the 200 soonest -- the sweep alone is 72 a day,
			 * so a weekly routine days away is past the cut.  Then its
			 * next run must be after the last one returned.
			 */
			if (first == NULL) {
				g_autoptr(GDateTime) last = g_date_time_new_from_iso8601(
					text_of(json_array_get_object_element(runs,
						json_array_get_length(runs) - 1), "at"), NULL);

				g_assert_cmpuint(json_array_get_length(runs), ==, 200);
				g_assert_cmpint(g_date_time_compare(a, last), >=, 0);
				continue;
			}

			b = g_date_time_new_from_iso8601(first, NULL);
			g_assert_true(g_date_time_equal(a, b));
			compared_one = TRUE;
		}

		/* the daily, at least, is inside the first 200 */
		g_assert_true(compared_one);

		compared = TRUE;
	}

	g_assert_true(compared);
	daemon_teardown(&fixture);
}

/*
 * A Run now on a stopped agent is on the calendar at once: off the
 * schedule, with its task, skipped because the agent is stopped and
 * still queued for when it starts.  Bad input is refused as such.
 */
static void
test_frames_occurrences(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) ran = NULL;
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(JsonNode) bad = NULL;
	g_autoptr(JsonNode) collapsed = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autoptr(GDateTime) from = g_date_time_add_hours(now, -1);
	g_autoptr(GDateTime) to = g_date_time_add_hours(now, 1);
	g_autofree gchar *from_text = g_date_time_format_iso8601(from);
	g_autofree gchar *to_text = g_date_time_format_iso8601(to);
	const gchar *task;
	JsonArray *list;
	JsonObject *found = NULL;
	guint i;

	daemon_setup(&fixture, DAEMON_FLEET);

	ran = request(&fixture, "routine.run",
	              clawt_ipc_payload_new("id", "brief", NULL));
	task = text_of(reply_payload(ran), "task");
	g_assert_nonnull(task);

	reply = request(&fixture, "routine.occurrences",
	                clawt_ipc_payload_new("from", from_text, "to", to_text,
	                                      "agent", "watcher", NULL));
	list = json_object_get_array_member(reply_payload(reply), "occurrences");

	for (i = 0; i < json_array_get_length(list); i++) {
		JsonObject *occurrence = json_array_get_object_element(list, i);

		if (g_strcmp0(text_of(occurrence, "routine"), "brief") == 0)
			found = occurrence;
	}

	g_assert_nonnull(found);
	g_assert_cmpstr(text_of(found, "kind"), ==, "manual");
	g_assert_cmpstr(text_of(found, "task"), ==, task);
	g_assert_cmpstr(text_of(found, "status"), ==, "skipped");
	g_assert_cmpstr(text_of(found, "reason"), ==, "agent_stopped");
	g_assert_true(json_object_get_boolean_member(found, "queued"));
	g_assert_true(json_object_get_boolean_member(found, "by_hand"));

	/* a date for a time, and a window past the bound */
	bad = request(&fixture, "routine.occurrences",
	              clawt_ipc_payload_new("from", "next tuesday", NULL));
	g_assert_true(clawt_ipc_frame_is_error(bad));
	g_clear_pointer(&bad, json_node_unref);
	bad = request(&fixture, "routine.occurrences",
	              clawt_ipc_payload_new("from", "2026-01-01",
	                                    "to", "2026-06-01", NULL));
	g_assert_true(clawt_ipc_frame_is_error(bad));

	/* dates alone are accepted, and collapse groups the sweep */
	collapsed = request(&fixture, "routine.occurrences",
	                    clawt_ipc_payload_new("from", "2026-11-23",
	                                          "to", "2026-11-30",
	                                          "collapse", "true", NULL));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		reply_payload(collapsed), "groups")), ==, 7);
	g_assert_cmpstr(text_of(reply_payload(collapsed), "from"), ==,
	                "2026-11-23T05:00:00Z");

	daemon_teardown(&fixture);
}

int
main(
	int   argc,
	char *argv[]
){
	/* Before anything asks GLib for local time, so every answer is Detroit's. */
	g_setenv("TZ", ZONE, TRUE);
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/calendar/cron/hourly-dst", test_hourly_across_dst);
	g_test_add_func("/calendar/cron/daily-dst", test_daily_across_dst);
	g_test_add_func("/calendar/cron/dom-or-dow",
	                test_day_of_month_or_day_of_week);
	g_test_add_func("/calendar/cron/steps", test_steps);
	g_test_add_func("/calendar/cron/presets", test_weekdays_and_hourly_presets);
	g_test_add_func("/calendar/cron/window-edges", test_window_edges);
	g_test_add_func("/calendar/history/late-tick",
	                test_a_late_tick_keeps_what_it_missed);
	g_test_add_func("/calendar/history/run-now", test_run_now_is_history_too);
	g_test_add_func("/calendar/history/catch-up", test_catch_up_history);
	g_test_add_func("/calendar/join/past-runs", test_past_runs_are_joined);
	g_test_add_func("/calendar/join/due-and-jitter",
	                test_upcoming_due_and_jitter);
	g_test_add_func("/calendar/bounds", test_bounds);
	g_test_add_func("/calendar/filters-and-libreclaw",
	                test_filters_and_libreclaw_crons);
	g_test_add_func("/calendar/upcoming-is-next-run",
	                test_upcoming_matches_next_run);
	g_test_add_func("/calendar/collapse", test_collapse_folds_a_busy_day);
	g_test_add_func("/calendar/ics", test_ics_folds_and_keeps_its_uids);
	g_test_add_func("/calendar/frames/upcoming-is-next-run",
	                test_frames_upcoming_matches_list);
	g_test_add_func("/calendar/frames/occurrences", test_frames_occurrences);

	return g_test_run();
}
