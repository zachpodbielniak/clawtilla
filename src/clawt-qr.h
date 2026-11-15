/*
 * clawt-qr.h - QR codes, for pairing a phone by camera
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The encoding is libqrencode's, and libqrencode is optional at build
 * time: without it every function here is still present and reports
 * %CLAWT_ERROR_NOT_SUPPORTED, so a client calls the same API either way
 * and falls back to showing the text.  That keeps the clients free of
 * `#ifdef`s -- the half nobody built is the half that rots -- and the
 * GIR the same whichever way the library was configured.
 *
 * Rendering for a toolkit stays in that toolkit's client: the library
 * hands out the module matrix (clawt_qr_encode()), and draws only what
 * needs no toolkit -- terminal text and SVG.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_QR_QUIET_ZONE:
 *
 * Light modules around the code on every side, as the QR specification
 * asks.  Scanners find the finder patterns by the contrast at their
 * edge, and a code drawn flush against a dark terminal or a coloured
 * card often does not scan at all.
 */
#define CLAWT_QR_QUIET_ZONE (4)

/**
 * clawt_qr_is_available:
 *
 * Whether this build can draw QR codes -- whether libqrencode was found
 * when it was configured.
 *
 * Returns: %TRUE if clawt_qr_encode() can succeed
 */
gboolean clawt_qr_is_available(void);

/**
 * clawt_qr_encode:
 * @text: what the code should carry
 * @out_size: (out): modules per side, without any quiet zone
 * @error: (out) (optional): return location for a #GError
 *
 * Encodes @text at error-correction level M, the level a phone camera
 * reads reliably off a screen without the code growing a version for a
 * URI of this length.
 *
 * The matrix is row-major, one byte per module, 1 for dark and 0 for
 * light, with no quiet zone: the caller adds %CLAWT_QR_QUIET_ZONE on the
 * scale it draws at.
 *
 * Returns: (transfer full) (nullable): @out_size * @out_size bytes, or
 *   %NULL with %CLAWT_ERROR_NOT_SUPPORTED when built without libqrencode
 *   and %CLAWT_ERROR_INVALID_ARGUMENT when @text does not fit
 */
GBytes *clawt_qr_encode(const gchar  *text,
                        guint        *out_size,
                        GError      **error);

/**
 * clawt_qr_render_terminal:
 * @text: what the code should carry
 * @light_background: %TRUE for a terminal drawing dark text on a light
 *   background
 * @error: (out) (optional): return location for a #GError
 *
 * The code as UTF-8 text, two modules per character cell with the half
 * blocks `▀`, `▄`, `█` and a space, quiet zone included, each line ending
 * in a newline.
 *
 * A block is drawn in the terminal's text colour, so which modules get
 * one depends on the background: on a dark terminal (the default) the
 * *light* modules are the blocks, and the quiet zone is a solid frame;
 * on a light one the dark modules are.  Drawn the other way round, a
 * code comes out inverted, which many phone scanners will not read.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL as
 *   clawt_qr_encode() fails
 */
gchar *clawt_qr_render_terminal(const gchar  *text,
                                gboolean      light_background,
                                GError      **error);

/**
 * clawt_qr_render_svg:
 * @text: what the code should carry
 * @error: (out) (optional): return location for a #GError
 *
 * The code as a standalone `<svg>` element: a white square and one black
 * path, in a `viewBox` measured in modules with the quiet zone included,
 * so it scales to any size without blurring.  Sized by whoever places
 * it; it carries no `width` or `height`.
 *
 * The markup holds numbers and fixed text only -- nothing from @text
 * appears in it -- so it is safe to place in a page unescaped.
 *
 * Returns: (transfer full) (nullable): the markup, or %NULL as
 *   clawt_qr_encode() fails
 */
gchar *clawt_qr_render_svg(const gchar  *text,
                           GError      **error);

G_END_DECLS
