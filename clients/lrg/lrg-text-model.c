/*
 * lrg-text-model.c - Plain text, shown through a markdown label
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawt-lrg-model.h"

#include <string.h>

gchar *
clawt_lrg_image_template(const gchar *prefix, const gchar *mime)
{
    const gchar *extension = clawt_avatar_extension_for_mime_type(mime);

    if (prefix == NULL || extension == NULL)
        return NULL;

    return g_strdup_printf("%s-XXXXXX.%s", prefix, extension);
}

gchar *
clawt_lrg_markdown_escape(const gchar *text)
{
    /*
     * CommonMark's escapable set is every ASCII punctuation character,
     * so escaping all of them is always correct -- a narrower list
     * would be a list somebody has to prove complete.
     */
    static const gchar punctuation[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    GString *out;
    const gchar *p;

    if (text == NULL)
        return g_strdup("");

    out = g_string_sized_new(strlen(text) + 16);

    for (p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            /*
             * Each line its own paragraph, and a run of blank lines one
             * break.  This wrote CommonMark's hard break, a backslash
             * before the newline -- which LrgMarkdownLabel does not
             * have: it drew the backslash and joined the lines, so a
             * decision's question read "configuration.\ \ Its reason".
             */
            if (out->len > 0 && !g_str_has_suffix(out->str, "\n\n"))
                g_string_append(out, "\n\n");
            continue;
        }

        if (*p == '\r')
            continue;

        if (strchr(punctuation, *p) != NULL)
            g_string_append_c(out, '\\');

        g_string_append_c(out, *p);
    }

    return g_string_free(out, FALSE);
}

gboolean
clawt_lrg_reply_is_current(gboolean guarded, guint sent, guint now)
{
    return !guarded || sent == now;
}
