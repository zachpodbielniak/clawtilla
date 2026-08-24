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
 * clawt_usage_drain:
 * @self: a #ClawtUsage
 * @agent_id: whose usage to read
 * @db_path: that agent's libreclaw database
 *
 * Returns what @agent_id has spent since the last call, and remembers
 * that it has been handed over.
 *
 * The first call on an agent returns 0 and only sets the watermark. A
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
