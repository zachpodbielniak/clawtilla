/*
 * test-lrg-models-schedule.c - The Schedule page's rows
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no display.  A canned routine.occurrences reply
 * -- one of every status, a collapsed group, a libreclaw cron -- and
 * the two bound refusals.  The page draws what these rows say.
 */

#include <clawtilla.h>

#include "../clients/lrg/lrg-schedule-model.c"

static JsonNode *
parse(const gchar *text)
{
    g_autoptr(GError) error = NULL;
    JsonNode *node = json_from_string(text, &error);

    g_assert_no_error(error);
    g_assert_nonnull(node);
    return node;
}

static ClawtLrgSchedRow *
find_title(GPtrArray *rows, const gchar *title)
{
    guint i;

    for (i = 0; i < rows->len; i++) {
        ClawtLrgSchedRow *row = g_ptr_array_index(rows, i);

        if (g_strcmp0(row->title, title) == 0)
            return row;
    }

    return NULL;
}

/*
 * 14:00Z is 09:00 in America/Detroit in November.  A client that
 * formatted in UTC would show 14:00, which is the wrong hour on the
 * grid.
 */
static const gchar *CANNED =
    "{"
    " \"time_zone\": \"America/Detroit\","
    " \"from\": \"2026-11-24T05:00:00Z\","
    " \"to\": \"2026-11-25T05:00:00Z\","
    " \"occurrences\": ["
    "  {\"source\": \"routine\", \"routine\": \"morning\","
    "   \"agent\": \"oxpecker\", \"title\": \"Morning\","
    "   \"at\": \"2026-11-24T14:00:00Z\","
    "   \"earliest\": \"2026-11-24T14:00:00Z\","
    "   \"latest\": \"2026-11-24T14:05:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"ok\", \"task\": \"task-ok\", \"room\": \"room-ok\"},"
    "  {\"source\": \"routine\", \"routine\": \"broken\","
    "   \"agent\": \"serval\", \"title\": \"Broken\","
    "   \"at\": \"2026-11-24T15:00:00Z\","
    "   \"earliest\": \"2026-11-24T15:00:00Z\","
    "   \"latest\": \"2026-11-24T15:00:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"failed\", \"detail\": \"computer would not start\"},"
    "  {\"source\": \"routine\", \"routine\": \"asleep\","
    "   \"agent\": \"serval\", \"title\": \"Asleep\","
    "   \"at\": \"2026-11-24T16:00:00Z\","
    "   \"earliest\": \"2026-11-24T16:00:00Z\","
    "   \"latest\": \"2026-11-24T16:00:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"skipped\", \"reason\": \"daemon_down\"},"
    "  {\"source\": \"routine\", \"routine\": \"live\","
    "   \"agent\": \"serval\", \"title\": \"Live\","
    "   \"at\": \"2026-11-24T17:00:00Z\","
    "   \"earliest\": \"2026-11-24T17:00:00Z\","
    "   \"latest\": \"2026-11-24T17:00:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"running\", \"task\": \"task-run\"},"
    "  {\"source\": \"routine\", \"routine\": \"now\","
    "   \"agent\": \"serval\", \"title\": \"Due now\","
    "   \"at\": \"2026-11-24T18:00:00Z\","
    "   \"earliest\": \"2026-11-24T18:00:00Z\","
    "   \"latest\": \"2026-11-24T18:00:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"due\", \"reason\": \"next_tick\"},"
    "  {\"source\": \"routine\", \"routine\": \"later\","
    "   \"agent\": \"serval\", \"title\": \"Later\","
    "   \"at\": \"2026-11-24T19:00:00Z\","
    "   \"earliest\": \"2026-11-24T19:00:00Z\","
    "   \"latest\": \"2026-11-24T19:00:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"upcoming\"},"
    "  {\"source\": \"libreclaw-cron\", \"routine\": \"matrix-morning\","
    "   \"agent\": \"hornbill\", \"title\": \"Matrix morning\","
    "   \"at\": \"2026-11-24T13:00:00Z\","
    "   \"earliest\": \"2026-11-24T13:00:00Z\","
    "   \"latest\": \"2026-11-24T13:00:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"unknown\", \"reason\": \"not_tracked\"},"
    "  {\"source\": \"routine\", \"routine\": \"held\","
    "   \"agent\": \"caracal\", \"title\": \"Held\","
    "   \"at\": \"2026-11-24T12:00:00Z\","
    "   \"earliest\": \"2026-11-24T12:00:00Z\","
    "   \"latest\": \"2026-11-24T12:00:00Z\","
    "   \"kind\": \"scheduled\", \"repeat\": \"daily\","
    "   \"status\": \"skipped\", \"reason\": \"agent_stopped\","
    "   \"queued\": true}"
    " ],"
    " \"groups\": ["
    "  {\"source\": \"routine\", \"routine\": \"pulse\","
    "   \"agent\": \"serval\", \"title\": \"Pulse\","
    "   \"day\": \"2026-11-24\", \"first\": \"2026-11-24T13:00:00Z\","
    "   \"last\": \"2026-11-24T21:00:00Z\", \"count\": 3,"
    "   \"label\": \"every hour, 08:00-16:00\","
    "   \"status\": \"failed\","
    "   \"statuses\": {\"ok\": 2, \"failed\": 1},"
    "   \"occurrences\": ["
    "    {\"source\": \"routine\", \"routine\": \"pulse\","
    "     \"agent\": \"serval\", \"title\": \"Pulse\","
    "     \"at\": \"2026-11-24T13:00:00Z\","
    "     \"earliest\": \"2026-11-24T13:00:00Z\","
    "     \"latest\": \"2026-11-24T13:00:00Z\","
    "     \"kind\": \"scheduled\", \"repeat\": \"hourly\","
    "     \"status\": \"ok\"},"
    "    {\"source\": \"routine\", \"routine\": \"pulse\","
    "     \"agent\": \"serval\", \"title\": \"Pulse\","
    "     \"at\": \"2026-11-24T14:00:00Z\","
    "     \"earliest\": \"2026-11-24T14:00:00Z\","
    "     \"latest\": \"2026-11-24T14:00:00Z\","
    "     \"kind\": \"scheduled\", \"repeat\": \"hourly\","
    "     \"status\": \"ok\"},"
    "    {\"source\": \"routine\", \"routine\": \"pulse\","
    "     \"agent\": \"serval\", \"title\": \"Pulse\","
    "     \"at\": \"2026-11-24T15:00:00Z\","
    "     \"earliest\": \"2026-11-24T15:00:00Z\","
    "     \"latest\": \"2026-11-24T15:00:00Z\","
    "     \"kind\": \"scheduled\", \"repeat\": \"hourly\","
    "     \"status\": \"failed\", \"task\": \"task-pulse\"}"
    "   ]}"
    " ],"
    " \"unscheduled\": ["
    "  {\"source\": \"libreclaw-cron\", \"id\": \"tick\","
    "   \"agent\": \"hornbill\", \"title\": \"Tick\","
    "   \"interval_seconds\": 30}"
    " ]"
    "}";

static void
test_every_status_and_the_group(void)
{
    g_autoptr(JsonNode) reply = parse(CANNED);
    g_autoptr(GPtrArray) rows = clawt_lrg_schedule_rows(reply);
    g_autoptr(GPtrArray) aside = clawt_lrg_schedule_unscheduled(reply);
    g_autofree gchar *zone = clawt_lrg_schedule_zone_line(reply);
    ClawtLrgSchedRow *morning;
    ClawtLrgSchedRow *broken;
    ClawtLrgSchedRow *asleep;
    ClawtLrgSchedRow *live;
    ClawtLrgSchedRow *due;
    ClawtLrgSchedRow *later;
    ClawtLrgSchedRow *cron;
    ClawtLrgSchedRow *held;
    ClawtLrgSchedRow *pulse;
    ClawtLrgSchedRow *child;

    /* 8 occurrences plus one group.  The group's runs are not also cards. */
    g_assert_cmpuint(rows->len, ==, 9);
    g_assert_cmpstr(zone, ==, "Times in America/Detroit");

    morning = find_title(rows, "Morning");
    g_assert_nonnull(morning);
    g_assert_cmpstr(morning->agent, ==, "oxpecker");
    g_assert_cmpstr(morning->when, ==, "09:00");
    g_assert_cmpstr(morning->day, ==, "2026-11-24");
    g_assert_cmpstr(morning->status, ==, "ok");
    g_assert_cmpstr(morning->status_label, ==, "ok");
    g_assert_cmpstr(morning->tone, ==, "success");
    g_assert_cmpstr(morning->task, ==, "task-ok");
    g_assert_cmpstr(morning->room, ==, "room-ok");
    g_assert_cmpint(morning->action, ==, CLAWT_LRG_SCHED_ACT_TASK);
    g_assert_nonnull(strstr(morning->tooltip, "Window 09:00-09:05"));
    g_assert_null(morning->source_note);

    broken = find_title(rows, "Broken");
    g_assert_cmpstr(broken->status, ==, "failed");
    g_assert_cmpstr(broken->tone, ==, "error");
    g_assert_cmpint(broken->action, ==, CLAWT_LRG_SCHED_ACT_DETAIL);
    g_assert_nonnull(strstr(broken->tooltip, "computer would not start"));

    asleep = find_title(rows, "Asleep");
    g_assert_cmpstr(asleep->status, ==, "skipped");
    g_assert_cmpstr(asleep->tone, ==, "neutral");

    live = find_title(rows, "Live");
    g_assert_cmpstr(live->status, ==, "running");
    g_assert_cmpstr(live->tone, ==, "accent");
    g_assert_cmpint(live->action, ==, CLAWT_LRG_SCHED_ACT_TASK);

    due = find_title(rows, "Due now");
    g_assert_cmpstr(due->status, ==, "due");
    g_assert_cmpstr(due->tone, ==, "warning");

    later = find_title(rows, "Later");
    g_assert_cmpstr(later->status, ==, "upcoming");
    g_assert_cmpstr(later->tone, ==, "dim");
    g_assert_cmpint(later->action, ==, CLAWT_LRG_SCHED_ACT_ROUTINE);
    g_assert_cmpstr(later->when, ==, "14:00");

    cron = find_title(rows, "Matrix morning");
    g_assert_nonnull(cron);
    g_assert_cmpstr(cron->source_note, ==, "libreclaw cron");
    g_assert_cmpstr(cron->status, ==, "unknown");
    g_assert_cmpstr(cron->status_label, ==,
                    "unknown, not tracked by clawtilla");
    g_assert_cmpint(cron->action, ==, CLAWT_LRG_SCHED_ACT_DETAIL);
    g_assert_cmpstr(cron->when, ==, "08:00");

    held = find_title(rows, "Held");
    g_assert_nonnull(strstr(held->tooltip, "Waiting for caracal to start"));

    pulse = find_title(rows, "Pulse");
    g_assert_true(pulse->group);
    g_assert_cmpuint(pulse->count, ==, 3);
    g_assert_cmpuint(pulse->children->len, ==, 3);
    g_assert_cmpstr(pulse->summary, ==, "every hour, 08:00-16:00");
    g_assert_cmpstr(pulse->status, ==, "failed");
    g_assert_cmpstr(pulse->counts, ==, "ok 2, failed 1");
    g_assert_cmpstr(pulse->when, ==, "08:00-16:00");
    g_assert_cmpint(pulse->action, ==, CLAWT_LRG_SCHED_ACT_EXPAND);
    child = g_ptr_array_index(pulse->children, 2);
    g_assert_cmpstr(child->status, ==, "failed");
    g_assert_cmpstr(child->task, ==, "task-pulse");
    g_assert_cmpint(child->action, ==, CLAWT_LRG_SCHED_ACT_TASK);

    /* soonest first: Held at 07:00, then the cron at 08:00 */
    g_assert_cmpstr(((ClawtLrgSchedRow *)g_ptr_array_index(rows, 0))->title,
                    ==, "Held");

    g_assert_cmpuint(aside->len, ==, 1);
    g_assert_cmpstr(((ClawtLrgSchedRow *)g_ptr_array_index(aside, 0))->title,
                    ==, "Tick");
    g_assert_nonnull(strstr(
        ((ClawtLrgSchedRow *)g_ptr_array_index(aside, 0))->tooltip, "30"));
}

static void
test_bounds_are_shown_whole(void)
{
    const gchar *days =
        "window is 90 days, longer than daemon.calendar_max_days (62)";
    const gchar *runs =
        "5000 occurrences is the limit; pulse holds most of them";
    g_autoptr(GError) too_long = g_error_new_literal(
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, days);
    g_autoptr(GError) too_many = g_error_new_literal(
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, runs);
    g_autofree gchar *long_text = clawt_lrg_schedule_error_text(too_long);
    g_autofree gchar *many_text = clawt_lrg_schedule_error_text(too_many);

    g_assert_cmpstr(long_text, ==, days);
    g_assert_nonnull(strstr(long_text, "62"));
    g_assert_cmpstr(many_text, ==, runs);
    g_assert_nonnull(strstr(many_text, "5000"));
    g_assert_nonnull(strstr(many_text, "pulse"));
    g_assert_null(clawt_lrg_schedule_error_text(NULL));
}

static void
test_window_and_collapse(void)
{
    g_autofree gchar *from = NULL;
    g_autofree gchar *to = NULL;
    g_autofree gchar *week_from = NULL;
    g_autofree gchar *week_to = NULL;
    g_autofree gchar *month_from = NULL;
    g_autofree gchar *month_to = NULL;
    g_autoptr(JsonNode) payload = NULL;
    JsonObject *body;
    gint year = 2026;
    gint month = 11;
    gint day = 24;

    g_assert_false(clawt_lrg_schedule_wants_collapse(CLAWT_LRG_SCHED_DAY));
    g_assert_false(clawt_lrg_schedule_wants_collapse(CLAWT_LRG_SCHED_WEEK));
    g_assert_true(clawt_lrg_schedule_wants_collapse(CLAWT_LRG_SCHED_MONTH));
    g_assert_cmpuint(clawt_lrg_schedule_view_count(), ==, 4);
    g_assert_cmpstr(clawt_lrg_schedule_view_label(CLAWT_LRG_SCHED_MONTH),
                    ==, "Month");

    clawt_lrg_schedule_range(CLAWT_LRG_SCHED_DAY, 2026, 11, 24, &from, &to);
    g_assert_cmpstr(from, ==, "2026-11-24");
    g_assert_cmpstr(to, ==, "2026-11-25");

    /* 2026-11-24 is a Tuesday; the week is Monday 23 through Monday 30. */
    clawt_lrg_schedule_range(CLAWT_LRG_SCHED_WEEK, 2026, 11, 24,
                             &week_from, &week_to);
    g_assert_cmpstr(week_from, ==, "2026-11-23");
    g_assert_cmpstr(week_to, ==, "2026-11-30");

    clawt_lrg_schedule_range(CLAWT_LRG_SCHED_MONTH, 2026, 11, 24,
                             &month_from, &month_to);
    g_assert_cmpstr(month_from, ==, "2026-10-26");
    g_assert_cmpstr(month_to, ==, "2026-12-07");

    clawt_lrg_schedule_move(CLAWT_LRG_SCHED_WEEK, -1, 2026, 11, 27,
                            &year, &month, &day);
    g_assert_cmpint(year, ==, 2026);
    g_assert_cmpint(month, ==, 11);
    g_assert_cmpint(day, ==, 17);

    clawt_lrg_schedule_move(CLAWT_LRG_SCHED_MONTH, 1, 2026, 11, 27,
                            &year, &month, &day);
    g_assert_cmpint(year, ==, 2026);
    g_assert_cmpint(month, ==, 12);
    g_assert_cmpint(day, ==, 17);

    clawt_lrg_schedule_move(CLAWT_LRG_SCHED_DAY, 0, 2026, 11, 27,
                            &year, &month, &day);
    g_assert_cmpint(day, ==, 27);

    payload = clawt_lrg_schedule_occurrences_payload(
        "2026-11-01", "2026-12-13", "", "forge", TRUE, TRUE);
    body = json_node_get_object(payload);
    g_assert_false(json_object_has_member(body, "agent"));
    g_assert_cmpstr(json_object_get_string_member(body, "team"), ==, "forge");
    g_assert_true(json_object_get_boolean_member(body, "collapse"));
    g_assert_true(json_object_get_boolean_member(body, "include_disabled"));

    g_clear_pointer(&payload, json_node_unref);
    payload = clawt_lrg_schedule_upcoming_payload("serval", NULL);
    body = json_node_get_object(payload);
    g_assert_cmpint(json_object_get_int_member(body, "limit"), ==, 10);
    g_assert_cmpstr(json_object_get_string_member(body, "agent"), ==, "serval");
    g_assert_false(json_object_has_member(body, "team"));
}

static void
test_upcoming_runs_are_rows(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"time_zone\": \"UTC\", \"runs\": ["
        " {\"source\": \"routine\", \"routine\": \"later\", \"agent\": \"serval\","
        "  \"title\": \"Later\", \"at\": \"2026-11-24T19:00:00Z\","
        "  \"earliest\": \"2026-11-24T19:00:00Z\","
        "  \"latest\": \"2026-11-24T19:00:00Z\","
        "  \"kind\": \"scheduled\", \"repeat\": \"daily\","
        "  \"status\": \"upcoming\"}]}");
    g_autoptr(GPtrArray) rows = clawt_lrg_schedule_rows(reply);
    ClawtLrgSchedRow *row = g_ptr_array_index(rows, 0);

    g_assert_cmpuint(rows->len, ==, 1);
    g_assert_cmpstr(row->when, ==, "19:00");
    g_assert_cmpstr(row->status, ==, "upcoming");
    g_assert_cmpint(row->action, ==, CLAWT_LRG_SCHED_ACT_ROUTINE);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg-schedule/rows", test_every_status_and_the_group);
    g_test_add_func("/lrg-schedule/bounds", test_bounds_are_shown_whole);
    g_test_add_func("/lrg-schedule/window", test_window_and_collapse);
    g_test_add_func("/lrg-schedule/upcoming", test_upcoming_runs_are_rows);

    return g_test_run();
}
