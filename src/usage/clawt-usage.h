/*
 * clawt-usage.h - What the fleet has spent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_USAGE (clawt_usage_get_type())

G_DECLARE_FINAL_TYPE(ClawtUsage, clawt_usage, CLAWT, USAGE, GObject)

#define CLAWT_TYPE_USAGE_TOTALS (clawt_usage_totals_get_type())

/**
 * ClawtUsageTotals:
 * @turns: how many AI turns were recorded
 * @input_tokens: tokens sent
 * @output_tokens: tokens received
 * @cost_micros: cost in millionths of a US dollar
 *
 * What one agent, or the whole fleet, has spent over some window.
 *
 * Cost is carried in micro-dollars rather than as a double because it is
 * summed across every agent in the fleet, and a running total of floats
 * disagrees with itself depending on the order the agents happened to be
 * visited in.
 */
typedef struct {
    gint64 turns;
    gint64 input_tokens;
    gint64 output_tokens;
    gint64 cost_micros;
} ClawtUsageTotals;

GType clawt_usage_totals_get_type(void);

/**
 * clawt_usage_totals_copy:
 * @self: a #ClawtUsageTotals
 *
 * Returns: (transfer full): a copy
 */
ClawtUsageTotals *clawt_usage_totals_copy(const ClawtUsageTotals *self);

/**
 * clawt_usage_totals_free:
 * @self: (transfer full): a #ClawtUsageTotals
 */
void clawt_usage_totals_free(ClawtUsageTotals *self);

/**
 * clawt_usage_totals_add:
 * @self: the running total
 * @other: what to add to it
 *
 * Accumulates @other into @self, so a fleet total is built from the
 * per-agent ones rather than from a second query.
 */
void clawt_usage_totals_add(ClawtUsageTotals       *self,
                            const ClawtUsageTotals *other);

/**
 * clawt_usage_format_cost:
 * @cost_micros: a cost in millionths of a US dollar
 *
 * Renders a cost for a person to read.
 *
 * Sub-cent amounts keep four decimal places rather than rounding to
 * "$0.00", because a fleet's per-turn costs are routinely below a cent
 * and a column of zeroes reads as "nothing is being recorded" -- which
 * is the one thing this is here to disprove.
 *
 * Returns: (transfer full): the formatted cost
 */
gchar *clawt_usage_format_cost(gint64 cost_micros);

/**
 * clawt_usage_database_path:
 * @state_dir: an agent's state directory
 *
 * The single place that knows where libreclaw keeps an agent's database.
 *
 * It is not where clawtilla asks for it. libreclaw's sqlite backend
 * builds the filename from `session.persist_dir` and never reads
 * `database.path`, so the file is always `<persist_dir>/libreclaw.db`
 * whatever the rendered config says. Two spellings of this path is how
 * `/reset` came to look for a file that has never existed on any
 * machine, find nothing, and report success.
 *
 * Returns: (transfer full): the path to the agent's libreclaw database
 */
gchar *clawt_usage_database_path(const gchar *state_dir);

/**
 * clawt_usage_read_totals:
 * @db_path: an agent's libreclaw database
 * @since: unix seconds to count from, or 0 for everything
 * @out: (out caller-allocates): where to put the totals
 * @error: (out) (optional): return location for a #GError
 *
 * Sums one agent's recorded turns.
 *
 * A database that is not there yet is not an error: an agent that has
 * never run has spent nothing, which is a total rather than a failure.
 * @out is zeroed in that case and %TRUE returned.
 *
 * Returns: %TRUE if the totals could be read
 */
gboolean clawt_usage_read_totals(const gchar       *db_path,
                                 gint64             since,
                                 ClawtUsageTotals  *out,
                                 GError           **error);

/**
 * clawt_usage_day_starts: (skip)
 * @now: unix seconds, the moment "today" is measured from
 * @days: how many days, today included
 *
 * The local midnights that bound @days days ending today: element 0 is
 * the start of the oldest day and element @days the start of tomorrow,
 * so day `i` is `[starts[i], starts[i + 1])`.  Local, because "what did
 * the fleet spend on Tuesday" means the person's Tuesday, and computed
 * as dates rather than by subtracting 86400 so a day that has 23 or 25
 * hours is still one day.
 *
 * Not introspectable: the array is @days + 1 long, which an annotation
 * cannot say.
 *
 * Returns: (transfer full): @days + 1 timestamps
 */
gint64 *clawt_usage_day_starts(gint64 now, guint days);

/**
 * clawt_usage_read_report: (skip)
 * @db_path: an agent's libreclaw database
 * @since: unix seconds to total from, or 0 for everything
 * @day_starts: (nullable): @days + 1 day boundaries from
 *   clawt_usage_day_starts(), or %NULL for no daily figures
 * @days: how many days @day_starts describes
 * @out: (out caller-allocates): the totals since @since
 * @last_turn_at: (out) (optional): unix seconds of the agent's most recent
 *   recorded turn ever, or 0 when it has never taken one
 * @daily_micros: (out caller-allocates) (array length=days) (optional):
 *   cost per day, oldest first; zeroed first
 * @error: (out) (optional): return location for a #GError
 *
 * One read of an agent's recorded turns for the fleet view: what it spent
 * in a period, when it last did anything at all, and how the spend fell
 * across the last @days days.  A missing database is an agent that has
 * never run: zero, last turn 0, and not created -- as for
 * clawt_usage_read_totals().
 *
 * Returns: %TRUE if the database could be read (or was not there)
 */
gboolean clawt_usage_read_report(const gchar       *db_path,
                                 gint64             since,
                                 const gint64      *day_starts,
                                 guint              days,
                                 ClawtUsageTotals  *out,
                                 gint64            *last_turn_at,
                                 gint64            *daily_micros,
                                 GError           **error);

/**
 * ClawtIdleState:
 * @CLAWT_IDLE_ACTIVE: took a turn within the idle window
 * @CLAWT_IDLE_IDLE: has taken turns, none within the window
 * @CLAWT_IDLE_NEVER: has never taken a turn
 *
 * How recently an agent did anything, for the Fleet page and the CLI.
 * Separate from whether it is running: an idle agent can be running
 * (the expensive case: a process and a computer for nothing) or stopped.
 */
typedef enum {
    CLAWT_IDLE_ACTIVE,
    CLAWT_IDLE_IDLE,
    CLAWT_IDLE_NEVER
} ClawtIdleState;

/**
 * clawt_usage_idle_state:
 * @last_turn_at: unix seconds of the last turn, 0 for never
 * @now: unix seconds
 * @idle_hours: the window, `orchestration.idle_hours`; 0 calls nothing idle
 *
 * The one rule every client and the CLI apply.
 *
 * Returns: the agent's #ClawtIdleState
 */
ClawtIdleState clawt_usage_idle_state(gint64 last_turn_at, gint64 now,
                                      gint64 idle_hours);

/**
 * clawt_usage_idle_label:
 * @state: a #ClawtIdleState
 *
 * Returns: (transfer none): "active", "idle" or "never used"
 */
const gchar *clawt_usage_idle_label(ClawtIdleState state);

/**
 * clawt_usage_idle_tone:
 * @state: a #ClawtIdleState
 *
 * The badge tone the clients paint it in: an idle agent is a warning
 * (something is being held for nothing), one never used is neutral.
 *
 * Returns: (transfer none): "good", "warn" or "neutral"
 */
const gchar *clawt_usage_idle_tone(ClawtIdleState state);

/**
 * clawt_usage_last_turn_label:
 * @last_turn_at: unix seconds of the last turn, 0 for never
 * @now: unix seconds
 *
 * Returns: (transfer full): "never", or how long ago, as
 *   clawt_time_ago_label() spells it
 */
gchar *clawt_usage_last_turn_label(gint64 last_turn_at, gint64 now);

/**
 * clawt_usage_sparkline:
 * @values: (array length=n): costs, oldest first
 * @n: how many
 *
 * One character per value, scaled to the largest: `▁`..`█` for spend,
 * `·` for a day with none -- so a day the fleet was idle reads as a gap
 * rather than as the smallest bar.  Text rather than a drawing so every
 * client shows the same thing: a GTK label, a web span, an lrg label.
 *
 * Returns: (transfer full): the strip
 */
gchar *clawt_usage_sparkline(const gint64 *values, guint n);

/**
 * clawt_usage_share_label:
 * @part: one agent's cost
 * @whole: the fleet's
 *
 * Returns: (transfer full): "42%", "<1%" for a sliver, "0%" for nothing
 */
gchar *clawt_usage_share_label(gint64 part, gint64 whole);

/* ── The Fleet page, as every client draws it ────────────────────── */

/**
 * clawt_usage_period_count:
 *
 * The periods the Fleet page offers: All time, Today, Last 7 days, Last
 * 30 days.  One table for every client, walked with _nth.
 *
 * Returns: how many there are
 */
guint clawt_usage_period_count(void);

/**
 * clawt_usage_period_label:
 * @n: an index below clawt_usage_period_count()
 *
 * Returns: (transfer none): what to call it
 */
const gchar *clawt_usage_period_label(guint n);

/**
 * clawt_usage_period_since:
 * @n: an index below clawt_usage_period_count()
 * @now: unix seconds
 *
 * Returns: the `since` it means: 0 for all time, local midnight for
 *   today, @now less the days otherwise
 */
gint64 clawt_usage_period_since(guint n, gint64 now);

/* The period the Fleet page opens on: Last 7 days. */
#define CLAWT_USAGE_PERIOD_DEFAULT (2)

/**
 * clawt_usage_fleet_payload:
 * @since: unix seconds the totals start at, 0 for all time
 * @days: how many days of daily figures, 0 for none
 *
 * The `usage.summary` request the Fleet page sends: the period, the
 * days, and the rows sorted by cost.
 *
 * Returns: (transfer full): the payload
 */
JsonNode *clawt_usage_fleet_payload(gint64 since, guint days);

/**
 * clawt_usage_fleet_summary:
 * @reply: a `usage.summary` reply object
 *
 * Returns: (transfer full): "$3.20 across 41 turns", or "Nothing spent
 *   in this period"
 */
gchar *clawt_usage_fleet_summary(JsonObject *reply);

/**
 * clawt_usage_fleet_budget_line:
 * @reply: a `usage.summary` reply object asked for with days
 *
 * Reads `daily_fleet_budget_micros`, the cap the router enforces.
 *
 * Returns: (transfer full) (nullable): "Today $0.42 of a $5.00 daily
 *   cap", or "Today $5.01 -- the $5.00 daily cap is reached; new work
 *   waits", or %NULL when no fleet cap is set or no days were asked for
 */
gchar *clawt_usage_fleet_budget_line(JsonObject *reply);

/**
 * clawt_usage_fleet_strip:
 * @reply: a `usage.summary` reply object asked for with days
 *
 * Returns: (transfer full): the fleet's days as clawt_usage_sparkline()
 *   draws them, oldest first; "" when no days were asked for
 */
gchar *clawt_usage_fleet_strip(JsonObject *reply);

/**
 * clawt_usage_fleet_row_subtitle:
 * @agent: one element of the reply's `agents`
 * @now: unix seconds
 *
 * Returns: (transfer full): "12 turns · last turn 3d ago", with
 *   " · asleep until a message" for an agent put to sleep, or " · sleeps
 *   after 24h" for one set to
 */
gchar *clawt_usage_fleet_row_subtitle(JsonObject *agent, gint64 now);

/**
 * clawt_usage_fleet_row_stoppable:
 * @agent: one element of the reply's `agents`
 *
 * Whether the row offers Stop: running and idle (or never used).  The
 * expensive case the page exists for -- a process and a computer held
 * for nothing.
 *
 * Returns: %TRUE when it is running and not active
 */
gboolean clawt_usage_fleet_row_stoppable(JsonObject *agent);

/**
 * clawt_usage_fleet_stoppable_count:
 * @reply: a `usage.summary` reply object
 *
 * Returns: how many rows clawt_usage_fleet_row_stoppable() offers Stop on
 */
guint clawt_usage_fleet_stoppable_count(JsonObject *reply);

/**
 * clawt_usage_new:
 *
 * Tracks how much of each agent's usage has already been charged.
 *
 * Reporting is a pure read; charging is not, because a task's budget has
 * to be told about each turn exactly once. This object owns that
 * watermark and nothing else.
 *
 * Returns: (transfer full): a new #ClawtUsage
 */
ClawtUsage *clawt_usage_new(void);

/**
 * clawt_usage_prime:
 * @self: a #ClawtUsage
 * @agent_id: whose baseline to establish
 * @db_path: that agent's libreclaw database
 * @error: (out) (optional): return location for an error
 *
 * Establishes the historical baseline before an agent can do new work.
 * A missing database is an empty baseline and is not created. Repeated
 * calls preserve any spending accumulated since the original baseline.
 * Call again after clawt_usage_forget() when replacing a database.
 *
 * Returns: %TRUE if a baseline is available
 */
gboolean clawt_usage_prime(ClawtUsage *self, const gchar *agent_id,
						  const gchar *db_path, GError **error);

/**
 * clawt_usage_drain:
 * @self: a #ClawtUsage
 * @agent_id: whose usage to read
 * @db_path: that agent's libreclaw database
 *
 * Returns what @agent_id has spent since the last call, and remembers
 * that it has been handed over.
 *
 * Without a preceding clawt_usage_prime(), the first call returns 0 and
 * only sets the watermark. The daemon must prime before new work begins. A
 * daemon restarted mid-task must not charge that task for every turn the
 * agent has ever taken -- which for an agent with a long history would
 * exhaust any budget instantly, on work that was already paid for.
 *
 * Returns: the cost in micro-dollars since the previous drain
 */
gint64 clawt_usage_drain(ClawtUsage  *self,
                         const gchar *agent_id,
                         const gchar *db_path);

/**
 * ClawtUsageOriginFunc:
 * @agent_id: whose turn
 * @room: (nullable): the room libreclaw recorded the turn in
 * @channel: (nullable): the channel it recorded
 * @user_data: data passed to clawt_usage_drain_tagged()
 *
 * Says what started one newly recorded turn, from the turn state of the
 * agent that ran it.
 *
 * Returns: (transfer full) (nullable): the origin, or %NULL to leave
 *   the turn untagged
 */
typedef ClawtUsageOrigin *(*ClawtUsageOriginFunc)(const gchar *agent_id,
						  const gchar *room,
						  const gchar *channel,
						  gpointer     user_data);

/**
 * clawt_usage_drain_tagged: (skip)
 * @self: a #ClawtUsage
 * @agent_id: whose usage to read
 * @db_path: that agent's libreclaw database
 * @func: (nullable): what tags each new turn's origin
 * @user_data: data for @func
 *
 * clawt_usage_drain(), and for each turn it hands over, @func's answer
 * written to the ledger beside @db_path (clawt_usage_ledger_path()).
 *
 * The drain is the one place every new turn passes exactly once, while
 * the agent's turn state still describes it -- so it is where a turn is
 * tagged.  Turns before the watermark was primed are history clawtilla
 * did not see begin, and are left untagged rather than guessed at.
 *
 * Returns: the cost in micro-dollars since the previous drain
 */
gint64 clawt_usage_drain_tagged(ClawtUsage           *self,
				const gchar          *agent_id,
				const gchar          *db_path,
				ClawtUsageOriginFunc  func,
				gpointer              user_data);

/**
 * clawt_usage_forget:
 * @self: a #ClawtUsage
 * @agent_id: an agent id
 *
 * Drops the watermark and any open handle for @agent_id.
 *
 * Required after `/reset`, which moves the whole sessions directory
 * aside: the next database at that path is a different one, starting its
 * ids again from 1, and a watermark from the old one would suppress
 * every row in it.
 */
void clawt_usage_forget(ClawtUsage  *self,
                        const gchar *agent_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtUsageTotals, clawt_usage_totals_free)

G_END_DECLS
