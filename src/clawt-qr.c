/*
 * clawt-qr.c - QR codes, for pairing a phone by camera
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "clawt-qr.h"

#include <errno.h>
#include <string.h>

#ifdef CLAWT_HAVE_QRENCODE
#include <qrencode.h>
#endif

gboolean
clawt_qr_is_available(void)
{
#ifdef CLAWT_HAVE_QRENCODE
    return TRUE;
#else
    return FALSE;
#endif
}

GBytes *
clawt_qr_encode(const gchar *text, guint *out_size, GError **error)
{
#ifdef CLAWT_HAVE_QRENCODE
    QRcode *code;
    guint8 *modules;
    gsize count;
    gsize i;
#endif

    g_return_val_if_fail(text != NULL, NULL);
    g_return_val_if_fail(out_size != NULL, NULL);

    *out_size = 0;

#ifdef CLAWT_HAVE_QRENCODE
    /*
     * Version 0 lets libqrencode pick the smallest that fits; 8-bit mode
     * with case kept, because a URI's percent-escapes and its name are
     * case-sensitive and the alphanumeric mode would upper-case them.
     */
    errno = 0;
    code = QRcode_encodeString(text, 0, QR_ECLEVEL_M, QR_MODE_8, 1);

    if (code == NULL) {
        gint saved = errno;

        if (saved == ERANGE)
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "too long to fit in a QR code");
        else
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "libqrencode could not encode it: %s",
                        g_strerror(saved));
        return NULL;
    }

    /* libqrencode packs flags into the upper bits; bit 0 is dark */
    count = (gsize)code->width * (gsize)code->width;
    modules = g_malloc(count);

    for (i = 0; i < count; i++)
        modules[i] = (guint8)(code->data[i] & 1);

    *out_size = (guint)code->width;
    QRcode_free(code);

    return g_bytes_new_take(modules, count);
#else
    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                        "this build has no QR support: it needs "
                        "libqrencode at build time (Fedora: "
                        "qrencode-devel; qrencode-libs at runtime)");
    return NULL;
#endif
}

/*
 * Whether the module at (@x, @y) is dark, where the coordinates include
 * the quiet zone -- anything outside the matrix is light.
 */
static gboolean
module_dark(const guint8 *modules, guint size, gint x, gint y)
{
    x -= CLAWT_QR_QUIET_ZONE;
    y -= CLAWT_QR_QUIET_ZONE;

    if (x < 0 || y < 0 || x >= (gint)size || y >= (gint)size)
        return FALSE;

    return modules[(gsize)y * size + (gsize)x] != 0;
}

gchar *
clawt_qr_render_terminal(const gchar *text, gboolean light_background,
                         GError **error)
{
    g_autoptr(GBytes) bytes = NULL;
    const guint8 *modules;
    GString *out;
    guint size = 0;
    gint total;
    gint x;
    gint y;

    g_return_val_if_fail(text != NULL, NULL);

    bytes = clawt_qr_encode(text, &size, error);

    if (bytes == NULL)
        return NULL;

    modules = g_bytes_get_data(bytes, NULL);
    total = (gint)size + 2 * CLAWT_QR_QUIET_ZONE;
    out = g_string_new(NULL);

    /*
     * Two rows of modules per line of text, because a character cell is
     * about twice as tall as it is wide: a full block per module would
     * draw the code twice as tall as wide, and scanners want squares.
     */
    for (y = 0; y < total; y += 2) {
        for (x = 0; x < total; x++) {
            gboolean top = module_dark(modules, size, x, y);
            gboolean bottom = (y + 1 < total)
                ? module_dark(modules, size, x, y + 1)
                : FALSE;

            /* a block is "lit" in the terminal's foreground colour */
            if (!light_background) {
                top = !top;
                bottom = !bottom;
            }

            /* past the last row, nothing is drawn on either background */
            if (y + 1 >= total)
                bottom = FALSE;

            if (top && bottom)
                g_string_append(out, "\342\226\210");        /* █ */
            else if (top)
                g_string_append(out, "\342\226\200");        /* ▀ */
            else if (bottom)
                g_string_append(out, "\342\226\204");        /* ▄ */
            else
                g_string_append_c(out, ' ');
        }

        g_string_append_c(out, '\n');
    }

    return g_string_free(out, FALSE);
}

gchar *
clawt_qr_render_svg(const gchar *text, GError **error)
{
    g_autoptr(GBytes) bytes = NULL;
    const guint8 *modules;
    GString *out;
    guint size = 0;
    guint total;
    guint x;
    guint y;

    g_return_val_if_fail(text != NULL, NULL);

    bytes = clawt_qr_encode(text, &size, error);

    if (bytes == NULL)
        return NULL;

    modules = g_bytes_get_data(bytes, NULL);
    total = size + 2 * CLAWT_QR_QUIET_ZONE;
    out = g_string_new(NULL);

    /*
     * crispEdges, so a browser scaling the code to a fractional pixel
     * size does not antialias the seams between modules into grey lines
     * a scanner reads as noise.  The colours are fixed rather than
     * themed: a QR code on a dark theme's background, inverted, is one
     * many scanners will not read.
     */
    g_string_append_printf(out,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %u %u\" "
        "shape-rendering=\"crispEdges\" role=\"img\" "
        "aria-label=\"Pairing QR code\">"
        "<rect width=\"%u\" height=\"%u\" fill=\"#ffffff\"/>"
        "<path fill=\"#000000\" d=\"",
        total, total, total, total);

    /* one subpath per horizontal run, not per module: a tenth the size */
    for (y = 0; y < size; y++) {
        x = 0;

        while (x < size) {
            guint start;

            if (modules[(gsize)y * size + x] == 0) {
                x++;
                continue;
            }

            start = x;

            while (x < size && modules[(gsize)y * size + x] != 0)
                x++;

            g_string_append_printf(out, "M%u %uh%uv1h-%uz",
                                   start + CLAWT_QR_QUIET_ZONE,
                                   y + CLAWT_QR_QUIET_ZONE,
                                   x - start, x - start);
        }
    }

    g_string_append(out, "\"/></svg>");

    return g_string_free(out, FALSE);
}
