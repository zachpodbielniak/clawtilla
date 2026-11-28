/*
 * lrg-schedule-model.c - Rows the Schedule page draws
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * See clawt-lrg-schedule-model.h.  No libregnum and no GTK: the GTK
 * client compiles this file too, and tests/test-lrg-models-schedule.c
 * includes it.
 */

#include "clawt-lrg-schedule-model.h"

#include <string.h>

/* Monday-start week.  GDateTime's day-of-week is 1 for Monday. */
#define WEEK_START (1)

static const gchar *view_labels[] = {
    "Day",
    "Week",
    "Month",
    "Upcoming"
};

/* ── Dates ────────────────────────────────────────────────────────── */

/*
 * Noon UTC on a civil date.  Adding days at noon does not fall into a
 * DST gap, and the date we read back is the civil date we put in.
 */
static GDateTime *
civil(gint year, gint month, gint day)
{
    return g_date_time_new_utc(year, month, day, 12, 0, 0.0);
}

static void
civil_parts(GDateTime *when, gint *year, gint *month, gint *day)
{
    *year = g_date_time_get_year(when);
    *month = g_date_time_get_month(when);
    *day = g_date_time_get_day_of_month(when);
}

static void
add_days(gint *year, gint *month, gint *day, gint count)
{
    g_autoptr(GDateTime) base = civil(*year, *month, *day);
    g_autoptr(GDateTime) next = NULL;

    if (base == NULL)
        return;

    next = g_date_time_add_days(base, count);

    if (next != NULL)
        civil_parts(next, year, month, day);
}

static void
add_months(gint *year, gint *month, gint *day, gint count)
{
    g_autoptr(GDateTime) base = civil(*year, *month, *day);
    g_autoptr(GDateTime) next = NULL;

    if (base == NULL)
        return;

    next = g_date_time_add_months(base, count);

    if (next != NULL)
        civil_parts(next, year, month, day);
}

static gchar *
ymd(gint year, gint month, gint day)
{
    return g_strdup_printf("%04d-%02d-%02d", year, month, day);
}

/* The Monday of the week containing this date, as a civil date. */
static void
week_monday(gint *year, gint *month, gint *day)
{
    g_autoptr(GDateTime) base = civil(*year, *month, *day);
    gint dow;

    if (base == NULL)
        return;

    dow = g_date_time_get_day_of_week(base);
    add_days(year, month, day, WEEK_START - dow);
}

static GTimeZone *
zone_of(const gchar *name)
{
    GTimeZone *zone = NULL;

    if (name != NULL && name[0] != '\0')
        zone = g_time_zone_new_identifier(name);

    if (zone == NULL)
        zone = g_time_zone_new_local();

    return zone;
}

static gchar *
format_iso(const gchar *iso, const gchar *zone_name, const gchar *format)
{
    g_autoptr(GDateTime) parsed = NULL;
    g_autoptr(GTimeZone) zone = NULL;
    g_autoptr(GDateTime) local = NULL;

    if (iso == NULL)
        return g_strdup("");

    parsed = g_date_time_new_from_iso8601(iso, NULL);

    if (parsed == NULL)
        return g_strdup(iso);

    zone = zone_of(zone_name);
    local = g_date_time_to_timezone(parsed, zone);

    if (local == NULL)
        return g_strdup(iso);

    return g_date_time_format(local, format);
}

static gint64
unix_of(const gchar *iso)
{
    g_autoptr(GDateTime) parsed = NULL;

    if (iso == NULL)
        return 0;

    parsed = g_date_time_new_from_iso8601(iso, NULL);

    return (parsed != NULL) ? g_date_time_to_unix(parsed) : 0;
}

static gboolean
blank(const gchar *text)
{
    return text == NULL || text[0] == '\0';
}

/* ── Views and the window ─────────────────────────────────────────── */

guint
clawt_lrg_schedule_view_count(void)
{
    return G_N_ELEMENTS(view_labels);
}

const gchar *
clawt_lrg_schedule_view_label(ClawtLrgSchedView view)
{
    if ((guint)view >= G_N_ELEMENTS(view_labels))
        return view_labels[0];

    return view_labels[view];
}

gboolean
clawt_lrg_schedule_wants_collapse(ClawtLrgSchedView view)
{
    return view == CLAWT_LRG_SCHED_MONTH;
}

gchar *
clawt_lrg_schedule_error_text(const GError *error)
{
    if (error == NULL || error->message == NULL || error->message[0] == '\0')
        return NULL;

    /*
     * Copied, not rewritten.  The sentence names the bound (62 days,
     * 5000 occurrences) and, for the second, the routine that holds
     * most.  A shorter banner would be the silent cut the frame refuses.
     */
    return g_strdup(error->message);
}

void
clawt_lrg_schedule_today(const gchar *zone, gint *year, gint *month, gint *day)
{
    g_autoptr(GTimeZone) tz = zone_of(zone);
    g_autoptr(GDateTime) now = g_date_time_new_now(tz);

    if (now == NULL) {
        *year = 1970;
        *month = 1;
        *day = 1;
        return;
    }

    civil_parts(now, year, month, day);
}

void
clawt_lrg_schedule_move(ClawtLrgSchedView view, gint direction,
                        gint today_y, gint today_m, gint today_d,
                        gint *year, gint *month, gint *day)
{
    if (direction == 0) {
        *year = today_y;
        *month = today_m;
        *day = today_d;
        return;
    }

    if (view == CLAWT_LRG_SCHED_MONTH) {
        add_months(year, month, day, direction);
        return;
    }

    if (view == CLAWT_LRG_SCHED_WEEK)
        add_days(year, month, day, direction * 7);
    else
        add_days(year, month, day, direction);
}

void
clawt_lrg_schedule_range(ClawtLrgSchedView view, gint year, gint month,
                         gint day, gchar **from, gchar **to)
{
    gint end_y = year;
    gint end_m = month;
    gint end_d = day;

    if (view == CLAWT_LRG_SCHED_WEEK) {
        week_monday(&year, &month, &day);
        end_y = year;
        end_m = month;
        end_d = day;
        add_days(&end_y, &end_m, &end_d, 7);
    } else if (view == CLAWT_LRG_SCHED_MONTH) {
        /* the Monday on or before the 1st, then six weeks */
        year = year;
        month = month;
        day = 1;
        week_monday(&year, &month, &day);
        end_y = year;
        end_m = month;
        end_d = day;
        add_days(&end_y, &end_m, &end_d, 42);
    } else {
        add_days(&end_y, &end_m, &end_d, 1);
    }

    *from = ymd(year, month, day);
    *to = ymd(end_y, end_m, end_d);
}

static void
add_string(JsonBuilder *builder, const gchar *key, const gchar *value)
{
    if (blank(value))
        return;

    json_builder_set_member_name(builder, key);
    json_builder_add_string_value(builder, value);
}

JsonNode *
clawt_lrg_schedule_occurrences_payload(const gchar *from, const gchar *to,
                                       const gchar *agent, const gchar *team,
                                       gboolean include_disabled,
                                       gboolean collapse)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    JsonNode *root;

    json_builder_begin_object(builder);
    add_string(builder, "from", from);
    add_string(builder, "to", to);
    add_string(builder, "agent", agent);
    add_string(builder, "team", team);
    json_builder_set_member_name(builder, "include_disabled");
    json_builder_add_boolean_value(builder, include_disabled);
    json_builder_set_member_name(builder, "collapse");
    json_builder_add_boolean_value(builder, collapse);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);

    return root;
}

JsonNode *
clawt_lrg_schedule_upcoming_payload(const gchar *agent, const gchar *team)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    JsonNode *root;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "limit");
    json_builder_add_int_value(builder, 10);
    add_string(builder, "agent", agent);
    add_string(builder, "team", team);
    json_builder_end_object(builder);
    root = json_builder_get_root(builder);

    return root;
}

gchar *
clawt_lrg_schedule_zone_line(JsonNode *reply)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *zone = NULL;

    if (root != NULL)
        zone = clawt_ipc_payload_string(root, "time_zone");

    if (blank(zone))
        return g_strdup("Times in the daemon's zone");

    return g_strdup_printf("Times in %s", zone);
}

/* ── Cards ────────────────────────────────────────────────────────── */

static const gchar *
tone_of(const gchar *status)
{
    if (g_strcmp0(status, "ok") == 0)
        return "success";

    if (g_strcmp0(status, "failed") == 0)
        return "error";

    if (g_strcmp0(status, "due") == 0)
        return "warning";

    if (g_strcmp0(status, "running") == 0)
        return "accent";

    if (g_strcmp0(status, "skipped") == 0)
        return "neutral";

    return "dim";
}

static gchar *
status_words(JsonObject *object)
{
    const gchar *status = clawt_ipc_payload_string(object, "status");
    const gchar *reason = clawt_ipc_payload_string(object, "reason");

    if (status == NULL)
        status = "unknown";

    /*
     * A libreclaw cron's past runs are unknown because clawtilla does
     * not run them.  The nick alone would read as a record we lost.
     */
    if (g_strcmp0(status, "unknown") == 0 &&
        g_strcmp0(reason, "not_tracked") == 0)
        return g_strdup("unknown, not tracked by clawtilla");

    return g_strdup(status);
}

static gchar *
tooltip_of(JsonObject *object, const gchar *zone, gboolean group)
{
    GString *text = g_string_new(NULL);
    const gchar *earliest = clawt_ipc_payload_string(object, "earliest");
    const gchar *latest = clawt_ipc_payload_string(object, "latest");
    const gchar *reason = clawt_ipc_payload_string(object, "reason");
    const gchar *detail = clawt_ipc_payload_string(object, "detail");
    const gchar *agent = clawt_ipc_payload_string(object, "agent");
    gboolean queued = FALSE;

    if (!group && earliest != NULL && latest != NULL &&
        g_strcmp0(earliest, latest) != 0) {
        g_autofree gchar *from = format_iso(earliest, zone, "%H:%M");
        g_autofree gchar *to = format_iso(latest, zone, "%H:%M");

        g_string_append_printf(text, "Window %s-%s", from, to);
    }

    if (json_object_has_member(object, "queued") &&
        json_object_get_boolean_member(object, "queued"))
        queued = TRUE;

    if (queued) {
        if (text->len > 0)
            g_string_append(text, "\n");

        g_string_append_printf(text, "Waiting for %s to start",
                               agent != NULL ? agent : "the agent");
    } else if (reason != NULL) {
        if (text->len > 0)
            g_string_append(text, "\n");

        g_string_append(text, reason);
    }

    if (detail != NULL) {
        if (text->len > 0)
            g_string_append(text, "\n");

        g_string_append(text, detail);
    }

    if (g_strcmp0(clawt_ipc_payload_string(object, "kind"), "manual") == 0) {
        if (text->len > 0)
            g_string_append(text, "\n");

        g_string_append(text, "Run by hand");
    }

    if (text->len == 0)
        g_string_append(text, group
                        ? "Several runs of this routine, this day"
                        : "No further detail");

    return g_string_free(text, FALSE);
}

static gchar *
counts_of(JsonObject *object)
{
    JsonObject *statuses;
    GList *members;
    GList *link;
    GString *text;

    if (!json_object_has_member(object, "statuses") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "statuses")))
        return NULL;

    statuses = json_object_get_object_member(object, "statuses");
    members = json_object_get_members(statuses);
    text = g_string_new(NULL);

    for (link = members; link != NULL; link = link->next) {
        const gchar *key = link->data;

        g_string_append_printf(text, "%s%s %" G_GINT64_FORMAT,
                               text->len > 0 ? ", " : "", key,
                               json_object_get_int_member(statuses, key));
    }

    g_list_free(members);

    if (text->len == 0) {
        g_string_free(text, TRUE);
        return NULL;
    }

    return g_string_free(text, FALSE);
}

static ClawtLrgSchedAction
action_of(JsonObject *object, gboolean group, gboolean libreclaw)
{
    const gchar *status = clawt_ipc_payload_string(object, "status");
    const gchar *task = clawt_ipc_payload_string(object, "task");

    if (group)
        return CLAWT_LRG_SCHED_ACT_EXPAND;

    if (task != NULL && g_strcmp0(status, "upcoming") != 0)
        return CLAWT_LRG_SCHED_ACT_TASK;

    if (g_strcmp0(status, "upcoming") == 0 && !libreclaw &&
        g_strcmp0(clawt_ipc_payload_string(object, "source"),
                  CLAWT_CALENDAR_SOURCE_ROUTINE) == 0)
        return CLAWT_LRG_SCHED_ACT_ROUTINE;

    return CLAWT_LRG_SCHED_ACT_DETAIL;
}

static ClawtLrgSchedRow *
row_new(void)
{
    ClawtLrgSchedRow *row = g_new0(ClawtLrgSchedRow, 1);

    row->children = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_schedule_row_free);

    return row;
}

void
clawt_lrg_schedule_row_free(ClawtLrgSchedRow *row)
{
    if (row == NULL)
        return;

    g_free(row->title);
    g_free(row->agent);
    g_free(row->when);
    g_free(row->day);
    g_free(row->status);
    g_free(row->status_label);
    g_free(row->tone);
    g_free(row->source_note);
    g_free(row->tooltip);
    g_free(row->task);
    g_free(row->room);
    g_free(row->routine);
    g_free(row->summary);
    g_free(row->counts);
    g_clear_pointer(&row->children, g_ptr_array_unref);
    g_free(row);
}

static ClawtLrgSchedRow *
row_from_occurrence(JsonObject *object, const gchar *zone)
{
    ClawtLrgSchedRow *row = row_new();
    const gchar *source = clawt_ipc_payload_string(object, "source");
    const gchar *at = clawt_ipc_payload_string(object, "at");
    gboolean libreclaw = g_strcmp0(source,
                                   CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON) == 0;

    row->title = g_strdup(clawt_ipc_payload_string(object, "title"));

    if (row->title == NULL)
        row->title = g_strdup(clawt_ipc_payload_string(object, "routine"));

    if (row->title == NULL)
        row->title = g_strdup("Untitled");

    row->agent = g_strdup(clawt_ipc_payload_string(object, "agent"));
    row->when = format_iso(at, zone, "%H:%M");
    row->day = format_iso(at, zone, "%Y-%m-%d");
    row->status = g_strdup(clawt_ipc_payload_string(object, "status"));

    if (row->status == NULL)
        row->status = g_strdup("unknown");

    row->status_label = status_words(object);
    row->tone = g_strdup(tone_of(row->status));
    row->source_note = libreclaw ? g_strdup("libreclaw cron") : NULL;
    row->tooltip = tooltip_of(object, zone, FALSE);
    row->task = g_strdup(clawt_ipc_payload_string(object, "task"));
    row->room = g_strdup(clawt_ipc_payload_string(object, "room"));
    row->routine = g_strdup(clawt_ipc_payload_string(object, "routine"));
    row->action = action_of(object, FALSE, libreclaw);
    row->sort_key = unix_of(at);

    return row;
}

static ClawtLrgSchedRow *
row_from_group(JsonObject *object, const gchar *zone)
{
    ClawtLrgSchedRow *row = row_new();
    JsonArray *runs = NULL;
    const gchar *source = clawt_ipc_payload_string(object, "source");
    gboolean libreclaw = g_strcmp0(source,
                                   CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON) == 0;
    guint i;

    row->group = TRUE;
    row->title = g_strdup(clawt_ipc_payload_string(object, "title"));

    if (row->title == NULL)
        row->title = g_strdup(clawt_ipc_payload_string(object, "routine"));

    row->agent = g_strdup(clawt_ipc_payload_string(object, "agent"));
    row->day = g_strdup(clawt_ipc_payload_string(object, "day"));
    {
        g_autofree gchar *first = format_iso(
            clawt_ipc_payload_string(object, "first"), zone, "%H:%M");
        g_autofree gchar *last = format_iso(
            clawt_ipc_payload_string(object, "last"), zone, "%H:%M");

        row->when = g_strdup_printf("%s-%s", first, last);
    }
    row->status = g_strdup(clawt_ipc_payload_string(object, "status"));

    if (row->status == NULL)
        row->status = g_strdup("unknown");

    row->status_label = status_words(object);
    row->tone = g_strdup(tone_of(row->status));
    row->source_note = libreclaw ? g_strdup("libreclaw cron") : NULL;
    row->summary = g_strdup(clawt_ipc_payload_string(object, "label"));
    row->counts = counts_of(object);
    row->tooltip = tooltip_of(object, zone, TRUE);
    row->routine = g_strdup(clawt_ipc_payload_string(object, "routine"));
    row->action = CLAWT_LRG_SCHED_ACT_EXPAND;
    row->count = (guint)clawt_ipc_payload_int(object, "count", 0);
    row->sort_key = unix_of(clawt_ipc_payload_string(object, "first"));

    if (json_object_has_member(object, "occurrences") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "occurrences")))
        runs = json_object_get_array_member(object, "occurrences");

    for (i = 0; runs != NULL && i < json_array_get_length(runs); i++) {
        JsonObject *one = json_array_get_object_element(runs, i);

        if (one != NULL)
            g_ptr_array_add(row->children, row_from_occurrence(one, zone));
    }

    return row;
}

static gint
row_cmp(gconstpointer a, gconstpointer b)
{
    const ClawtLrgSchedRow *left = *(const ClawtLrgSchedRow *const *)a;
    const ClawtLrgSchedRow *right = *(const ClawtLrgSchedRow *const *)b;

    if (left->sort_key != right->sort_key)
        return (left->sort_key < right->sort_key) ? -1 : 1;

    return g_strcmp0(left->routine, right->routine);
}

static const gchar *
reply_zone(JsonObject *root)
{
    const gchar *zone = NULL;

    if (root != NULL)
        zone = clawt_ipc_payload_string(root, "time_zone");

    return zone;
}

static void
append_array(GPtrArray *rows, JsonObject *root, const gchar *key,
             gboolean groups)
{
    JsonArray *list = NULL;
    const gchar *zone = reply_zone(root);
    guint i;

    if (root == NULL || !json_object_has_member(root, key) ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, key)))
        return;

    list = json_object_get_array_member(root, key);

    for (i = 0; i < json_array_get_length(list); i++) {
        JsonObject *one = json_array_get_object_element(list, i);

        if (one == NULL)
            continue;

        g_ptr_array_add(rows, groups ? row_from_group(one, zone)
                                     : row_from_occurrence(one, zone));
    }
}

GPtrArray *
clawt_lrg_schedule_rows(JsonNode *reply)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_schedule_row_free);
    JsonObject *root = clawt_ipc_reply_object(reply);

    /*
     * occurrences for a plain window, runs for routine.upcoming.  A
     * group lives only under groups; its runs are not also in
     * occurrences, which is the frame's rule.
     */
    append_array(rows, root, "occurrences", FALSE);
    append_array(rows, root, "runs", FALSE);
    append_array(rows, root, "groups", TRUE);
    g_ptr_array_sort(rows, row_cmp);

    return rows;
}

GPtrArray *
clawt_lrg_schedule_unscheduled(JsonNode *reply)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_schedule_row_free);
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *list = NULL;
    guint i;

    if (root == NULL || !json_object_has_member(root, "unscheduled") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "unscheduled")))
        return rows;

    list = json_object_get_array_member(root, "unscheduled");

    for (i = 0; i < json_array_get_length(list); i++) {
        JsonObject *one = json_array_get_object_element(list, i);
        ClawtLrgSchedRow *row;
        const gchar *problem;
        gint64 every;

        if (one == NULL)
            continue;

        row = row_new();
        row->title = g_strdup(clawt_ipc_payload_string(one, "title"));

        if (row->title == NULL)
            row->title = g_strdup(clawt_ipc_payload_string(one, "id"));

        row->agent = g_strdup(clawt_ipc_payload_string(one, "agent"));
        row->routine = g_strdup(clawt_ipc_payload_string(one, "id"));
        row->source_note = g_strdup("libreclaw cron");
        row->status = g_strdup("unknown");
        row->status_label = g_strdup("not on the calendar");
        row->tone = g_strdup("dim");
        row->action = CLAWT_LRG_SCHED_ACT_DETAIL;
        problem = clawt_ipc_payload_string(one, "problem");
        every = clawt_ipc_payload_int(one, "interval_seconds", 0);

        if (every > 0)
            row->tooltip = g_strdup_printf(
                "Every %" G_GINT64_FORMAT
                " seconds from when the agent started. Not a fixed time.",
                every);
        else if (problem != NULL)
            row->tooltip = g_strdup(problem);
        else
            row->tooltip = g_strdup("No schedule clawtilla can lay out");

        row->when = g_strdup("");
        row->day = g_strdup("");
        g_ptr_array_add(rows, row);
    }

    return rows;
}
