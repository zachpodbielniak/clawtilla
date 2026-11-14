/*
 * clawt-merge.h - Several results, one turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_MERGE_H
#define CLAWT_MERGE_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_MERGE_RULE:
 *
 * What a section heading of a merged delivery opens and closes with:
 * two U+2500 BOX DRAWINGS LIGHT HORIZONTAL.  A heading is a whole line
 * reading `<rule> 2 of 3 · <label> <rule>`.
 */
#define CLAWT_MERGE_RULE "\xe2\x94\x80\xe2\x94\x80"

/**
 * CLAWT_MERGE_MAX_PARTS:
 *
 * The most results the router folds into one delivery.  A bound rather
 * than "everything waiting", so one turn's input cannot grow with
 * however far behind an agent has fallen; the rest are the next turn.
 */
#define CLAWT_MERGE_MAX_PARTS (16)

/**
 * clawt_merge_escape:
 * @text: (nullable): text going into a section
 *
 * Defuses any line of @text that would read as a section heading, so a
 * peer's reply cannot split itself into two results or pose as a
 * settle notice inside a merged delivery -- the same reason
 * clawt_preamble_escape() exists for the `[clawtilla]` marker.  A
 * heading-like line has its leading rule rewritten to `--`.
 *
 * Returns: (transfer full) (nullable): the escaped text, or %NULL for
 *   %NULL
 */
gchar *clawt_merge_escape(const gchar *text);

/**
 * clawt_merge_append_section:
 * @out: where the merged body is being built
 * @index: this section's position, from 1
 * @total: how many sections there are
 * @label: what the section is, as one line ("'oryx' answering you")
 * @text: the section's text, already escaped for the reader it is going
 *   to (clawt_preamble_escape() for an agent); its heading-like lines
 *   are escaped here
 *
 * Appends one heading and its text.  The one writer of the format
 * clawt_merge_split() reads, used for what the agent is handed and for
 * the note recorded in the room, so the clients and the agent see the
 * same sections.
 */
void clawt_merge_append_section(GString     *out,
                                guint        index,
                                guint        total,
                                const gchar *label,
                                const gchar *text);

/**
 * clawt_merge_split:
 * @body: a body built with clawt_merge_append_section()
 * @out_preface: (out) (optional) (transfer full) (nullable): whatever
 *   came before the first heading, trimmed, or %NULL when there was
 *   nothing
 * @out_labels: (out) (optional) (transfer full) (array zero-terminated=1):
 *   each section's label
 * @out_texts: (out) (optional) (transfer full) (array zero-terminated=1):
 *   each section's text, trimmed
 *
 * Reads a merged body back into its sections, for a client drawing one
 * as sections rather than as one long message.  A body with no headings
 * is zero sections and the whole of it is the preface.
 *
 * Returns: how many sections there are
 */
guint clawt_merge_split(const gchar   *body,
                        gchar        **out_preface,
                        GStrv         *out_labels,
                        GStrv         *out_texts);

G_END_DECLS

#endif /* CLAWT_MERGE_H */
