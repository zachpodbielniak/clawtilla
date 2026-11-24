/*
 * clawt-office-sheet.h - A character's sprite sheet, cut from its frames
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_OFFICE_SHEET_H
#define CLAWT_OFFICE_SHEET_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * clawt_office_sheet_available:
 *
 * Whether this build can draw a sheet: it needs cairo, which comes with
 * the optional PDF support (POPPLER=1).  Without it a generated
 * character is its front view, standing still.
 *
 * Returns: %TRUE when clawt_office_sheet_compose() can work
 */
gboolean clawt_office_sheet_available(void);

/**
 * clawt_office_sheet_compose:
 * @rows: (element-type GPtrArray): one array of PNG #GBytes per row, in
 *   order; a row may be empty
 * @out_frame_width: (out): the width of one cell
 * @out_frame_height: (out): the height of one cell
 * @error: (out) (optional): where a failure is written
 *
 * Lays frames out on a grid the office draws from: one row per
 * animation, every cell the size of the largest frame, each frame
 * centred in its cell on a transparent background.
 *
 * Returns: (transfer full) (nullable): the sheet as a PNG, or %NULL
 */
GBytes *clawt_office_sheet_compose(GPtrArray  *rows,
                                   guint      *out_frame_width,
                                   guint      *out_frame_height,
                                   GError    **error);

/**
 * clawt_office_sheet_walk_layout:
 * @rows: (element-type GPtrArray): the rows clawt_office_sheet_compose()
 *   drew, in the order idle, walk_down, walk_up, walk_left, walk_right
 * @fps: frames a second for the walk
 *
 * The animations object clawt_office_character_set_sheet() takes for a
 * sheet drawn in that order.  An empty walk row is left out, so the
 * renderer falls back to the idle frame rather than drawing nothing.
 *
 * Returns: (transfer full): the animations
 */
JsonObject *clawt_office_sheet_walk_layout(GPtrArray *rows,
                                           guint      fps);

G_END_DECLS

#endif /* CLAWT_OFFICE_SHEET_H */
