/*
 * clawt-memory-journal.c - Every change to a memory, and its undo
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "memory/clawt-memory-journal.h"

G_DEFINE_BOXED_TYPE(ClawtMemoryJournalEntry, clawt_memory_journal_entry,
                    clawt_memory_journal_entry_copy,
                    clawt_memory_journal_entry_free)

ClawtMemoryJournalEntry *
clawt_memory_journal_entry_copy(
	const ClawtMemoryJournalEntry *self
){
	ClawtMemoryJournalEntry *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = g_new0(ClawtMemoryJournalEntry, 1);
	copy->id = self->id;
	copy->at = self->at;
	copy->action = g_strdup(self->action);
	copy->memory_id = g_strdup(self->memory_id);
	copy->actor = g_strdup(self->actor);
	copy->excerpt = g_strdup(self->excerpt);
	copy->undoes = self->undoes;
	copy->undone_at = self->undone_at;
	copy->undone_by = g_strdup(self->undone_by);
	copy->undoable = self->undoable;
	copy->why_not = g_strdup(self->why_not);
	copy->scope = g_strdup(self->scope);

	return copy;
}

void
clawt_memory_journal_entry_free(
	ClawtMemoryJournalEntry *self
){
	if (self == NULL)
		return;

	g_free(self->action);
	g_free(self->memory_id);
	g_free(self->actor);
	g_free(self->excerpt);
	g_free(self->undone_by);
	g_free(self->why_not);
	g_free(self->scope);
	g_free(self);
}

const gchar *
clawt_memory_journal_action_label(
	const gchar *action
){
	if (g_strcmp0(action, "add") == 0)
		return "Remembered";
	if (g_strcmp0(action, "forget") == 0)
		return "Forgot";
	if (g_strcmp0(action, "pin") == 0)
		return "Pinned";
	if (g_strcmp0(action, "unpin") == 0)
		return "Unpinned";
	if (g_strcmp0(action, "undo") == 0)
		return "Undid";

	return (action != NULL) ? action : "Changed";
}

const gchar *
clawt_memory_journal_undo_label(
	const gchar *action
){
	if (g_strcmp0(action, "add") == 0)
		return "Archive it";
	if (g_strcmp0(action, "forget") == 0)
		return "Bring it back";
	if (g_strcmp0(action, "pin") == 0)
		return "Unpin it";
	if (g_strcmp0(action, "unpin") == 0)
		return "Pin it again";

	return "Undo";
}

gchar *
clawt_memory_journal_describe(
	const ClawtMemoryJournalEntry *entry
){
	g_autoptr(GString) line = NULL;
	g_autofree gchar *verb = NULL;
	const gchar *who;

	g_return_val_if_fail(entry != NULL, NULL);

	who = (entry->actor != NULL && *entry->actor != '\0')
		? entry->actor : "somebody";
	verb = g_ascii_strdown(clawt_memory_journal_action_label(entry->action),
		-1);
	line = g_string_new(NULL);

	if (g_strcmp0(entry->action, "undo") == 0)
		g_string_append_printf(line, "%s undid change #%" G_GINT64_FORMAT
			" to %s", who, entry->undoes,
			entry->memory_id != NULL ? entry->memory_id : "a memory");
	else if (entry->excerpt != NULL && *entry->excerpt != '\0')
		g_string_append_printf(line, "%s %s “%s” (%s)", who, verb,
			entry->excerpt,
			entry->memory_id != NULL ? entry->memory_id : "?");
	else
		g_string_append_printf(line, "%s %s %s", who, verb,
			entry->memory_id != NULL ? entry->memory_id : "a memory");

	if (entry->undone_at > 0)
		g_string_append_printf(line, "; undone by %s",
			(entry->undone_by != NULL && *entry->undone_by != '\0')
				? entry->undone_by : "somebody");

	return g_string_free(g_steal_pointer(&line), FALSE);
}
