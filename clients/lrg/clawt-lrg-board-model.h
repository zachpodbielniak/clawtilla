/*
 * clawt-lrg-board-model.h - Rows the agent board draws
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The Board and Timeline in clawtilla-gtk and clawtilla-lrg both call
 * this.  It includes no libregnum and no GTK header: tests feed it a
 * canned task.board reply and read the columns, chips and timeline
 * spans back.  Which column a card is in is the daemon's decision
 * (clawt_board_classify()); this only shapes that reply for a view.
 */

#pragma once

#include "clawt-lrg-model.h"

G_BEGIN_DECLS

/**
 * ClawtLrgBoardView:
 * @CLAWT_LRG_BOARD_VIEW_LIST: today's task list
 * @CLAWT_LRG_BOARD_VIEW_BOARD: columns, cards, lanes
 * @CLAWT_LRG_BOARD_VIEW_TIMELINE: one bar per card for the window
 *
 * The switch on the Tasks page.  List is the page that already existed.
 */
typedef enum {
    CLAWT_LRG_BOARD_VIEW_LIST = 0,
    CLAWT_LRG_BOARD_VIEW_BOARD,
    CLAWT_LRG_BOARD_VIEW_TIMELINE
} ClawtLrgBoardView;

/**
 * ClawtLrgBoardChip:
 * @worker: the assignee
 * @title: the child task's title, one line
 * @initials: one or two letters, for a face that has no picture
 * @state: the state nick
 * @elapsed_label: how long it has been going
 * @after_label: (nullable): "waits on X", sibling chips only
 * @text: initials, state, elapsed and @after_label, the line a view draws
 * @elapsed: seconds
 *
 * One worker chip.  Freed with its card.
 */
typedef struct {
    gchar *worker;
    gchar *title;
    gchar *initials;
    gchar *state;
    gchar *elapsed_label;
    gchar *after_label;
    gchar *text;
    gint64 elapsed;
} ClawtLrgBoardChip;

/**
 * ClawtLrgBoardCard:
 * @id: the task id
 * @title: one line
 * @state: the state nick
 * @column: the column id
 * @repo: (nullable): the repository's last path component
 * @note: the last progress note, one line, ellipsized
 * @elapsed_label: how long it has been going
 * @cost_label: (nullable): what it cost, or %NULL when the reply has none
 * @ticket_label: (nullable): "#N"
 * @ticket_url: (nullable): the VENTURE page
 * @review_wait_label: (nullable): how long a verdict has been waited for
 * @review_for: (nullable): who the verdict is waiting on
 * @blocks_line: (nullable): why it is blocked, one line
 * @quiet: whether the quiet rule has flagged it
 * @chips: (element-type ClawtLrgBoardChip): the tasks it handed on
 *
 * One card.  Freed with its column.
 */
typedef struct {
    gchar     *id;
    gchar     *title;
    gchar     *state;
    gchar     *column;
    gchar     *repo;
    gchar     *note;
    gchar     *elapsed_label;
    gchar     *cost_label;
    gchar     *ticket_label;
    gchar     *ticket_url;
    gchar     *review_wait_label;
    gchar     *review_for;
    gchar     *blocks_line;
    gboolean   quiet;
    GPtrArray *chips;
} ClawtLrgBoardCard;

/**
 * ClawtLrgBoardColumn:
 * @id: the column id
 * @label: the heading
 * @collapsed: %TRUE for Done, which starts closed
 * @cards: (element-type ClawtLrgBoardCard): display order
 *
 * One of the seven columns.  An empty one is still here.
 */
typedef struct {
    gchar     *id;
    gchar     *label;
    gboolean   collapsed;
    GPtrArray *cards;
} ClawtLrgBoardColumn;

/**
 * ClawtLrgBoardHeader:
 * @agent: the agent id
 * @name: the display name
 * @state: the live state nick
 * @team: (nullable): its team
 * @step_text: (nullable): what the running turn is doing
 * @current_id: (nullable): the task that turn is on
 * @current_title: (nullable): that task's title
 * @activity_label: (nullable): "busy 3m" or "idle 12m"
 * @counts_line: backlog, in flight, in review, done
 * @mailbox: how many items wait
 * @backlog: the backlog count
 * @in_flight: planning + delegated + gating
 * @in_review: the review count
 * @done: finished inside the window
 *
 * The strip above the columns, and the agent page's status card.
 */
typedef struct {
    gchar *agent;
    gchar *name;
    gchar *state;
    gchar *team;
    gchar *step_text;
    gchar *current_id;
    gchar *current_title;
    gchar *activity_label;
    gchar *counts_line;
    guint  mailbox;
    guint  backlog;
    guint  in_flight;
    guint  in_review;
    guint  done;
} ClawtLrgBoardHeader;

/**
 * ClawtLrgBoardLane:
 * @agent: the member
 * @name: the display name
 * @state: the live state nick
 * @current_id: (nullable): its current task
 * @current_title: (nullable): that task's title
 * @step_text: (nullable): its running turn's step
 * @queue_label: queued, running, review, mailbox
 * @activity_label: (nullable): busy or idle
 *
 * One worker lane.
 */
typedef struct {
    gchar *agent;
    gchar *name;
    gchar *state;
    gchar *current_id;
    gchar *current_title;
    gchar *step_text;
    gchar *queue_label;
    gchar *activity_label;
} ClawtLrgBoardLane;

/**
 * ClawtLrgBoardBar:
 * @task_id: the task a click opens
 * @label: the title, indented by @depth in the view
 * @state: the state nick the colour comes from
 * @tone: a badge tone
 * @depth: 0 for a card, 1 for a chip under it
 * @x0: the start, 0..1 across the axis
 * @x1: the end
 * @from: the span's start, a Unix time
 * @to: the span's end
 *
 * One bar of the timeline.  A card's children sit under it.
 */
typedef struct {
    gchar   *task_id;
    gchar   *label;
    gchar   *state;
    gchar   *tone;
    gint     depth;
    gdouble  x0;
    gdouble  x1;
    gint64   from;
    gint64   to;
} ClawtLrgBoardBar;

/**
 * ClawtLrgBoard:
 * @header: the strip
 * @columns: (element-type ClawtLrgBoardColumn): all seven, in order
 * @lanes: (element-type ClawtLrgBoardLane): the team's members
 * @bars: (element-type ClawtLrgBoardBar): the timeline, cards then children
 * @now: the reply's clock
 * @since: the window's start
 * @axis_from: the timeline's left edge
 * @axis_to: the timeline's right edge
 * @now_x: where "now" sits, 0..1
 * @window_spec: "today", or `<N>h` / `<N>d`
 * @agent: whose board
 * @team: (nullable): whose lanes
 *
 * One task.board reply, ready to draw.  Freed with
 * clawt_lrg_board_free().
 */
typedef struct {
    ClawtLrgBoardHeader header;
    GPtrArray          *columns;
    GPtrArray          *lanes;
    GPtrArray          *bars;
    gint64              now;
    gint64              since;
    gint64              axis_from;
    gint64              axis_to;
    gdouble             now_x;
    gchar              *window_spec;
    gchar              *agent;
    gchar              *team;
} ClawtLrgBoard;

void clawt_lrg_board_free(ClawtLrgBoard *board);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtLrgBoard, clawt_lrg_board_free)

guint clawt_lrg_board_view_count(void);

const gchar *clawt_lrg_board_view_label(ClawtLrgBoardView view);

/**
 * clawt_lrg_board_parse:
 * @board: (nullable): a task.board reply object
 *
 * Returns: (transfer full) (nullable): the rows, or %NULL when @board
 *   is %NULL
 */
ClawtLrgBoard *clawt_lrg_board_parse(JsonObject *board);

/**
 * clawt_lrg_board_column:
 * @board: a parsed board
 * @id: a column id
 *
 * Returns: (transfer none) (nullable): that column
 */
ClawtLrgBoardColumn *clawt_lrg_board_column(ClawtLrgBoard *board,
                                            const gchar   *id);

gboolean clawt_lrg_board_column_starts_collapsed(const gchar *id);

const gchar *clawt_lrg_board_state_tone(const gchar *state);

gchar *clawt_lrg_board_initials(const gchar *name);

gchar *clawt_lrg_board_one_line(const gchar *text, guint max_chars);

gchar *clawt_lrg_board_wait_label(gint64 seconds);

gchar *clawt_lrg_board_chip_text(const gchar *initials,
                                 const gchar *state,
                                 const gchar *elapsed,
                                 const gchar *after_label);

/**
 * clawt_lrg_board_ticket_url:
 * @ticket: (nullable): a card's `ticket` object
 *
 * The page a person opens.  When the object carries `base` and `id`,
 * the link is clawt_venture_record_url(); otherwise the `url` the
 * daemon already stored, which is that helper's output.
 *
 * Returns: (transfer full) (nullable): the URL
 */
gchar *clawt_lrg_board_ticket_url(JsonObject *ticket);

/**
 * clawt_lrg_board_axis:
 * @since: the window's start
 * @now: the clock
 * @spec: (nullable): the window spec
 * @from: (out): the timeline's left edge
 * @to: (out): the timeline's right edge
 * @now_x: (out): where "now" sits, 0..1
 *
 * Today runs from local midnight to the next midnight, so "now" is a
 * mark on the day rather than the right-hand edge.  Any other window
 * runs from @since to @now.
 */
void clawt_lrg_board_axis(gint64 since, gint64 now, const gchar *spec,
                          gint64 *from, gint64 *to, gdouble *now_x);

/**
 * clawt_lrg_board_span:
 * @axis_from: the timeline's left edge
 * @axis_to: the timeline's right edge
 * @bar_from: the bar's start
 * @bar_to: the bar's end
 * @x0: (out): the start, 0..1
 * @x1: (out): the end
 *
 * Where a span sits on the axis.  A span that misses the axis is
 * %FALSE and both outputs are 0.
 *
 * Returns: %TRUE when the span overlaps the axis
 */
gboolean clawt_lrg_board_span(gint64 axis_from, gint64 axis_to,
                              gint64 bar_from, gint64 bar_to,
                              gdouble *x0, gdouble *x1);

/**
 * clawt_lrg_board_bar_cells:
 * @x0: the start, 0..1
 * @x1: the end
 * @cells: how many cells
 *
 * A text bar for a view with no canvas: full blocks over the span,
 * light shade around it.  A span that rounds to nothing still gets
 * one cell.
 *
 * Returns: (transfer full): the bar
 */
gchar *clawt_lrg_board_bar_cells(gdouble x0, gdouble x1, guint cells);

gboolean clawt_lrg_board_event_refreshes(const gchar *kind);

gboolean clawt_lrg_board_event_is_step(const gchar *kind);

/**
 * clawt_lrg_board_step_line:
 * @kind_nick: (nullable): a step kind nick
 * @agent: (nullable): who
 * @tool: (nullable): the tool
 * @text: (nullable): the prose
 * @detail: (nullable): the tool's preview
 * @failed: whether the tool failed
 *
 * What a header shows for one turn.step: clawt_turn_step_summary(),
 * so GTK and lrg cannot write a second sentence for it.
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_lrg_board_step_line(const gchar *kind_nick,
                                 const gchar *agent,
                                 const gchar *tool,
                                 const gchar *text,
                                 const gchar *detail,
                                 gboolean     failed);

/**
 * clawt_lrg_board_patch_step:
 * @board: a parsed board
 * @agent: the event's subject
 * @line: the new step line
 * @at: when, a Unix time
 *
 * Writes @line onto the header or a lane when @agent is that one.
 * A burst of steps does not ask task.board again.
 *
 * Returns: %TRUE when a row changed
 */
gboolean clawt_lrg_board_patch_step(ClawtLrgBoard *board,
                                    const gchar   *agent,
                                    const gchar   *line,
                                    gint64         at);

G_END_DECLS
