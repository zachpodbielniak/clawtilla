/*
 * clawt-export.c - Taking a conversation somewhere else
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-export.h"

#include <string.h>

const gchar *
clawt_export_format_extension(ClawtExportFormat format)
{
    switch (format) {
    case CLAWT_EXPORT_PLAIN:
        return ".txt";
    case CLAWT_EXPORT_ORG:
        return ".org";
    case CLAWT_EXPORT_MARKDOWN:
    default:
        return ".md";
    }
}

/* What pandoc calls the format. */
static const gchar *
pandoc_name(ClawtExportFormat format)
{
    switch (format) {
    case CLAWT_EXPORT_PLAIN:
        return "plain";
    case CLAWT_EXPORT_ORG:
        return "org";
    default:
        return NULL;
    }
}

gboolean
clawt_export_available(ClawtExportFormat format)
{
    g_autofree gchar *pandoc = NULL;

    if (format == CLAWT_EXPORT_MARKDOWN)
        return TRUE;

    pandoc = g_find_program_in_path("pandoc");

    return pandoc != NULL;
}

gchar *
clawt_export_convert(const gchar *markdown, ClawtExportFormat format,
                     GError **error)
{
    g_autoptr(GSubprocess) pandoc = NULL;
    g_autofree gchar *out = NULL;
    g_autofree gchar *errors = NULL;
    const gchar *target = pandoc_name(format);

    g_return_val_if_fail(markdown != NULL, NULL);

    if (target == NULL)
        return g_strdup(markdown);

    if (!clawt_export_available(format)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "pandoc is not installed, so %s is not available",
                    target);
        return NULL;
    }

    /*
     * --wrap=none is the whole point.
     *
     * pandoc rewraps at 72 columns by default, which turns one
     * paragraph into a dozen hard-wrapped lines: unreadable in an
     * editor that soft wraps, and a nuisance to edit or diff
     * afterwards. One paragraph, one line.
     *
     * --standalone is deliberately *not* passed: this is a fragment
     * somebody is going to paste into their notes, not a document that
     * wants a preamble of its own.
     */
    pandoc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                              G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                              G_SUBPROCESS_FLAGS_STDERR_PIPE,
                              error,
                              "pandoc",
                              "--from", "markdown",
                              "--to", target,
                              "--wrap=none",
                              NULL);

    if (pandoc == NULL)
        return NULL;

    if (!g_subprocess_communicate_utf8(pandoc, markdown, NULL, &out, &errors,
                                       error))
        return NULL;

    if (!g_subprocess_get_successful(pandoc)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "pandoc could not convert this: %s",
                    (errors != NULL && errors[0] != '\0') ? errors
                                                          : "no reason given");
        return NULL;
    }

    return g_steal_pointer(&out);
}

/*
 * A conversation as markdown.
 *
 * Every other format is converted from this one, so the three cannot
 * disagree about what was said.
 */
static gchar *
transcript_markdown(const gchar *room_id, GPtrArray *messages)
{
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    g_string_append_printf(out, "# %s\n\n",
                           room_id != NULL ? room_id : "conversation");

    for (i = 0; messages != NULL && i < messages->len; i++) {
        ClawtMessage *message = g_ptr_array_index(messages, i);
        const gchar *sender = clawt_message_get_sender_id(message);
        const gchar *body = clawt_message_get_body(message);
        g_autoptr(GDateTime) when = NULL;
        g_autofree gchar *stamp = NULL;

        when = g_date_time_new_from_unix_local(
            clawt_message_get_timestamp(message));
        stamp = (when != NULL) ? g_date_time_format(when, "%Y-%m-%d %H:%M")
                               : g_strdup("");

        /*
         * The sender as a heading rather than a "**name:**" prefix, so
         * the result has structure a converter can use -- org gets real
         * outline levels and plain text gets an underline, instead of
         * every turn being one long paragraph.
         */
        g_string_append_printf(out, "## %s \xe2\x80\x94 %s\n\n",
                               sender != NULL ? sender : "?", stamp);
        g_string_append(out, body != NULL ? body : "");
        g_string_append(out, "\n\n");
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

gchar *
clawt_export_transcript(const gchar *room_id, GPtrArray *messages,
                        ClawtExportFormat format, GError **error)
{
    g_autofree gchar *markdown = transcript_markdown(room_id, messages);

    if (format == CLAWT_EXPORT_MARKDOWN)
        return g_steal_pointer(&markdown);

    return clawt_export_convert(markdown, format, error);
}
