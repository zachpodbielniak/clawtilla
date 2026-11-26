/*
 * clawt-office-board.h - The office's provider usage board, laid out
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the drawn office shows of `usage.providers`, in two parts that
 * know nothing of GL or a toolkit, so the tests check the code every
 * renderer runs:
 *
 *  - the model (clawt_office_board_new()): every login and every window,
 *    worded and levelled by the clawt_usage_*() calls the Fleet page
 *    uses -- title, value, reset, tone, and which window is the
 *    tightest.  Nothing is re-derived here; the web client sends this
 *    model to the browser as it stands.
 *  - the layout (clawt_office_board_layout_world() and
 *    clawt_office_board_layout_panel()): where each box, meter and word
 *    goes, measured with the renderer's own font through a callback, and
 *    ellipsized to its column.  The board on the wall never leaves its
 *    zone; the details panel never leaves the view.
 */

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#ifndef CLAWT_OFFICE_BOARD_H
#define CLAWT_OFFICE_BOARD_H

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_OFFICE_BOARD (clawt_office_board_get_type())
#define CLAWT_TYPE_OFFICE_BOARD_LAYOUT (clawt_office_board_layout_get_type())

/**
 * CLAWT_OFFICE_BOARD_MIN_TEXT:
 *
 * The smallest text the board draws, in logical pixels (times the device
 * scale): the native size of raylib's pixel font, below which its glyphs
 * are scaled down into mush.  The wall board shows words only where they
 * fit at this size; elsewhere it shows gauges.
 */
#define CLAWT_OFFICE_BOARD_MIN_TEXT (10.0f)

/**
 * CLAWT_OFFICE_BOARD_PANEL_TEXT:
 *
 * The details panel's text size, in logical pixels (times the device
 * scale).
 */
#define CLAWT_OFFICE_BOARD_PANEL_TEXT (10.0f)

/**
 * ClawtOfficeBoardWindow:
 * @label: the window's name ("Session (5h)")
 * @value: its figure (clawt_usage_window_value_label())
 * @reset: (nullable): when it renews (clawt_usage_window_reset_label())
 * @tone: (transfer none): `good`, `warn`, `bad` or `neutral`
 *   (clawt_allowance_level_tone() of clawt_usage_window_level())
 * @balance: a currency or credit figure: drawn as text, never a meter
 * @has_left: the source said how much is left
 * @left: how much is left, 0 to 100, when @has_left
 *
 * One allowance window, worded.
 */
typedef struct {
    gchar       *label;
    gchar       *value;
    gchar       *reset;
    const gchar *tone;
    gboolean     balance;
    gboolean     has_left;
    gdouble      left;
} ClawtOfficeBoardWindow;

/**
 * ClawtOfficeBoardLogin:
 * @title: clawt_usage_login_title()
 * @initial: the first letter of @title, upper case: the provider's mark
 *   on the wall
 * @status: clawt_usage_login_status_label(): when it was read, or why not
 * @tone: (transfer none): the tightest window's tone, `neutral` without one
 * @tightest: the index in @windows of clawt_usage_login_tightest_window(),
 *   or -1 when the login has no meter
 * @windows: (element-type ClawtOfficeBoardWindow): every window, in the
 *   provider's order
 *
 * One provider login, worded.
 */
typedef struct {
    gchar       *title;
    gchar       *initial;
    gchar       *status;
    const gchar *tone;
    gint         tightest;
    GPtrArray   *windows;
} ClawtOfficeBoardLogin;

/**
 * ClawtOfficeBoard:
 * @logins: (element-type ClawtOfficeBoardLogin): every login
 *
 * `usage.providers`, worded for the office.  Reference counted: see
 * clawt_office_board_ref() and clawt_office_board_free().
 */
typedef struct {
    GPtrArray *logins;
    /*< private >*/
    gint       ref_count;
} ClawtOfficeBoard;

GType clawt_office_board_get_type(void) G_GNUC_CONST;

/**
 * clawt_office_board_new:
 * @usage: (nullable): `usage.providers`' answer
 * @now: the current Unix time, in seconds, for the reset labels
 *
 * Every login and every window in @usage, worded by clawt_usage_*().
 *
 * Returns: (transfer full): the board; empty for %NULL or no logins
 */
ClawtOfficeBoard *clawt_office_board_new(JsonObject *usage, gint64 now);

/**
 * clawt_office_board_ref:
 * @self: a board
 *
 * Returns: (transfer full): @self, with one more reference
 */
ClawtOfficeBoard *clawt_office_board_ref(ClawtOfficeBoard *self);

/**
 * clawt_office_board_free:
 * @self: (nullable): a board
 *
 * Drops a reference; the last one frees it.
 */
void clawt_office_board_free(ClawtOfficeBoard *self);

/**
 * clawt_office_board_n_windows:
 * @self: a board
 *
 * Returns: how many windows there are across every login
 */
guint clawt_office_board_n_windows(ClawtOfficeBoard *self);

/**
 * clawt_office_board_to_json:
 * @self: a board
 *
 * The board as the web client's script draws it: `logins`, each with
 * `title`, `initial`, `status`, `tone`, `tightest` and `windows`, each
 * with `label`, `value`, `reset` (when known), `tone`, `kind` (`meter`
 * or `balance`) and `remaining` (when known, never for a balance).
 *
 * Returns: (transfer full): a new object
 */
JsonObject *clawt_office_board_to_json(ClawtOfficeBoard *self);

/**
 * ClawtOfficeBoardMeasure:
 * @text: the text, UTF-8
 * @size: the font size, in pixels
 * @user_data: the renderer's
 *
 * How wide the renderer draws @text at @size.
 *
 * Returns: the width in pixels
 */
typedef gfloat (*ClawtOfficeBoardMeasure)(const gchar *text, gfloat size, gpointer user_data);

/**
 * ClawtOfficeBoardItemKind:
 * @CLAWT_OFFICE_BOARD_ITEM_BACKING: the board's or panel's background
 * @CLAWT_OFFICE_BOARD_ITEM_TRACK: a meter's empty track
 * @CLAWT_OFFICE_BOARD_ITEM_FILL: what is left, in @tone
 * @CLAWT_OFFICE_BOARD_ITEM_BADGE: a provider's mark, a square in @tone
 * @CLAWT_OFFICE_BOARD_ITEM_TEXT: words, top-left at @x, @y
 *
 * What one drawn item is.
 */
typedef enum {
    CLAWT_OFFICE_BOARD_ITEM_BACKING,
    CLAWT_OFFICE_BOARD_ITEM_TRACK,
    CLAWT_OFFICE_BOARD_ITEM_FILL,
    CLAWT_OFFICE_BOARD_ITEM_BADGE,
    CLAWT_OFFICE_BOARD_ITEM_TEXT
} ClawtOfficeBoardItemKind;

/**
 * ClawtOfficeBoardInk:
 * @CLAWT_OFFICE_BOARD_INK_TITLE: a heading or a login's title: brightest
 * @CLAWT_OFFICE_BOARD_INK_LABEL: a window's name
 * @CLAWT_OFFICE_BOARD_INK_VALUE: a figure
 * @CLAWT_OFFICE_BOARD_INK_MUTED: a status line, a reset, "+N more"
 * @CLAWT_OFFICE_BOARD_INK_ON_BADGE: a letter on a badge: dark on its tone
 *
 * Which of the renderer's text colours a text item takes.
 */
typedef enum {
    CLAWT_OFFICE_BOARD_INK_TITLE,
    CLAWT_OFFICE_BOARD_INK_LABEL,
    CLAWT_OFFICE_BOARD_INK_VALUE,
    CLAWT_OFFICE_BOARD_INK_MUTED,
    CLAWT_OFFICE_BOARD_INK_ON_BADGE
} ClawtOfficeBoardInk;

/**
 * ClawtOfficeBoardItem:
 * @kind: what it is
 * @x: its left, in screen pixels
 * @y: its top
 * @width: its width: for text, the measured width of @text
 * @height: its height: for text, @size
 * @size: a text item's font size
 * @room: a text item's column: @width never exceeds it
 * @text: (nullable): a text item's words, already ellipsized
 * @tone: (nullable): `good`, `warn`, `bad` or `neutral` for a fill or a
 *   badge; %NULL otherwise
 * @ink: a text item's colour
 *
 * One thing to draw.  Drawn in order: later items go over earlier ones.
 */
typedef struct {
    ClawtOfficeBoardItemKind kind;
    gfloat               x;
    gfloat               y;
    gfloat               width;
    gfloat               height;
    gfloat               size;
    gfloat               room;
    gchar               *text;
    const gchar         *tone;
    ClawtOfficeBoardInk  ink;
} ClawtOfficeBoardItem;

/**
 * ClawtOfficeBoardDetail:
 * @CLAWT_OFFICE_BOARD_DETAIL_NONE: nothing drawn (no logins, or no room)
 * @CLAWT_OFFICE_BOARD_DETAIL_MARK: one badge in the worst tone: the zone
 *   is too small on screen for a gauge per login
 * @CLAWT_OFFICE_BOARD_DETAIL_GAUGES: a badge and a meter per login, for
 *   its tightest window
 * @CLAWT_OFFICE_BOARD_DETAIL_ROWS: every login's title and every window's
 *   label, meter and figure
 * @CLAWT_OFFICE_BOARD_DETAIL_PANEL: the details panel
 *
 * How much a layout shows.
 */
typedef enum {
    CLAWT_OFFICE_BOARD_DETAIL_NONE,
    CLAWT_OFFICE_BOARD_DETAIL_MARK,
    CLAWT_OFFICE_BOARD_DETAIL_GAUGES,
    CLAWT_OFFICE_BOARD_DETAIL_ROWS,
    CLAWT_OFFICE_BOARD_DETAIL_PANEL
} ClawtOfficeBoardDetail;

/**
 * ClawtOfficeBoardLayout:
 * @detail: how much is shown
 * @x: the left of everything drawn, in screen pixels
 * @y: its top
 * @width: its width
 * @height: its height
 * @hidden: how many logins (gauges) or windows (rows, panel) did not fit
 *   and are counted in a "+N more" instead
 * @items: (element-type ClawtOfficeBoardItem): what to draw, in order
 *
 * A board or panel, placed.  Reference counted: see
 * clawt_office_board_layout_ref() and clawt_office_board_layout_free().
 */
typedef struct {
    ClawtOfficeBoardDetail detail;
    gfloat                 x;
    gfloat                 y;
    gfloat                 width;
    gfloat                 height;
    guint                  hidden;
    GArray                *items;
    /*< private >*/
    gint                   ref_count;
} ClawtOfficeBoardLayout;

GType clawt_office_board_layout_get_type(void) G_GNUC_CONST;

/**
 * clawt_office_board_layout_ref:
 * @self: a layout
 *
 * Returns: (transfer full): @self, with one more reference
 */
ClawtOfficeBoardLayout *clawt_office_board_layout_ref(ClawtOfficeBoardLayout *self);

/**
 * clawt_office_board_layout_free:
 * @self: (nullable): a layout
 *
 * Drops a reference; the last one frees it.
 */
void clawt_office_board_layout_free(ClawtOfficeBoardLayout *self);

/**
 * clawt_office_board_layout_world:
 * @board: a board
 * @zone_x: the left of the `usage_board` zone on screen, in pixels
 * @zone_y: its top
 * @zone_width: its width
 * @zone_height: its height
 * @ui_scale: device pixels per logical pixel (1 or more)
 * @measure: (scope call): the renderer's text width
 * @user_data: for @measure
 *
 * The board on the wall, inside the zone and never outside it.  The most
 * detail that fits wins: rows of words when every login and window fits
 * at %CLAWT_OFFICE_BOARD_MIN_TEXT or larger; otherwise a gauge per
 * login (its initial and a meter for its tightest window), as many as
 * fit at a legible size with "+N" for the rest; otherwise one mark in
 * the worst tone.  So the level of detail follows the zone's size on
 * screen -- the zoom, the window and the device scale together -- rather
 * than a zoom step.
 *
 * Returns: (transfer full): the layout; %CLAWT_OFFICE_BOARD_DETAIL_NONE
 *   with no items when there are no logins or the zone has no area
 */
ClawtOfficeBoardLayout *clawt_office_board_layout_world(ClawtOfficeBoard        *board,
                                                        gfloat                   zone_x,
                                                        gfloat                   zone_y,
                                                        gfloat                   zone_width,
                                                        gfloat                   zone_height,
                                                        gint                     ui_scale,
                                                        ClawtOfficeBoardMeasure  measure,
                                                        gpointer                 user_data);

/**
 * clawt_office_board_layout_panel:
 * @board: a board
 * @docked: %TRUE to dock in the view's top right corner; %FALSE to sit
 *   beside the anchor
 * @anchor_x: the board on screen, which the panel sits beside: its left
 * @anchor_y: its top
 * @anchor_width: its width
 * @anchor_height: its height
 * @view_width: the view, in pixels
 * @view_height: likewise
 * @ui_scale: device pixels per logical pixel (1 or more)
 * @measure: (scope call): the renderer's text width
 * @user_data: for @measure
 *
 * The details panel: a heading, then per login its title and status, and
 * per window its label, a meter (none for a balance), its figure and
 * when it resets.  Columns are as wide as their widest entry, shrunk --
 * the reset column dropped first -- when the panel would be wider than
 * the view, and every text is ellipsized to its column.  Rows that do
 * not fit the view's height are counted in a last "+N more" row.  Beside
 * the anchor it goes right of it, else left, else over it, and is then
 * moved wholly inside the view.
 *
 * Returns: (transfer full): the layout; %CLAWT_OFFICE_BOARD_DETAIL_NONE
 *   with no items when there are no logins
 */
ClawtOfficeBoardLayout *clawt_office_board_layout_panel(ClawtOfficeBoard        *board,
                                                        gboolean                 docked,
                                                        gfloat                   anchor_x,
                                                        gfloat                   anchor_y,
                                                        gfloat                   anchor_width,
                                                        gfloat                   anchor_height,
                                                        gfloat                   view_width,
                                                        gfloat                   view_height,
                                                        gint                     ui_scale,
                                                        ClawtOfficeBoardMeasure  measure,
                                                        gpointer                 user_data);

/**
 * clawt_office_board_ellipsize:
 * @text: (nullable): UTF-8 text
 * @room: the width it must fit, in pixels
 * @size: the font size
 * @measure: (scope call): the renderer's text width
 * @user_data: for @measure
 *
 * @text cut at a character and ended with "…" so it measures @room or
 * less; @text itself when it fits; "" when not even "…" fits.
 *
 * Returns: (transfer full): the text to draw
 */
gchar *clawt_office_board_ellipsize(const gchar             *text,
                                    gfloat                   room,
                                    gfloat                   size,
                                    ClawtOfficeBoardMeasure  measure,
                                    gpointer                 user_data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtOfficeBoard, clawt_office_board_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtOfficeBoardLayout, clawt_office_board_layout_free)

G_END_DECLS

#endif /* CLAWT_OFFICE_BOARD_H */
