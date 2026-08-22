/*
 * clawt-export.h - Taking a conversation somewhere else
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

#include <glib.h>

G_BEGIN_DECLS

/**
 * ClawtExportFormat:
 * @CLAWT_EXPORT_MARKDOWN: as written; no conversion
 * @CLAWT_EXPORT_PLAIN: markup removed
 * @CLAWT_EXPORT_ORG: org-mode
 *
 * What to turn a conversation into on the way out.
 */
typedef enum {
    CLAWT_EXPORT_MARKDOWN = 0,
    CLAWT_EXPORT_PLAIN,
    CLAWT_EXPORT_ORG
} ClawtExportFormat;

/**
 * clawt_export_format_extension:
 * @format: which format
 *
 * The file extension, with its dot.
 *
 * Returns: (transfer none): ".md", ".txt" or ".org"
 */
const gchar *clawt_export_format_extension(ClawtExportFormat format);

/**
 * clawt_export_available:
 * @format: which format
 *
 * Whether this format can actually be produced here.
 *
 * Markdown always can. The other two are pandoc's work, so they depend
 * on pandoc being installed -- and a menu entry that silently produces
 * markdown under an org label is worse than one that is not offered.
 *
 * Returns: %TRUE when the format is available
 */
gboolean clawt_export_available(ClawtExportFormat format);

/**
 * clawt_export_convert:
 * @markdown: the document to convert
 * @format: what to turn it into
 * @error: (out) (optional): return location for a #GError
 *
 * Converts markdown to @format with pandoc.
 *
 * Run with `--wrap=none`, so a paragraph stays one line. pandoc's
 * default rewraps at 72 columns, which turns every paragraph into a
 * block of hard-wrapped lines -- unreadable in an editor that soft
 * wraps, and a nuisance to diff or edit afterwards.
 *
 * Returns: (transfer full) (nullable): the converted document, or %NULL
 */
gchar *clawt_export_convert(const gchar        *markdown,
                            ClawtExportFormat   format,
                            GError            **error);

/**
 * clawt_export_transcript:
 * @room_id: which conversation, for the title
 * @messages: (element-type ClawtMessage): the messages, oldest first
 * @format: what to produce
 * @error: (out) (optional): return location for a #GError
 *
 * Renders a conversation as a document.
 *
 * Built as markdown first and converted from there, so the three
 * formats cannot disagree about what the conversation contained.
 *
 * Returns: (transfer full) (nullable): the document, or %NULL
 */
gchar *clawt_export_transcript(const gchar        *room_id,
                               GPtrArray          *messages,
                               ClawtExportFormat   format,
                               GError            **error);

G_END_DECLS
