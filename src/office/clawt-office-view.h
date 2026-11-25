/*
 * clawt-office-view.h - The arithmetic of a drawn office in a view
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What a client embedding the office and the renderer drawing it have
 * to agree on, with no toolkit and no GL in it, so the tests check the
 * same code both sides run: how tall the view is on a page, the size in
 * device pixels the renderer is asked for, how far the map is zoomed and
 * where it is centred, where name labels go so they do not cover each
 * other, and the one-line JSON a host writes to the renderer's stdin to
 * switch 2D and 3D, follow somebody, zoom, and say the device scale.
 */

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#ifndef CLAWT_OFFICE_VIEW_H
#define CLAWT_OFFICE_VIEW_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_OFFICE_VIEW_MIN_HEIGHT:
 *
 * The shortest the drawing is on a page, in logical pixels: below this
 * a fitted map shows people too small to tell apart, so a short window
 * scrolls the page instead.
 */
#define CLAWT_OFFICE_VIEW_MIN_HEIGHT (320)

/**
 * CLAWT_OFFICE_VIEW_FALLBACK_WIDTH:
 *
 * A width to start the renderer at before the view has been laid out.
 */
#define CLAWT_OFFICE_VIEW_FALLBACK_WIDTH (746)

/**
 * CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT:
 *
 * A height to start at before the page has a height: 16:9 with
 * %CLAWT_OFFICE_VIEW_FALLBACK_WIDTH.
 */
#define CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT (420)

/**
 * CLAWT_OFFICE_VIEW_MAX_DIMENSION:
 *
 * The largest width or height clawtilla-office draws at (its own
 * `--width`/`--height` limit).
 */
#define CLAWT_OFFICE_VIEW_MAX_DIMENSION (8192)

/**
 * CLAWT_OFFICE_VIEW_MIN_DIMENSION:
 *
 * The smallest width or height clawtilla-office draws at.
 */
#define CLAWT_OFFICE_VIEW_MIN_DIMENSION (64)

/**
 * CLAWT_OFFICE_VIEW_MAX_UI_SCALE:
 *
 * The largest device scale the renderer multiplies its text by.
 */
#define CLAWT_OFFICE_VIEW_MAX_UI_SCALE (4)

/**
 * ClawtOfficeViewMode:
 * @CLAWT_OFFICE_VIEW_2D: the map, from above
 * @CLAWT_OFFICE_VIEW_3D: the level, from an orbiting camera
 *
 * How the office is drawn, as a host asks for it.  A world without a 3D
 * level is drawn in 2D whatever is asked.
 */
typedef enum {
    CLAWT_OFFICE_VIEW_2D = 0,
    CLAWT_OFFICE_VIEW_3D = 1
} ClawtOfficeViewMode;

/**
 * ClawtOfficeZoom:
 * @CLAWT_OFFICE_ZOOM_FIT: the whole map in the view, at the largest
 *   whole-number scale that fits (or the fraction that fits, when even
 *   one map pixel per device pixel is too big)
 * @CLAWT_OFFICE_ZOOM_1X: one map pixel per logical pixel
 * @CLAWT_OFFICE_ZOOM_2X: two logical pixels per map pixel
 *
 * How far the 2D map is zoomed.  In 3D it picks the camera's distance:
 * Fit the whole level, 1x and 2x closer in.
 */
typedef enum {
    CLAWT_OFFICE_ZOOM_FIT = 0,
    CLAWT_OFFICE_ZOOM_1X  = 1,
    CLAWT_OFFICE_ZOOM_2X  = 2
} ClawtOfficeZoom;

/**
 * clawt_office_view_mode_nick:
 * @mode: a #ClawtOfficeViewMode
 *
 * Returns: (transfer none): "2d" or "3d"
 */
const gchar *clawt_office_view_mode_nick(ClawtOfficeViewMode mode);

/**
 * clawt_office_view_mode_from_nick:
 * @nick: (nullable): "2d" or "3d", in any case
 * @out: (out): the mode
 *
 * Returns: %TRUE when @nick named a mode; @out is untouched otherwise
 */
gboolean clawt_office_view_mode_from_nick(const gchar         *nick,
                                          ClawtOfficeViewMode *out);

/**
 * clawt_office_zoom_nick:
 * @zoom: a #ClawtOfficeZoom
 *
 * Returns: (transfer none): "fit", "1x" or "2x"
 */
const gchar *clawt_office_zoom_nick(ClawtOfficeZoom zoom);

/**
 * clawt_office_zoom_from_nick:
 * @nick: (nullable): "fit", "1x" or "2x", in any case
 * @out: (out): the zoom
 *
 * Returns: %TRUE when @nick named a zoom; @out is untouched otherwise
 */
gboolean clawt_office_zoom_from_nick(const gchar     *nick,
                                     ClawtOfficeZoom *out);

/**
 * clawt_office_view_height_for_page:
 * @page_height: the height of the page the view is on, in logical
 *   pixels, or 0 or less when it is not known yet
 *
 * How tall the drawing is on a page: three quarters of what the page
 * shows, so the office fills the window and the heading of the roster
 * under it still shows, and never under %CLAWT_OFFICE_VIEW_MIN_HEIGHT.
 * An unknown page is %CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT.
 *
 * Returns: the height in logical pixels
 */
gint clawt_office_view_height_for_page(gint page_height);

/**
 * clawt_office_view_device_size:
 * @width: the view's width in logical pixels
 * @height: its height
 * @scale: the device scale factor (1 or more; less is taken as 1)
 * @out_width: (out): the width the renderer is asked for, in device pixels
 * @out_height: (out): likewise the height
 *
 * The logical size times the scale, so a frame covers the view pixel for
 * pixel.  Past %CLAWT_OFFICE_VIEW_MAX_DIMENSION both sides shrink by the
 * same factor (the frame is then scaled up, keeping its shape); under
 * %CLAWT_OFFICE_VIEW_MIN_DIMENSION a side is raised to it.
 *
 * Returns: %FALSE when @width or @height is 0 or less: nothing to draw,
 *   and the outs are untouched
 */
gboolean clawt_office_view_device_size(gint   width,
                                       gint   height,
                                       gint   scale,
                                       guint *out_width,
                                       guint *out_height);

/**
 * clawt_office_view_clamp_ui_scale:
 * @ui_scale: a device scale factor, from a host
 *
 * Returns: @ui_scale in 1 to %CLAWT_OFFICE_VIEW_MAX_UI_SCALE
 */
gint clawt_office_view_clamp_ui_scale(gint ui_scale);

/**
 * clawt_office_view_map_zoom:
 * @zoom: what was asked for
 * @ui_scale: device pixels per logical pixel
 * @view_width: the view, in device pixels
 * @view_height: likewise
 * @map_width: the map, in map pixels
 * @map_height: likewise
 *
 * Device pixels per map pixel.  Fit is a whole number whenever the map
 * fits at one or more, so pixel art is scaled by repeating pixels, never
 * by smearing them; below one it is the fraction that fits.  1x is
 * @ui_scale and 2x twice it -- one or two logical pixels per map pixel,
 * crisp on any screen, and larger than the view on a small one.
 *
 * Returns: the zoom; 1 for a map with no size
 */
gfloat clawt_office_view_map_zoom(ClawtOfficeZoom zoom,
                                  gint            ui_scale,
                                  gfloat          view_width,
                                  gfloat          view_height,
                                  gfloat          map_width,
                                  gfloat          map_height);

/**
 * clawt_office_view_clamp_centre:
 * @view_width: the view, in device pixels
 * @view_height: likewise
 * @map_width: the map, in map pixels
 * @map_height: likewise
 * @zoom: device pixels per map pixel
 * @centre_x: (inout): the map point wanted in the middle of the view
 * @centre_y: (inout): likewise
 *
 * Keeps a zoomed map in the view: on an axis where the map is smaller
 * than the view it is centred; where it is larger the centre is held
 * far enough in that no edge of the view shows past the map's.
 */
void clawt_office_view_clamp_centre(gfloat  view_width,
                                    gfloat  view_height,
                                    gfloat  map_width,
                                    gfloat  map_height,
                                    gfloat  zoom,
                                    gfloat *centre_x,
                                    gfloat *centre_y);

/**
 * ClawtOfficeLabel:
 * @x: the left of the label where it would be drawn, in screen pixels
 * @y: its top
 * @width: its width
 * @height: its height
 * @pinned: shown where it is whatever it covers (the label under the
 *   pointer)
 * @dy: set by clawt_office_labels_arrange(): how far down to draw it
 * @shown: set by clawt_office_labels_arrange(): whether to draw it at all
 *
 * One name label, for clawt_office_labels_arrange().
 */
typedef struct {
    gfloat   x;
    gfloat   y;
    gfloat   width;
    gfloat   height;
    gboolean pinned;
    gfloat   dy;
    gboolean shown;
} ClawtOfficeLabel;

/**
 * clawt_office_labels_arrange:
 * @labels: (array length=n_labels): the labels, most important first
 * @n_labels: how many
 * @gap: the space kept between two labels, in screen pixels
 * @max_steps: how many times a label may be pushed down
 * @bottom: the bottom of the view: a label is not pushed past it
 *
 * Keeps name labels from covering each other.  Pinned labels go first,
 * where they are.  Each other label, in order, goes where it is if that
 * comes within @gap of no label already placed, else just below the
 * lowest one in its way, and again, for up to @max_steps pushes -- so
 * people at adjacent desks get a short stack of names under them rather
 * than a pile of letters.  A label with no room in reach, or pushed past
 * @bottom, is not drawn: in a crowded or zoomed-out view the first
 * labels win, and the rest show on hover, which pins them.
 *
 * Returns: how many labels are shown
 */
guint clawt_office_labels_arrange(ClawtOfficeLabel *labels,
                                  guint             n_labels,
                                  gfloat            gap,
                                  guint             max_steps,
                                  gfloat            bottom);

/**
 * ClawtOfficeViewControl:
 * @has_mode: whether the line set the mode
 * @mode: the mode, when @has_mode
 * @has_focus: whether the line set whom to follow
 * @focus: (nullable): the participant to follow, %NULL for nobody, when
 *   @has_focus
 * @has_zoom: whether the line set the zoom
 * @zoom: the zoom, when @has_zoom
 * @ui_scale: the device scale, 0 when the line did not say
 *
 * One control line, parsed.  Each member is optional on the wire, so a
 * host may send only what changed.
 */
typedef struct {
    gboolean             has_mode;
    ClawtOfficeViewMode  mode;
    gboolean             has_focus;
    gchar               *focus;
    gboolean             has_zoom;
    ClawtOfficeZoom      zoom;
    gint                 ui_scale;
} ClawtOfficeViewControl;

/**
 * clawt_office_view_control_line:
 * @mode: 2D or 3D
 * @focus: (nullable): the participant the camera follows, %NULL or ""
 *   for nobody
 * @zoom: the zoom
 * @ui_scale: device pixels per logical pixel
 *
 * The line a host writes to an embedded renderer's stdin to say how the
 * office is to be drawn now: `{"mode":"3d","focus":"chief","zoom":"fit",
 * "ui_scale":2}` and a newline.  The whole state every time, so a line
 * lost to nothing in particular is corrected by the next.
 *
 * Returns: (transfer full): the line, newline included
 */
gchar *clawt_office_view_control_line(ClawtOfficeViewMode  mode,
                                      const gchar         *focus,
                                      ClawtOfficeZoom      zoom,
                                      gint                 ui_scale);

/**
 * clawt_office_view_control_parse:
 * @line: one line, without its newline
 * @out: (out caller-allocates): the members it set; clear it with
 *   clawt_office_view_control_clear()
 * @error: return location for a #GError
 *
 * Reads a line clawt_office_view_control_line() wrote, or any JSON object
 * with some of its members.  A member of the wrong type or an unknown
 * mode or zoom is refused, naming it, rather than half-applied: a host
 * and a renderer of different versions should disagree loudly.
 * Unknown members are ignored, for the same reason in the other
 * direction: a newer host's extra field must not stop an older renderer.
 * An empty focus is nobody.
 *
 * Returns: %TRUE when @line was a usable control line; %FALSE with
 *   @error set to %CLAWT_ERROR_INVALID_ARGUMENT otherwise
 */
gboolean clawt_office_view_control_parse(const gchar             *line,
                                         ClawtOfficeViewControl  *out,
                                         GError                 **error);

/**
 * clawt_office_view_control_clear:
 * @control: a parsed line
 *
 * Frees what @control holds and zeroes it.
 */
void clawt_office_view_control_clear(ClawtOfficeViewControl *control);

G_END_DECLS

#endif /* CLAWT_OFFICE_VIEW_H */
