/*
 * clawt-board.h - The agent board: one agent's work, column by column
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * "Where is oxpecker right now, and what is still in front of him?"  The
 * task tree, the states, the progress notes, the streamed steps, the
 * mailbox and the VENTURE tickets already answer it between them; this
 * is the one place that puts them together, so the CLI, the GTK client
 * and the web client draw the same board from the same reply
 * (task.board, docs/ipc-protocol.org).
 *
 * Three layers, kept apart on purpose:
 *
 *   - clawt_board_classify() is pure: a task, its children, a clock and a
 *     few facts in, a column out.  Every rule is in it and in
 *     docs/tasks.org, and it is tested over a table.
 *   - clawt_board_build() gathers the facts from a #ClawtTaskManager and
 *     a #ClawtBoardSource (what only the daemon knows: agents' live
 *     state, decisions, waits, spend) and writes the reply.
 *   - clawt_board_render_text() draws that reply for a terminal, so the
 *     text the CLI prints is a view of the JSON and nothing else.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "task/clawt-task.h"
#include "task/clawt-task-manager.h"

G_BEGIN_DECLS

/**
 * CLAWT_BOARD_QUIET_MINUTES:
 *
 * How long a running card may go without a sign of life before it is
 * flagged quiet, when `orchestration.board_quiet_minutes` is not set.
 */
#define CLAWT_BOARD_QUIET_MINUTES (20)

/**
 * CLAWT_BOARD_MEMBER_COLUMNS:
 *
 * The member of a `task.board` reply that lists the seven columns, in
 * order.  It is the wire name, not a measure unit: clients read the
 * reply through this so the name lives in the library alone.
 */
#define CLAWT_BOARD_MEMBER_COLUMNS "columns"

/**
 * ClawtBoardBlocks:
 * @CLAWT_BOARD_BLOCK_NONE: nothing blocks it
 * @CLAWT_BOARD_BLOCK_FAILED: the task itself failed
 * @CLAWT_BOARD_BLOCK_STALLED: the task itself was stopped by a limit
 * @CLAWT_BOARD_BLOCK_CHILD_FAILED: a task it handed on failed, and it has
 *   not reported since
 * @CLAWT_BOARD_BLOCK_CHILD_STALLED: a task it handed on stalled, and it
 *   has not reported since
 * @CLAWT_BOARD_BLOCK_DECISION: a decision for the operator is open on it
 *   or on one of its children, or a child waits on the operator
 * @CLAWT_BOARD_BLOCK_WAITING: its assignee said it waits on somebody
 *   (clawtilla_task_progress `waiting_on`)
 * @CLAWT_BOARD_BLOCK_QUIET: running, and nothing has been heard of it
 *   for the quiet window
 * @CLAWT_BOARD_BLOCK_PHASE: its assignee reported the `blocked` phase
 *
 * Why a card is in the blocked column; any number at once.  Each has a
 * wire spelling (clawt_board_block_nick()).
 */
typedef enum {
    CLAWT_BOARD_BLOCK_NONE          = 0,
    CLAWT_BOARD_BLOCK_FAILED        = 1 << 0,
    CLAWT_BOARD_BLOCK_STALLED       = 1 << 1,
    CLAWT_BOARD_BLOCK_CHILD_FAILED  = 1 << 2,
    CLAWT_BOARD_BLOCK_CHILD_STALLED = 1 << 3,
    CLAWT_BOARD_BLOCK_DECISION      = 1 << 4,
    CLAWT_BOARD_BLOCK_WAITING       = 1 << 5,
    CLAWT_BOARD_BLOCK_QUIET         = 1 << 6,
    CLAWT_BOARD_BLOCK_PHASE         = 1 << 7
} ClawtBoardBlocks;

/**
 * ClawtBoardFacts:
 * @task: the card's task
 * @children: (element-type ClawtTask) (nullable): the tasks @task handed
 *   on, directly, in any state
 * @owner: (nullable): whose board it is; a task assigned to anybody else
 *   is hidden.  %NULL skips the check
 * @now: the clock, a Unix time
 * @done_since: the start of the window, a Unix time: a task that finished
 *   before it is hidden
 * @quiet_minutes: the quiet window, or 0 for no quiet rule
 * @decisions: open operator decisions on @task and its children
 * @wait_pending: whether @owner has a clawtilla wait pending, which is a
 *   reason to hear nothing
 *
 * Everything clawt_board_classify() decides from.  Plain values, so a
 * test writes one in a line.
 */
typedef struct {
    ClawtTask   *task;
    GPtrArray   *children;
    const gchar *owner;
    gint64       now;
    gint64       done_since;
    guint        quiet_minutes;
    guint        decisions;
    gboolean     wait_pending;
} ClawtBoardFacts;

/**
 * clawt_board_classify:
 * @facts: what is known about the card
 * @blocks: (out) (optional): why, when the answer is
 *   %CLAWT_BOARD_BLOCKED
 *
 * The one column a top-level task is drawn in.  Pure: nothing is read
 * but @facts, so the same facts always give the same column.  The rules,
 * in the order they are tried (docs/tasks.org, "The agent board"):
 *
 *   1. Hidden: not @owner's, on the operator's backlog, cancelled, or
 *      finished before @done_since.
 *   2. Done: completed inside the window.
 *   3. Blocked, pinned first: failed or stalled itself (inside the
 *      window); a child failed or stalled and the card has not reported
 *      progress or a phase since; an open operator decision on it or a
 *      child, or a child waiting on the operator; it waits on somebody
 *      itself; the `blocked` phase; or quiet (clawt_board_is_quiet()).
 *   4. Backlog: pending, or held until the tasks it waits for complete.
 *   5. Review: its own result waits for its reviewer.
 *   6. Running, with a phase reported: planning, delegated, review for
 *      `reviewing`, gating for `gating` and `landing`.  A phase overrides
 *      everything below.
 *   7. Running, inferred: review when a child's result waits for @owner's
 *      verdict; delegated when a child has not finished; planning
 *      otherwise.
 *
 * Returns: the column
 */
ClawtBoardColumn clawt_board_classify(const ClawtBoardFacts *facts,
                                      ClawtBoardBlocks      *blocks);

/**
 * clawt_board_is_quiet:
 * @task: a task
 * @children: (element-type ClawtTask) (nullable): the tasks it handed on
 * @now: the clock, a Unix time
 * @quiet_minutes: the quiet window, or 0 for none
 * @wait_pending: whether its assignee has a clawtilla wait pending
 * @quiet_at: (out) (optional): when it is (or would become) quiet, a Unix
 *   time, or 0 when the rule does not apply to it
 *
 * Whether a card has gone quiet: it is `running`, its assignee has no
 * wait pending, every child that has not finished is itself running, and
 * nothing -- no progress or phase report, no streamed step, no state
 * change, on it or on those children -- has been heard for
 * @quiet_minutes.  Not "stalled": that is a task state, a limit stopping
 * the work.  A child still pending, held or in review is waiting on
 * somebody else, so its parent is not quiet.
 *
 * Returns: %TRUE when quiet now
 */
gboolean clawt_board_is_quiet(ClawtTask *task,
                              GPtrArray *children,
                              gint64     now,
                              guint      quiet_minutes,
                              gboolean   wait_pending,
                              gint64    *quiet_at);

/**
 * clawt_board_agent_activity:
 * @turn_running: whether the agent is in a turn
 * @waits_pending: how many clawtilla waits it has pending
 *
 * What an agent is doing, the one rule every surface asks (since
 * 0.78.0): a running turn is %CLAWT_AGENT_ACTIVITY_WORKING whatever is
 * pending; otherwise a pending wait is %CLAWT_AGENT_ACTIVITY_WAITING --
 * it started something in the background and is waiting to be told,
 * which is not the same as having nothing to do; otherwise
 * %CLAWT_AGENT_ACTIVITY_IDLE.  The run state is not an input: a stopped
 * agent's waits are still looked at, and the notice waits in its
 * mailbox, so the answer stands beside #ClawtAgentState rather than
 * replacing it.
 *
 * Returns: the activity
 */
ClawtAgentActivity clawt_board_agent_activity(gboolean turn_running,
                                              guint    waits_pending);

/**
 * clawt_board_agent_activity_nick:
 * @activity: a #ClawtAgentActivity
 *
 * Returns: (transfer none): its spelling on the wire: `idle`, `waiting`
 *   or `working`
 */
const gchar *clawt_board_agent_activity_nick(ClawtAgentActivity activity);

/**
 * clawt_board_waiting_label:
 * @label: (nullable): what it waits for: the newest pending wait's `job`,
 *   else its label
 * @since: when it began waiting, a Unix time, or 0 for not known
 * @now: the clock, a Unix time
 *
 * A waiting agent the way every surface writes it, in one spelling:
 * `waiting: gate g39 in clawt-oxpecker (12m)`.  The age is how long the
 * agent has had a wait pending without a break (`waiting_since` on
 * agent.list), and is left off when @since is 0.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_board_waiting_label(const gchar *label,
                                 gint64       since,
                                 gint64       now);

/**
 * clawt_board_column_nick:
 * @column: a #ClawtBoardColumn
 *
 * Returns: (transfer none): the column's id on the wire, e.g. `delegated`
 */
const gchar *clawt_board_column_nick(ClawtBoardColumn column);

/**
 * clawt_board_column_label:
 * @column: a #ClawtBoardColumn
 *
 * The heading every client draws over the column, in one wording.
 *
 * Returns: (transfer none): the label, e.g. "In review"
 */
const gchar *clawt_board_column_label(ClawtBoardColumn column);

/**
 * clawt_board_block_nick:
 * @block: one #ClawtBoardBlocks flag
 *
 * Returns: (transfer none) (nullable): its spelling on the wire, e.g.
 *   `child_failed`, or %NULL for anything but a single flag
 */
const gchar *clawt_board_block_nick(ClawtBoardBlocks block);

/**
 * clawt_board_parse_window:
 * @spec: (nullable): `today` (or %NULL / empty), `<N>h` or `<N>d`
 * @now: the clock, a Unix time
 * @since: (out): the start of the window, a Unix time
 * @error: (out) (optional): return location for a #GError
 *
 * How far back the done column reaches.  `today` is local midnight;
 * `<N>h` and `<N>d` count back from @now (N from 1 to 90 days' worth).
 * Anything else is %CLAWT_ERROR_INVALID_ARGUMENT.
 *
 * Returns: %TRUE when @spec is a window
 */
gboolean clawt_board_parse_window(const gchar  *spec,
                                  gint64        now,
                                  gint64       *since,
                                  GError      **error);

/**
 * clawt_board_result_refs:
 * @result: (nullable): a task's result
 * @commit: (out) (optional) (transfer full) (nullable): the first commit
 *   it names: a word of 7 to 40 hex digits with at least one digit and
 *   one letter in it
 * @branch: (out) (optional) (transfer full) (nullable): the branch it
 *   names: the word after "branch", or a word shaped like a conventional
 *   branch (`feat/...`, `fix/...`, `clawtilla/...` and the like)
 *
 * What a done card shows of where the work landed.  A heuristic over
 * text an agent wrote, so either may be %NULL; a task's own checkout
 * branch, when it has one, is the better answer for the branch.
 *
 * Returns: %TRUE when either was found
 */
gboolean clawt_board_result_refs(const gchar  *result,
                                 gchar       **commit,
                                 gchar       **branch);

/**
 * clawt_board_timeline:
 * @task: a task
 * @journal: (nullable): its journal, as clawt_task_store_journal() gives
 *   it: objects with `at` and `state`, oldest first
 * @now: the clock, a Unix time
 *
 * When each state the task was in started and ended, and the same for
 * each phase it reported.  From the journal when there is one (every
 * task saved since 0.35.0 has one, a row per state change); otherwise
 * from the task's own stamps -- created, started, finished -- which is
 * all an older task has.  Consecutive rows in one state are one span.
 * The last span has no `to`: it is still going, or the task finished.
 *
 * Returns: (transfer full): an object: `source` (`journal` or `stamps`),
 *   `states` (each `state`, `from`, `to`) and `phases` (each `phase`,
 *   `from`, `to`)
 */
JsonNode *clawt_board_timeline(ClawtTask *task,
                               JsonArray *journal,
                               gint64     now);

/**
 * clawt_board_duration_label:
 * @seconds: a span
 *
 * A span the way the board writes one: `45s`, `12m`, `3h05m`, `2d04h`.
 * A negative span is written as 0s.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_board_duration_label(gint64 seconds);

/**
 * ClawtBoardAgent:
 * @name: (nullable): its display name
 * @state: (nullable): its live state, as #ClawtAgentState spells it
 * @team: (nullable): its team
 * @step_text: (nullable): what its current turn's latest step says
 * @step_tool: (nullable): the tool that step used
 * @step_at: when that step was taken, a Unix time, or 0
 * @turn_task: (nullable): the task its running turn is on
 * @turn_started: when its running turn began, a Unix time, or 0 when it
 *   is not in a turn
 * @last_active: when it was last seen doing anything, a Unix time, or 0
 *   for not known
 * @mailbox: how many items wait in its mailbox
 * @waits_pending: how many clawtilla waits it has pending (since 0.78.0)
 * @wait_label: (nullable): what the newest of them waits for: its `job`
 *   when it was given one, else its label
 * @waiting_since: since when it has had a wait pending without a break,
 *   a Unix time, or 0 when it has none
 *
 * What the board shows of one agent, as a #ClawtBoardSource reports it.
 * The strings are owned by the struct; clawt_board_agent_clear() frees
 * them.
 */
typedef struct {
    gchar  *name;
    gchar  *state;
    gchar  *team;
    gchar  *step_text;
    gchar  *step_tool;
    gint64  step_at;
    gchar  *turn_task;
    gint64  turn_started;
    gint64  last_active;
    guint   mailbox;
    guint   waits_pending;
    gchar  *wait_label;
    gint64  waiting_since;
} ClawtBoardAgent;

/**
 * clawt_board_agent_clear: (skip)
 * @agent: a #ClawtBoardAgent
 *
 * Frees its strings and zeroes it.
 */
void clawt_board_agent_clear(ClawtBoardAgent *agent);

/**
 * ClawtBoardSource: (skip)
 * @describe_agent: (nullable): fills a #ClawtBoardAgent for an agent id;
 *   %FALSE for an agent it does not know
 * @open_decisions: (nullable): how many operator decisions are open on a
 *   task id
 * @wait_pending: (nullable): whether an agent has a clawtilla wait pending
 * @task_cost: (nullable): what a task and everything under it has cost,
 *   in USD, or a negative number for not known
 * @team_members: (nullable): a team's members, by agent id (transfer
 *   full, element-type utf8), or %NULL for no such team
 *
 * What only the daemon knows, asked through callbacks so the board can
 * be built (and tested) without one.  Any may be %NULL: the board then
 * leaves out what it would have said.
 */
typedef struct {
    gboolean   (*describe_agent)(const gchar     *agent_id,
                                 ClawtBoardAgent *out,
                                 gpointer         user_data);
    guint      (*open_decisions)(const gchar *task_id, gpointer user_data);
    gboolean   (*wait_pending)(const gchar *agent_id, gpointer user_data);
    gdouble    (*task_cost)(const gchar *task_id, gpointer user_data);
    GPtrArray *(*team_members)(const gchar *team_id, gpointer user_data);
} ClawtBoardSource;

/**
 * ClawtBoardRequest: (skip)
 * @agent: whose board
 * @team: (nullable): a team whose members get lanes
 * @window: (nullable): the window as asked for, echoed in the reply
 * @done_since: the start of the window, a Unix time
 *   (clawt_board_parse_window())
 * @now: the clock, a Unix time
 * @quiet_minutes: the quiet window, 0 for none
 * @timeline: whether each card and chip carries its timeline
 *
 * What task.board asked for, resolved.
 */
typedef struct {
    const gchar *agent;
    const gchar *team;
    const gchar *window;
    gint64       done_since;
    gint64       now;
    guint        quiet_minutes;
    gboolean     timeline;
} ClawtBoardRequest;

/**
 * clawt_board_build: (skip)
 * @tasks: the task manager
 * @request: what was asked for
 * @source: (nullable): what only the daemon knows
 * @user_data: passed to every @source callback
 *
 * The whole board as task.board answers it (docs/ipc-protocol.org,
 * "task.board"): the header, the columns with their cards and chips, the
 * worker lanes and the totals.  Cards are @request's agent's top-level
 * tasks in the live set -- those assigned to it whose parent is not also
 * its own -- plus, for a window longer than a day, the tasks it
 * completed in it that the store still holds.
 *
 * Returns: (transfer full): the board, an object node
 */
JsonNode *clawt_board_build(ClawtTaskManager        *tasks,
                            const ClawtBoardRequest *request,
                            const ClawtBoardSource  *source,
                            gpointer                 user_data);

/**
 * clawt_board_render_text:
 * @board: a task.board reply's object
 *
 * The board as `clawtilla task board` prints it: a header, then one
 * section per column in the reply's order, each card with its ticket,
 * repository, title, elapsed time, cost and last note, and its worker
 * chips under it; then the lanes.  Times are measured against the
 * reply's own `now`, so the text is a pure view of the JSON.
 *
 * Returns: (transfer full): the text, ending in a newline
 */
gchar *clawt_board_render_text(JsonObject *board);

G_END_DECLS
