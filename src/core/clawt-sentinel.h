/*
 * clawt-sentinel.h - A read-only scan of the event log for recurring trouble
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `<state_dir>/events/<day>.ndjson` is the record of everything the fleet
 * did, one line per event, and nobody reads it until something has gone
 * wrong -- by which time the same tool has failed forty times, a queue
 * has kept a message waiting all afternoon, or an agent has spent a
 * day's budget on a loop.  Each of those is visible in the log as a
 * pattern and invisible in any one event.
 *
 * The sentinel reads the log and groups what it finds into *issues*:
 *
 *   - `tool_failure`: one agent's tool failing again and again --
 *     `turn.step` with `failed`, and `computer.exec` with a non-zero
 *     `exit` -- grouped by agent and tool, reported at
 *     `sentinel.min_failures`;
 *   - `queue_wait`: a message queued for an agent (`mailbox.queued`) and
 *     delivered (`mailbox.delivered`) more than `sentinel.queue_wait_seconds`
 *     later, paired first-in first-out per agent, and messages still
 *     queued past it when the scan ends;
 *   - `spend`: an agent whose `turn.cost` for one day reached
 *     `sentinel.daily_spend_micros`, and every `spending.limit-reached`;
 *   - `stall`: `turn.timed_out`, `turn.repeating`, `exchange.stalled` and
 *     `message.refused`, grouped by agent and kind.
 *
 * **It never writes to the event log.**  Files are mapped read-only
 * (g_mapped_file_new() with `writable` FALSE), and the report goes to a
 * path that is refused if it is inside the log directory -- so a scanner
 * pointed at the wrong place cannot become a writer into the record it
 * exists to read.  It runs out of process too: `clawtilla sentinel` is
 * the scanner, and the daemon only starts it and reads what it wrote.
 *
 * The report is YAML (clawt_sentinel_report_to_yaml()), because it is a
 * file a person may open; the daemon hands clients the same document as
 * JSON.
 *
 * What it cannot see, said rather than implied: the queue pairing is
 * first-in first-out per agent and cannot tell a merged delivery's parts
 * apart, so a wait is the time from a queueing to the *next* delivery to
 * that agent; a daemon restart in the window is read as it was logged.
 * Cost comes only from `turn.cost` events, which a daemon older than the
 * sentinel did not write.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * ClawtSentinelOptions:
 * @days: how many days of log files to read, counting today; 0 reads all
 * @min_failures: how many failures of one tool make an issue
 * @queue_wait_seconds: how long a message may wait in a queue before the
 *   wait is an issue
 * @daily_spend_micros: one agent's spend in one day that makes an issue;
 *   0 reports only the caps that were reached
 * @now_usec: wall-clock microseconds the scan treats as now; 0 for the
 *   real time
 *
 * What a scan looks for.  The defaults are the schema's `sentinel.*`.
 */
typedef struct {
	guint  days;
	guint  min_failures;
	guint  queue_wait_seconds;
	gint64 daily_spend_micros;
	gint64 now_usec;
} ClawtSentinelOptions;

/**
 * clawt_sentinel_options_init:
 * @options: (out caller-allocates): the options to fill
 *
 * The schema's defaults, for a caller with no config to read.
 */
void
clawt_sentinel_options_init(
	ClawtSentinelOptions *options
);

/**
 * clawt_sentinel_scan:
 * @events_dir: the event log directory, `<state_dir>/events`
 * @options: (nullable): what to look for, or %NULL for the defaults
 * @error: (out) (optional): return location for a #GError
 *
 * Reads every `<day>.ndjson` in @events_dir within @options' days, read-only,
 * and groups what it finds into issues.  A line that is not an event is
 * counted and skipped, never fatal: the log is appended to by a daemon
 * that may have died mid-line.
 *
 * The report: `sentinel` (`version`, `scanned_at`, `since`, `files`,
 * `events`, `skipped`, `truncated`) and `issues`, worst first, each with
 * `kind`, `severity` (`bad` or `warning`), `subject`, `key`, `title`,
 * `count`, `first_at`, `last_at` (unix seconds), `value` (the kind's
 * measure: longest wait in seconds, spend in micro-dollars, else 0) and
 * `sample` (the last detail seen, when there was one).
 *
 * Returns: (transfer full) (nullable): the report, or %NULL when the
 *   directory cannot be read
 */
JsonNode *
clawt_sentinel_scan(
	const gchar                *events_dir,
	const ClawtSentinelOptions *options,
	GError                    **error
);

/**
 * clawt_sentinel_report_to_yaml:
 * @report: a report from clawt_sentinel_scan()
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the report as YAML
 */
gchar *
clawt_sentinel_report_to_yaml(
	JsonNode  *report,
	GError   **error
);

/**
 * clawt_sentinel_report_from_yaml:
 * @text: what clawt_sentinel_report_to_yaml() wrote
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the report, or %NULL when @text is
 *   not one
 */
JsonNode *
clawt_sentinel_report_from_yaml(
	const gchar  *text,
	GError      **error
);

/**
 * clawt_sentinel_write_report:
 * @report: a report
 * @path: where to write it
 * @events_dir: (nullable): the directory that was scanned
 * @error: (out) (optional): return location for a #GError
 *
 * Writes @report as YAML, atomically and 0600.  Refused when @path is
 * inside @events_dir: the event log is the record, and the scanner that
 * reads it must not be able to add a file to it.
 *
 * Returns: %TRUE when it was written
 */
gboolean
clawt_sentinel_write_report(
	JsonNode     *report,
	const gchar  *path,
	const gchar  *events_dir,
	GError      **error
);

/**
 * clawt_sentinel_kind_label:
 * @kind: (nullable): an issue's `kind`
 *
 * What a person reads for a kind: "Failing tool", "Slow queue",
 * "Spending", "Stalled turns", or the kind itself.  One spelling for the
 * three clients.
 *
 * Returns: (transfer none): the label
 */
const gchar *
clawt_sentinel_kind_label(
	const gchar *kind
);

/**
 * clawt_sentinel_age_text:
 * @scanned_at: the report's `scanned_at`, unix seconds, or 0
 * @now: unix seconds
 *
 * "Scanned 3m ago", or "Not scanned yet" -- what the Issues view says
 * above the list, so an old report is not read as the fleet's present.
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_sentinel_age_text(
	gint64 scanned_at,
	gint64 now
);

G_END_DECLS
