/*
 * clawt-merge.c - Several results, one turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * libreclaw runs one turn per message, so a chief whose five workers
 * report while it is busy takes five turns, each rereading everything.
 * The router folds results waiting in one room into one delivery; this
 * file is the shape of that delivery's body, written in one place and
 * read back in one place, so the agent and the three clients see the
 * same sections.
 */

#include "chat/clawt-merge.h"

#include <string.h>

/* U+00B7 MIDDLE DOT, between the count and the label. */
#define MERGE_DOT "\xc2\xb7"

/*
 * Whether @line, with leading blanks skipped, opens with the rule.
 * Leading blanks count because a reader skims past indentation, and a
 * heading that is one space in still reads as a heading.
 */
static gboolean
line_opens_with_rule(
	const gchar *line
){
	while (*line == ' ' || *line == '\t')
		line++;

	return g_str_has_prefix(line, CLAWT_MERGE_RULE);
}

gchar *
clawt_merge_escape(
	const gchar *text
){
	g_auto(GStrv) lines = NULL;
	g_autoptr(GString) out = NULL;
	guint i;

	if (text == NULL)
		return NULL;

	lines = g_strsplit(text, "\n", -1);
	out = g_string_sized_new(strlen(text) + 8);

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];

		if (i > 0)
			g_string_append_c(out, '\n');

		if (line_opens_with_rule(line)) {
			const gchar *rule = strstr(line, CLAWT_MERGE_RULE);

			/* Everything before the rule, then "--" in its place. */
			g_string_append_len(out, line, (gssize)(rule - line));
			g_string_append(out, "--");
			g_string_append(out, rule + strlen(CLAWT_MERGE_RULE));
			continue;
		}

		g_string_append(out, line);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

void
clawt_merge_append_section(
	GString     *out,
	guint        index,
	guint        total,
	const gchar *label,
	const gchar *text
){
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *clean_label = NULL;

	g_return_if_fail(out != NULL);

	/*
	 * The label is one line by construction: a newline in it would end
	 * the heading early and start the section text with the rest.
	 */
	clean_label = g_strdup(label != NULL ? label : "");
	g_strdelimit(clean_label, "\r\n", ' ');

	escaped = clawt_merge_escape(text != NULL ? text : "");

	if (out->len > 0 && out->str[out->len - 1] != '\n')
		g_string_append_c(out, '\n');
	if (out->len > 0)
		g_string_append_c(out, '\n');

	g_string_append_printf(out, "%s %u of %u " MERGE_DOT " %s %s\n%s\n",
		CLAWT_MERGE_RULE, index, total, clean_label, CLAWT_MERGE_RULE,
		escaped);
}

/*
 * Reads a heading line, or says it is not one.  Strict on purpose: the
 * writer is above, and anything it did not write is section text.
 */
static gboolean
parse_heading(
	const gchar  *line,
	gchar       **out_label
){
	const gchar *cursor;
	const gchar *dot;
	gsize len = strlen(line);
	gsize rule = strlen(CLAWT_MERGE_RULE);
	guint64 index;
	gchar *end = NULL;

	if (!g_str_has_prefix(line, CLAWT_MERGE_RULE " "))
		return FALSE;
	if (len < 2 * rule + 2 || !g_str_has_suffix(line, " " CLAWT_MERGE_RULE))
		return FALSE;

	cursor = line + rule + 1;
	index = g_ascii_strtoull(cursor, &end, 10);
	if (end == cursor || index == 0 || !g_str_has_prefix(end, " of "))
		return FALSE;

	dot = strstr(end, " " MERGE_DOT " ");
	if (dot == NULL)
		return FALSE;

	cursor = dot + strlen(" " MERGE_DOT " ");

	if (cursor > line + len - rule - 1)
		return FALSE;

	*out_label = g_strndup(cursor, (gsize)((line + len - rule - 1) - cursor));
	return TRUE;
}

guint
clawt_merge_split(
	const gchar   *body,
	gchar        **out_preface,
	GStrv         *out_labels,
	GStrv         *out_texts
){
	g_auto(GStrv) lines = NULL;
	g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) texts = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) preface = g_string_new(NULL);
	GString *current = NULL;
	guint count;
	guint i;

	lines = g_strsplit(body != NULL ? body : "", "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		gchar *label = NULL;

		if (parse_heading(lines[i], &label)) {
			if (current != NULL)
				g_ptr_array_add(texts, g_strstrip(g_string_free(current, FALSE)));
			g_ptr_array_add(labels, label);
			current = g_string_new(NULL);
			continue;
		}

		if (current == NULL) {
			g_string_append(preface, lines[i]);
			g_string_append_c(preface, '\n');
		} else {
			g_string_append(current, lines[i]);
			g_string_append_c(current, '\n');
		}
	}

	if (current != NULL)
		g_ptr_array_add(texts, g_strstrip(g_string_free(current, FALSE)));

	if (out_preface != NULL) {
		g_strstrip(preface->str);
		*out_preface = (preface->str[0] != '\0')
			? g_strdup(preface->str) : NULL;
	}

	count = labels->len;

	g_ptr_array_add(labels, NULL);
	g_ptr_array_add(texts, NULL);

	if (out_labels != NULL)
		*out_labels = (GStrv)g_ptr_array_steal(labels, NULL);
	if (out_texts != NULL)
		*out_texts = (GStrv)g_ptr_array_steal(texts, NULL);

	return count;
}
