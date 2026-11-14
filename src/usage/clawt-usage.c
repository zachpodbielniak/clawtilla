/*
 * clawt-usage.c - What the fleet has spent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every agent is a libreclaw process, and libreclaw has recorded one
 * `token_usage` row per AI turn -- tokens and cost -- since 0.24.0.  Each
 * agent's rows are in its own database, so the fleet's spend is the sum
 * over those files and has been sitting on disk, unread, the whole time.
 *
 * Read through libreclaw's own API rather than by opening its schema, for
 * the same reason `/reset` does: the table belongs to libreclaw, and a
 * copy of its column list here would be a second thing to keep in step
 * with a table we do not own.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>

#include "usage/clawt-usage.h"

/*
 * One agent's place in its own history.
 *
 * Both halves are needed.  Rows are filtered by `recorded_at`, which has
 * one-second resolution, so two turns in the same second are
 * indistinguishable by time alone -- the id settles which of them has
 * already been charged.
 */
typedef struct {
    LcDatabase *db;
    gint64      last_recorded_at;
    gint64      last_id;
    gboolean    primed;
} AgentWatermark;

struct _ClawtUsage {
    GObject     parent_instance;
    GHashTable *watermarks;   /* agent id -> AgentWatermark* */
};

G_DEFINE_FINAL_TYPE(ClawtUsage, clawt_usage, G_TYPE_OBJECT)

/* ── ClawtUsageTotals ────────────────────────────────────────────── */

ClawtUsageTotals *
clawt_usage_totals_copy(const ClawtUsageTotals *self)
{
    ClawtUsageTotals *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtUsageTotals, 1);
    *copy = *self;

    return copy;
}

void
clawt_usage_totals_free(ClawtUsageTotals *self)
{
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtUsageTotals, clawt_usage_totals,
                    clawt_usage_totals_copy, clawt_usage_totals_free)

void
clawt_usage_totals_add(ClawtUsageTotals *self, const ClawtUsageTotals *other)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(other != NULL);

    self->turns         += other->turns;
    self->input_tokens  += other->input_tokens;
    self->output_tokens += other->output_tokens;
    self->cost_micros   += other->cost_micros;
}

gchar *
clawt_usage_format_cost(gint64 cost_micros)
{
    gdouble dollars = (gdouble)cost_micros / 1000000.0;

    /*
     * Anything that would round to $0.00 is shown to four places
     * instead.  A per-turn cost of a fifth of a cent is the ordinary
     * case here, and a table of "$0.00" says the wrong thing about it.
     */
    if (cost_micros != 0 && cost_micros > -10000 && cost_micros < 10000)
        return g_strdup_printf("$%.4f", dollars);

    return g_strdup_printf("$%.2f", dollars);
}

/* ── Where the database is ───────────────────────────────────────── */

gchar *
clawt_usage_database_path(const gchar *state_dir)
{
    g_return_val_if_fail(state_dir != NULL, NULL);

    return g_build_filename(state_dir, "sessions", "libreclaw.db", NULL);
}

/* ── Reading ─────────────────────────────────────────────────────── */

/*
 * Opens an agent's database for reading.
 *
 * Returns NULL without setting @error when the file is simply not there
 * -- an agent that has never started has no history, which the callers
 * report as zero rather than as a failure.  Opening would otherwise
 * *create* the file, leaving an empty database behind every time
 * somebody asked what a stopped agent had spent.
 */
static LcDatabase *
open_agent_database(const gchar *db_path, GError **error)
{
    LcDatabase *db;

    if (!g_file_test(db_path, G_FILE_TEST_EXISTS))
        return NULL;

    db = LC_DATABASE(lc_sqlite_database_new());

    if (!lc_database_open(db, db_path, error)) {
        g_object_unref(db);
        return NULL;
    }

    return db;
}

/*
 * Sums a row array into @out, tracking the highest row seen.
 *
 * @min_id, when positive, skips rows at or below it.  The time filter
 * cannot express "after this row" on its own, and re-reading a second's
 * worth of rows is how the same turn would be charged twice.
 */
static void
accumulate_rows(GPtrArray        *rows,
                gint64            min_id,
                ClawtUsageTotals *out,
                gint64           *out_max_id,
                gint64           *out_max_time)
{
    guint i;

    for (i = 0; rows != NULL && i < rows->len; i++) {
        LcDbTokenUsage *row = g_ptr_array_index(rows, i);

        if (min_id > 0 && row->id <= min_id)
            continue;

        out->turns         += 1;
        out->input_tokens  += row->input_tokens;
        out->output_tokens += row->output_tokens;
        /* -1 is unknown, never a refund: an unpriced turn adds nothing */
        if (row->cost_usd_micros > 0)
            out->cost_micros += row->cost_usd_micros;

        if (out_max_id != NULL && row->id > *out_max_id)
            *out_max_id = row->id;
        if (out_max_time != NULL && row->recorded_at > *out_max_time)
            *out_max_time = row->recorded_at;
    }
}

gboolean
clawt_usage_read_totals(const gchar       *db_path,
                        gint64             since,
                        ClawtUsageTotals  *out,
                        GError           **error)
{
    g_autoptr(LcDatabase) db = NULL;
    GPtrArray *rows;

    g_return_val_if_fail(db_path != NULL, FALSE);
    g_return_val_if_fail(out != NULL, FALSE);

    memset(out, 0, sizeof(*out));

    db = open_agent_database(db_path, error);
    if (db == NULL)
        return (error == NULL || *error == NULL);

    rows = lc_database_query_token_usage(db, NULL, NULL, since, 0, error);
    if (rows == NULL) {
        lc_database_close(db);
        return FALSE;
    }

    accumulate_rows(rows, 0, out, NULL, NULL);

    g_ptr_array_unref(rows);
    lc_database_close(db);

    return TRUE;
}

/* ── The fleet view ──────────────────────────────────────────────── */

gint64 *
clawt_usage_day_starts(gint64 now, guint days)
{
    g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(now);
    g_autoptr(GDateTime) today = NULL;
    gint64 *starts;
    guint i;

    g_return_val_if_fail(days > 0, NULL);

    today = g_date_time_new_local(g_date_time_get_year(at),
                                  g_date_time_get_month(at),
                                  g_date_time_get_day_of_month(at), 0, 0, 0);
    starts = g_new0(gint64, days + 1);

    /* day by date, not by 86400 seconds: a DST day is still one day */
    for (i = 0; i <= days; i++) {
        g_autoptr(GDateTime) day =
            g_date_time_add_days(today, (gint)i - (gint)(days - 1));

        starts[i] = g_date_time_to_unix(day);
    }

    return starts;
}

gboolean
clawt_usage_read_report(const gchar       *db_path,
                        gint64             since,
                        const gint64      *day_starts,
                        guint              days,
                        ClawtUsageTotals  *out,
                        gint64            *last_turn_at,
                        gint64            *daily_micros,
                        GError           **error)
{
    g_autoptr(LcDatabase) db = NULL;
    GPtrArray *rows;
    guint i;

    g_return_val_if_fail(db_path != NULL, FALSE);
    g_return_val_if_fail(out != NULL, FALSE);

    memset(out, 0, sizeof(*out));
    if (last_turn_at != NULL)
        *last_turn_at = 0;
    if (daily_micros != NULL && days > 0)
        memset(daily_micros, 0, sizeof(gint64) * days);

    db = open_agent_database(db_path, error);
    if (db == NULL)
        return (error == NULL || *error == NULL);

    /*
     * Every row, not only those since @since: the last turn is the last
     * one ever, and an agent idle for longer than the period is exactly
     * the agent this view exists to find.
     */
    rows = lc_database_query_token_usage(db, NULL, NULL, 0, 0, error);
    if (rows == NULL) {
        lc_database_close(db);
        return FALSE;
    }

    for (i = 0; i < rows->len; i++) {
        LcDbTokenUsage *row = g_ptr_array_index(rows, i);

        if (last_turn_at != NULL && row->recorded_at > *last_turn_at)
            *last_turn_at = row->recorded_at;

        if (row->recorded_at >= since) {
            out->turns         += 1;
            out->input_tokens  += row->input_tokens;
            out->output_tokens += row->output_tokens;
            if (row->cost_usd_micros > 0)
                out->cost_micros += row->cost_usd_micros;
        }

        if (daily_micros != NULL && day_starts != NULL && days > 0 &&
            row->recorded_at >= day_starts[0] &&
            row->recorded_at < day_starts[days]) {
            guint d;

            for (d = 0; d < days; d++) {
                if (row->recorded_at < day_starts[d + 1]) {
                    daily_micros[d] += MAX(row->cost_usd_micros, 0);
                    break;
                }
            }
        }
    }

    g_ptr_array_unref(rows);
    lc_database_close(db);

    return TRUE;
}

ClawtIdleState
clawt_usage_idle_state(gint64 last_turn_at, gint64 now, gint64 idle_hours)
{
    if (last_turn_at <= 0)
        return CLAWT_IDLE_NEVER;
    if (idle_hours > 0 && now - last_turn_at >= idle_hours * 3600)
        return CLAWT_IDLE_IDLE;
    return CLAWT_IDLE_ACTIVE;
}

/* No `default:` in either: a state added later is a -Wswitch warning here */
const gchar *
clawt_usage_idle_label(ClawtIdleState state)
{
    switch (state) {
    case CLAWT_IDLE_ACTIVE:
        return "active";
    case CLAWT_IDLE_IDLE:
        return "idle";
    case CLAWT_IDLE_NEVER:
        return "never used";
    }
    return "active";
}

const gchar *
clawt_usage_idle_tone(ClawtIdleState state)
{
    switch (state) {
    case CLAWT_IDLE_ACTIVE:
        return "good";
    case CLAWT_IDLE_IDLE:
        return "warn";
    case CLAWT_IDLE_NEVER:
        return "neutral";
    }
    return "neutral";
}

gchar *
clawt_usage_last_turn_label(gint64 last_turn_at, gint64 now)
{
    /* the time-ago helper is in microseconds; a task stamp once was not */
    if (last_turn_at <= 0)
        return g_strdup("never");
    return clawt_time_ago_label(last_turn_at * G_USEC_PER_SEC,
                                now * G_USEC_PER_SEC);
}

gchar *
clawt_usage_sparkline(const gint64 *values, guint n)
{
    static const gchar *const bars[] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
    };
    GString *out = g_string_new(NULL);
    gint64 top = 0;
    guint i;

    for (i = 0; values != NULL && i < n; i++)
        top = MAX(top, values[i]);

    for (i = 0; values != NULL && i < n; i++) {
        if (values[i] <= 0 || top <= 0) {
            g_string_append(out, "\xc2\xb7");
        } else {
            /* the smallest spend is still the lowest bar, never a gap */
            guint level = (guint)((values[i] * 7) / top);

            g_string_append(out, bars[MIN(level, 7)]);
        }
    }

    return g_string_free(out, FALSE);
}

gchar *
clawt_usage_share_label(gint64 part, gint64 whole)
{
    gint64 percent;

    if (part <= 0 || whole <= 0)
        return g_strdup("0%");
    percent = (part * 100 + whole / 2) / whole;
    if (percent < 1)
        return g_strdup("<1%");
    return g_strdup_printf("%" G_GINT64_FORMAT "%%", MIN(percent, 100));
}

static const struct {
    const gchar *label;
    gint         days;     /* 0 all time, -1 since local midnight */
} usage_periods[] = {
    { "All time", 0 },
    { "Today", -1 },
    { "Last 7 days", 7 },
    { "Last 30 days", 30 }
};

guint
clawt_usage_period_count(void)
{
    return G_N_ELEMENTS(usage_periods);
}

const gchar *
clawt_usage_period_label(guint n)
{
    return usage_periods[MIN(n, G_N_ELEMENTS(usage_periods) - 1)].label;
}

gint64
clawt_usage_period_since(guint n, gint64 now)
{
    gint days;

    if (n >= G_N_ELEMENTS(usage_periods))
        return 0;
    days = usage_periods[n].days;
    if (days == 0)
        return 0;
    if (days < 0) {
        g_autofree gint64 *starts = clawt_usage_day_starts(now, 1);

        return starts[0];
    }
    return now - (gint64)days * 86400;
}

JsonNode *
clawt_usage_fleet_payload(gint64 since, guint days)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
    json_builder_set_member_name(builder, "days");
    json_builder_add_int_value(builder, days);
    json_builder_set_member_name(builder, "sort");
    json_builder_add_string_value(builder, "cost");
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* A member that is an object, or NULL -- a type check is not a pointer check */
static JsonObject *
member_object(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;
    node = json_object_get_member(object, name);
    return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static JsonArray *
member_array(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;
    node = json_object_get_member(object, name);
    return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

gchar *
clawt_usage_fleet_summary(JsonObject *reply)
{
    JsonObject *total = member_object(reply, "total");
    gint64 cost = clawt_ipc_payload_int(total, "cost_micros", 0);
    gint64 turns = clawt_ipc_payload_int(total, "turns", 0);
    g_autofree gchar *money = NULL;

    if (total == NULL || turns == 0)
        return g_strdup("Nothing spent in this period");

    money = clawt_usage_format_cost(cost);
    return g_strdup_printf("%s across %" G_GINT64_FORMAT " turn%s", money,
                           turns, turns == 1 ? "" : "s");
}

gchar *
clawt_usage_fleet_budget_line(JsonObject *reply)
{
    JsonArray *days = member_array(reply, "days");
    gint64 today;
    gint64 cap_micros;
    g_autofree gchar *spent = NULL;
    g_autofree gchar *cap = NULL;

    if (reply == NULL || days == NULL || json_array_get_length(days) == 0)
        return NULL;

    cap_micros = clawt_ipc_payload_int(reply, "daily_fleet_budget_micros", 0);
    if (cap_micros <= 0)
        return NULL;

    today = clawt_ipc_payload_int(
        json_array_get_object_element(days, json_array_get_length(days) - 1),
        "cost_micros", 0);
    spent = clawt_usage_format_cost(today);
    cap = clawt_usage_format_cost(cap_micros);

    /* at, not over: the router refuses at the figure, so it is never passed by much */
    if (today >= cap_micros)
        return g_strdup_printf("Today %s -- the %s daily cap is reached; "
                               "new work waits", spent, cap);
    return g_strdup_printf("Today %s of a %s daily cap", spent, cap);
}

gchar *
clawt_usage_fleet_strip(JsonObject *reply)
{
    JsonArray *days = member_array(reply, "days");
    g_autofree gint64 *values = NULL;
    guint n;
    guint i;

    if (days == NULL || (n = json_array_get_length(days)) == 0)
        return g_strdup("");

    values = g_new0(gint64, n);
    for (i = 0; i < n; i++)
        values[i] = clawt_ipc_payload_int(json_array_get_object_element(days, i),
                                          "cost_micros", 0);
    return clawt_usage_sparkline(values, n);
}

gchar *
clawt_usage_fleet_row_subtitle(JsonObject *agent, gint64 now)
{
    gint64 turns = clawt_ipc_payload_int(agent, "turns", 0);
    gint64 sleeps = clawt_ipc_payload_int(agent, "idle_stop_hours", 0);
    g_autofree gchar *last = clawt_usage_last_turn_label(
        clawt_ipc_payload_int(agent, "last_turn_at", 0), now);
    GString *out = g_string_new(NULL);

    g_string_append_printf(out, "%" G_GINT64_FORMAT " turn%s \xc2\xb7 last turn %s",
                           turns, turns == 1 ? "" : "s", last);

    if (clawt_ipc_payload_boolean(agent, "asleep", FALSE))
        g_string_append(out, " \xc2\xb7 asleep until a message");
    else if (sleeps > 0)
        g_string_append_printf(out, " \xc2\xb7 sleeps after %" G_GINT64_FORMAT "h",
                               sleeps);

    return g_string_free(out, FALSE);
}

gboolean
clawt_usage_fleet_row_stoppable(JsonObject *agent)
{
    const gchar *idle = clawt_ipc_payload_string(agent, "idle");

    return g_strcmp0(clawt_ipc_payload_string(agent, "state"), "running") == 0 &&
           (g_strcmp0(idle, clawt_usage_idle_label(CLAWT_IDLE_IDLE)) == 0 ||
            g_strcmp0(idle, clawt_usage_idle_label(CLAWT_IDLE_NEVER)) == 0);
}

guint
clawt_usage_fleet_stoppable_count(JsonObject *reply)
{
    JsonArray *agents = member_array(reply, "agents");
    guint count = 0;
    guint i;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++)
        if (clawt_usage_fleet_row_stoppable(json_array_get_object_element(agents, i)))
            count++;
    return count;
}

/* ── Charging ────────────────────────────────────────────────────── */

static void
agent_watermark_free(gpointer data)
{
    AgentWatermark *mark = data;

    if (mark->db != NULL) {
        lc_database_close(mark->db);
        g_object_unref(mark->db);
    }

    g_free(mark);
}

ClawtUsage *
clawt_usage_new(void)
{
    return g_object_new(CLAWT_TYPE_USAGE, NULL);
}

gboolean
clawt_usage_prime(ClawtUsage *self, const gchar *agent_id,
				  const gchar *db_path, GError **error)
{
	AgentWatermark *mark;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) local = NULL;
	ClawtUsageTotals totals = { 0, 0, 0, 0 };

	g_return_val_if_fail(CLAWT_IS_USAGE(self), FALSE);
	g_return_val_if_fail(agent_id != NULL, FALSE);
	g_return_val_if_fail(db_path != NULL, FALSE);

	mark = g_hash_table_lookup(self->watermarks, agent_id);
	if (mark == NULL) {
		mark = g_new0(AgentWatermark, 1);
		g_hash_table_insert(self->watermarks, g_strdup(agent_id), mark);
	}
	/* Reconnects must not consume costs that have not reached a reply yet. */
	if (mark->primed)
		return TRUE;
	if (mark->db == NULL) {
		mark->db = open_agent_database(db_path, &local);
		if (mark->db == NULL) {
			if (local != NULL) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			/* Known empty, but leave the handle retryable when the agent
			 * creates its database and records its first paid turn. */
			mark->primed = TRUE;
			return TRUE;
		}
	}
	rows = lc_database_query_token_usage(mark->db, NULL, NULL, 0, 0, error);
	if (rows == NULL)
		return FALSE;
	accumulate_rows(rows, 0, &totals, &mark->last_id, &mark->last_recorded_at);
	mark->primed = TRUE;
	return TRUE;
}

gint64
clawt_usage_drain(ClawtUsage *self, const gchar *agent_id,
                  const gchar *db_path)
{
	return clawt_usage_drain_tagged(self, agent_id, db_path, NULL, NULL);
}

/*
 * Writes @func's answer for each row past the watermark.
 *
 * A tag that cannot be written is logged and dropped: the ledger is a
 * report, and refusing to charge a task because its turn could not be
 * labelled would turn a missing label into a budget nobody enforces.
 */
static void
tag_rows(
	GPtrArray            *rows,
	gint64                min_id,
	const gchar          *agent_id,
	const gchar          *db_path,
	ClawtUsageOriginFunc  func,
	gpointer              user_data
){
	g_autofree gchar *ledger_path = NULL;
	guint i;

	if (func == NULL || rows == NULL)
		return;

	ledger_path = clawt_usage_ledger_path_beside(db_path);

	for (i = 0; i < rows->len; i++) {
		LcDbTokenUsage *row = g_ptr_array_index(rows, i);
		g_autoptr(ClawtUsageOrigin) origin = NULL;
		g_autoptr(GError) error = NULL;

		if (row->id <= min_id)
			continue;

		origin = func(agent_id, row->room, row->channel, user_data);
		if (origin == NULL)
			continue;

		if (!clawt_usage_ledger_record(ledger_path, row->id, row->recorded_at,
					       row->room, origin, &error))
			g_warning("usage: turn %" G_GINT64_FORMAT " of %s is untagged: %s",
				  row->id, agent_id, error->message);
	}
}

gint64
clawt_usage_drain_tagged(ClawtUsage *self, const gchar *agent_id,
                         const gchar *db_path, ClawtUsageOriginFunc func,
                         gpointer user_data)
{
    AgentWatermark   *mark;
    ClawtUsageTotals  totals = { 0, 0, 0, 0 };
    GPtrArray        *rows;
    g_autoptr(GError) error = NULL;
    gint64            max_id;
    gint64            max_time;

    g_return_val_if_fail(CLAWT_IS_USAGE(self), 0);
    g_return_val_if_fail(agent_id != NULL, 0);
    g_return_val_if_fail(db_path != NULL, 0);

    mark = g_hash_table_lookup(self->watermarks, agent_id);
    if (mark == NULL) {
        mark = g_new0(AgentWatermark, 1);
        g_hash_table_insert(self->watermarks, g_strdup(agent_id), mark);
    }

    /*
     * The handle is kept open across drains.  Charging happens on the
     * daemon's main context whenever an agent answers, and reopening a
     * database -- which runs libreclaw's schema check every time -- is
     * not something to do on that path.
     *
     * A database that is not there yet is not cached as a failure: an
     * agent starting for the first time writes it moments later.
     */
    if (mark->db == NULL) {
        mark->db = open_agent_database(db_path, &error);

        if (mark->db == NULL) {
            if (error != NULL)
                g_debug("usage: cannot read %s: %s", db_path, error->message);
            return 0;
        }
    }

    max_id   = mark->last_id;
    max_time = mark->last_recorded_at;

    rows = lc_database_query_token_usage(mark->db, NULL, NULL,
                                         mark->last_recorded_at, 0, &error);
    if (rows == NULL) {
        g_debug("usage: cannot query %s: %s", db_path,
                error != NULL ? error->message : "unknown error");
        return 0;
    }

    accumulate_rows(rows, mark->last_id, &totals, &max_id, &max_time);

    /* history from before the baseline is not ours to label */
    if (mark->primed)
        tag_rows(rows, mark->last_id, agent_id, db_path, func, user_data);
    g_ptr_array_unref(rows);

    mark->last_id          = max_id;
    mark->last_recorded_at = max_time;

    /*
     * The first sight of an agent sets the watermark and charges
     * nothing.  Everything before now was spent under some earlier
     * daemon, on work that has already finished -- charging a fresh task
     * for it would exhaust its budget before its first turn.
     */
    if (!mark->primed) {
        mark->primed = TRUE;
        return 0;
    }

    return totals.cost_micros;
}

void
clawt_usage_forget(ClawtUsage *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_USAGE(self));
    g_return_if_fail(agent_id != NULL);

    g_hash_table_remove(self->watermarks, agent_id);
}

/* ── GObject ─────────────────────────────────────────────────────── */

static void
clawt_usage_finalize(GObject *object)
{
    ClawtUsage *self = CLAWT_USAGE(object);

    g_clear_pointer(&self->watermarks, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_usage_parent_class)->finalize(object);
}

static void
clawt_usage_class_init(ClawtUsageClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = clawt_usage_finalize;
}

static void
clawt_usage_init(ClawtUsage *self)
{
    self->watermarks = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, agent_watermark_free);
}
