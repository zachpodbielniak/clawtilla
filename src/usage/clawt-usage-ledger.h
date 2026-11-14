/*
 * clawt-usage-ledger.h - What the fleet spent, by model, by origin, by day
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

/* ── Who started a turn ──────────────────────────────────────────── */

/**
 * ClawtUsageOriginKind:
 * @CLAWT_USAGE_ORIGIN_UNKNOWN: nothing was recorded for the turn: it
 *   predates the ledger, or no turn state described it
 * @CLAWT_USAGE_ORIGIN_OPERATOR: the person running the fleet
 * @CLAWT_USAGE_ORIGIN_AGENT: another agent in the fleet
 * @CLAWT_USAGE_ORIGIN_ROUTINE: a routine's run, or work it handed on
 * @CLAWT_USAGE_ORIGIN_TRIGGER: a trigger's run, or work it handed on
 * @CLAWT_USAGE_ORIGIN_TASK: a task whose starter is not one of the above
 * @CLAWT_USAGE_ORIGIN_CHANNEL: a message on one of the agent's own
 *   channels (Matrix, email...), which clawtilla never routed
 * @CLAWT_USAGE_ORIGIN_OTHER: a sender clawtilla knows by name only, such
 *   as a Grok bot
 *
 * What a turn's cost is charged to when the ledger is read by origin.
 *
 * A routine or a trigger wins over the agent that delivered the turn:
 * a routine that delegates to three agents is one piece of work, and
 * "which routine costs me most" is the question this exists to answer.
 */
typedef enum {
	CLAWT_USAGE_ORIGIN_UNKNOWN,
	CLAWT_USAGE_ORIGIN_OPERATOR,
	CLAWT_USAGE_ORIGIN_AGENT,
	CLAWT_USAGE_ORIGIN_ROUTINE,
	CLAWT_USAGE_ORIGIN_TRIGGER,
	CLAWT_USAGE_ORIGIN_TASK,
	CLAWT_USAGE_ORIGIN_CHANNEL,
	CLAWT_USAGE_ORIGIN_OTHER
} ClawtUsageOriginKind;

/**
 * clawt_usage_origin_kind_nick:
 * @kind: a #ClawtUsageOriginKind
 *
 * Returns: (transfer none): "untagged", "operator", "agent", "routine",
 *   "trigger", "task", "channel" or "other"
 */
const gchar *clawt_usage_origin_kind_nick(ClawtUsageOriginKind kind);

/**
 * clawt_usage_origin_kind_from_nick:
 * @nick: (nullable): a nick from clawt_usage_origin_kind_nick()
 *
 * Returns: the kind, %CLAWT_USAGE_ORIGIN_UNKNOWN for anything else
 */
ClawtUsageOriginKind clawt_usage_origin_kind_from_nick(const gchar *nick);

/**
 * clawt_usage_origin_classify:
 * @sender: (nullable): who the delivery that set the turn up came from,
 *   %NULL when no turn state described the turn
 * @sender_is_agent: whether @sender is an agent in the fleet
 * @task_id: (nullable): the task the turn was about
 * @routine_id: (nullable): the routine whose run started @task_id's tree
 * @trigger_id: (nullable): the trigger whose run started @task_id's tree
 * @channel: (nullable): the libreclaw channel the turn arrived on
 * @out_id: (out) (transfer none) (optional): which operator, agent,
 *   routine, trigger, task, channel or sender; %NULL for the operator
 *   and for an unknown origin.  Points into the arguments.
 *
 * The one rule for what a turn is charged to, so the daemon that writes
 * the ledger and the test that pins it read the same thing.
 *
 * Returns: the #ClawtUsageOriginKind
 */
ClawtUsageOriginKind clawt_usage_origin_classify(const gchar  *sender,
						 gboolean      sender_is_agent,
						 const gchar  *task_id,
						 const gchar  *routine_id,
						 const gchar  *trigger_id,
						 const gchar  *channel,
						 const gchar **out_id);

/**
 * clawt_usage_origin_label:
 * @kind: a #ClawtUsageOriginKind
 * @id: (nullable): the id clawt_usage_origin_classify() gave
 *
 * The key a ledger grouped by origin uses: "operator", "untagged", or
 * the kind and the id, as "routine:daily-brief" or "agent:chief".
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_usage_origin_label(ClawtUsageOriginKind kind, const gchar *id);

#define CLAWT_TYPE_USAGE_ORIGIN (clawt_usage_origin_get_type())

/**
 * ClawtUsageOrigin:
 * @kind: what started the turn
 * @id: (nullable): which one, as clawt_usage_origin_classify() names it
 * @sender: (nullable): the delivery's sender as the mailbox had it
 * @task_id: (nullable): the task the turn was about
 * @provider: (nullable): the agent's `model.provider` when it ran
 * @model: (nullable): the model the delivery asked for, if any
 *
 * Everything clawtilla knows about one turn that libreclaw's
 * `token_usage` row does not say.
 */
typedef struct {
	ClawtUsageOriginKind kind;
	gchar               *id;
	gchar               *sender;
	gchar               *task_id;
	gchar               *provider;
	gchar               *model;
} ClawtUsageOrigin;

GType clawt_usage_origin_get_type(void);

/**
 * clawt_usage_origin_new:
 * @kind: what started the turn
 * @id: (nullable): which one
 *
 * Returns: (transfer full): a new #ClawtUsageOrigin with the other
 *   fields unset
 */
ClawtUsageOrigin *clawt_usage_origin_new(ClawtUsageOriginKind kind,
					 const gchar         *id);

/**
 * clawt_usage_origin_copy:
 * @self: a #ClawtUsageOrigin
 *
 * Returns: (transfer full): a deep copy
 */
ClawtUsageOrigin *clawt_usage_origin_copy(const ClawtUsageOrigin *self);

/**
 * clawt_usage_origin_free:
 * @self: (transfer full) (nullable): a #ClawtUsageOrigin
 */
void clawt_usage_origin_free(ClawtUsageOrigin *self);

/* ── Whether a turn has a price ──────────────────────────────────── */

/**
 * clawt_usage_turn_is_priced:
 * @cost_micros: the recorded cost of one turn
 * @input_tokens: its input tokens
 * @output_tokens: its output tokens
 *
 * Whether a turn's recorded cost is a figure or a placeholder.
 *
 * Negative is unknown -- `-1` is the sentinel all the way through
 * ai-glib and libreclaw.  So is zero against tokens: libreclaw records a
 * turn with no reported cost and no price table as 0, which is the same
 * number a free turn would carry and must not be read as one.  An
 * unpriced turn is counted, and kept out of every sum.
 *
 * Returns: %TRUE when @cost_micros is a price
 */
gboolean clawt_usage_turn_is_priced(gint64 cost_micros, gint64 input_tokens,
				    gint64 output_tokens);

/* ── Where the tags are kept ─────────────────────────────────────── */

/**
 * clawt_usage_ledger_path:
 * @state_dir: an agent's state directory
 *
 * The file clawtilla keeps each turn's origin in, beside libreclaw's
 * database inside `sessions/`.  Beside it on purpose: `/reset` moves that
 * directory aside and the next database numbers its rows from 1 again,
 * so tags keyed on the old row ids must go with the rows they describe.
 *
 * Returns: (transfer full): `<state_dir>/sessions/clawtilla-ledger.db`
 */
gchar *clawt_usage_ledger_path(const gchar *state_dir);

/**
 * clawt_usage_ledger_path_beside:
 * @db_path: an agent's libreclaw database, from clawt_usage_database_path()
 *
 * Returns: (transfer full): the ledger file in the same directory
 */
gchar *clawt_usage_ledger_path_beside(const gchar *db_path);

/**
 * clawt_usage_ledger_record:
 * @ledger_path: the agent's ledger file
 * @usage_id: the `token_usage` row id the tag describes
 * @recorded_at: that row's unix seconds
 * @room: (nullable): the room the turn ran in
 * @origin: what started it
 * @error: (out) (optional): return location for a #GError
 *
 * Writes one tag, creating the file and migrating its table as needed.
 * Writing a tag for a row that already has one replaces it.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_usage_ledger_record(const gchar            *ledger_path,
				   gint64                  usage_id,
				   gint64                  recorded_at,
				   const gchar            *room,
				   const ClawtUsageOrigin *origin,
				   GError                **error);

/* ── Grouping ────────────────────────────────────────────────────── */

/**
 * clawt_usage_group_count:
 *
 * The ways the Spending page and `clawtilla cost --by` group the
 * ledger: agent, model, origin and day.  One table, walked with _nth by
 * every client.
 *
 * Returns: how many there are
 */
guint clawt_usage_group_count(void);

/**
 * clawt_usage_group_nth:
 * @n: an index below clawt_usage_group_count()
 *
 * Returns: (transfer none): the nick: "agent", "model", "origin", "day"
 */
const gchar *clawt_usage_group_nth(guint n);

/**
 * clawt_usage_group_label:
 * @n: an index below clawt_usage_group_count()
 *
 * Returns: (transfer none): what a selector calls it: "Agent", "Model"...
 */
const gchar *clawt_usage_group_label(guint n);

/**
 * clawt_usage_group_index:
 * @nick: (nullable): a nick from clawt_usage_group_nth()
 *
 * Returns: its index, or -1 when it is not one
 */
gint clawt_usage_group_index(const gchar *nick);

#define CLAWT_TYPE_USAGE_LEDGER (clawt_usage_ledger_get_type())

G_DECLARE_FINAL_TYPE(ClawtUsageLedger, clawt_usage_ledger, CLAWT, USAGE_LEDGER,
		     GObject)

/**
 * clawt_usage_ledger_new:
 * @by: a nick from clawt_usage_group_nth(); anything else is "agent"
 *
 * An accumulator of turns into groups, and the `usage.ledger` reply it
 * makes.
 *
 * Returns: (transfer full): a new #ClawtUsageLedger
 */
ClawtUsageLedger *clawt_usage_ledger_new(const gchar *by);

/**
 * clawt_usage_ledger_get_by:
 * @self: a #ClawtUsageLedger
 *
 * Returns: (transfer none): the grouping it was made with
 */
const gchar *clawt_usage_ledger_get_by(ClawtUsageLedger *self);

/**
 * clawt_usage_ledger_add_turn:
 * @self: a #ClawtUsageLedger
 * @agent_id: whose turn
 * @recorded_at: when, unix seconds
 * @model: (nullable): the model it ran on
 * @origin_label: (nullable): its clawt_usage_origin_label(), %NULL for
 *   one with no tag
 * @input_tokens: input tokens
 * @output_tokens: output tokens
 * @cost_micros: the recorded cost; see clawt_usage_turn_is_priced()
 *
 * Counts one turn into its group.
 */
void clawt_usage_ledger_add_turn(ClawtUsageLedger *self,
				 const gchar      *agent_id,
				 gint64            recorded_at,
				 const gchar      *model,
				 const gchar      *origin_label,
				 gint64            input_tokens,
				 gint64            output_tokens,
				 gint64            cost_micros);

/**
 * clawt_usage_ledger_add_agent:
 * @self: a #ClawtUsageLedger
 * @agent_id: the agent
 * @state_dir: its state directory
 * @since: unix seconds to count from, 0 for everything
 * @error: (out) (optional): return location for a #GError
 *
 * Reads an agent's `token_usage` rows through libreclaw and its tags
 * from the ledger file, and counts each row.  A missing database or
 * ledger is no history, and neither is created.
 *
 * Returns: %TRUE if both could be read (or were not there)
 */
gboolean clawt_usage_ledger_add_agent(ClawtUsageLedger *self,
				      const gchar      *agent_id,
				      const gchar      *state_dir,
				      gint64            since,
				      GError          **error);

/**
 * clawt_usage_ledger_to_json:
 * @self: a #ClawtUsageLedger
 *
 * The reply body: `by`, `groups` (each with `key`, `turns`,
 * `unpriced_turns`, `input_tokens`, `output_tokens`, `cost_micros`) and
 * a `total` of the same shape without `key`.  Days run oldest first;
 * everything else most expensive first.
 *
 * Returns: (transfer full): a JSON object node
 */
JsonNode *clawt_usage_ledger_to_json(ClawtUsageLedger *self);

/* ── Reading the reply, as every client does ─────────────────────── */

/**
 * clawt_usage_ledger_payload:
 * @since: unix seconds, 0 for everything
 * @by: a nick from clawt_usage_group_nth()
 * @agent: (nullable): one agent only
 *
 * Returns: (transfer full): a `usage.ledger` request payload
 */
JsonNode *clawt_usage_ledger_payload(gint64 since, const gchar *by,
				     const gchar *agent);

/**
 * clawt_usage_ledger_cost_label:
 * @group: one element of `groups`, or the `total`
 *
 * Returns: (transfer full): the cost as clawt_usage_format_cost() spells
 *   it, or "unpriced" when every turn in it has no price -- "$0.00"
 *   there would say free, which nobody knows
 */
gchar *clawt_usage_ledger_cost_label(JsonObject *group);

/**
 * clawt_usage_ledger_row_subtitle:
 * @group: one element of `groups`
 *
 * Returns: (transfer full): "12 turns · 3 unpriced · 4,210 tokens out",
 *   leaving out the unpriced part when there are none
 */
gchar *clawt_usage_ledger_row_subtitle(JsonObject *group);

/**
 * clawt_usage_ledger_unpriced_line:
 * @reply: a `usage.ledger` reply object
 *
 * Returns: (transfer full) (nullable): "3 turns have no recorded price
 *   and are not in the total", or %NULL when every turn has one
 */
gchar *clawt_usage_ledger_unpriced_line(JsonObject *reply);

/**
 * clawt_usage_csv_cell:
 * @value: (nullable): one cell's text
 *
 * One CSV cell, safe to open in a spreadsheet.
 *
 * A cell beginning with `=`, `+`, `-`, `@`, a tab or a carriage return
 * is a formula to a spreadsheet, and a model name, a routine id or a
 * room is text somebody else chose -- so it gets a leading `'`.  Then a
 * cell holding a comma, a quote, a newline or an edge space is quoted,
 * with its quotes doubled.
 *
 * Returns: (transfer full): the cell
 */
gchar *clawt_usage_csv_cell(const gchar *value);

/**
 * clawt_usage_ledger_csv:
 * @reply: a `usage.ledger` reply object
 *
 * The groups as CSV: a header naming the grouping, then one line per
 * group with `turns`, `unpriced_turns`, `input_tokens`, `output_tokens`
 * and `cost_usd`.  No total line, so a spreadsheet's own sum is not
 * doubled.  A group with no priced turn has an empty cost: unknown, not
 * zero.  Lines end in CRLF, as RFC 4180 has it.
 *
 * Returns: (transfer full): the document
 */
gchar *clawt_usage_ledger_csv(JsonObject *reply);

/**
 * clawt_usage_ledger_csv_name:
 * @reply: a `usage.ledger` reply object
 * @now: unix seconds
 *
 * Returns: (transfer full): a file name to suggest, as
 *   "clawtilla-spending-by-model-2026-11-14.csv"
 */
gchar *clawt_usage_ledger_csv_name(JsonObject *reply, gint64 now);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtUsageOrigin, clawt_usage_origin_free)

G_END_DECLS
