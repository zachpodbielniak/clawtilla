/*
 * clawt-memory-journal.h - Every change to a memory, and its undo
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla_memory_add, _forget and _pin act at once, from inside one
 * conversation, and a memory is read back later as the agent's own
 * conclusion with nothing to say where it came from.  So an agent that
 * wrote down something false, forgot something true or pinned the wrong
 * thing changed every later turn, and a person had no way to see that it
 * had happened, let alone take it back.
 *
 * Each memory database therefore keeps a journal beside its memories:
 * one row per add, forget, pin and unpin, written by the store in the
 * same function that makes the change -- never by a caller remembering
 * to -- and an undo per row.  An undo is itself a row, so the journal
 * says who took what back and when.
 *
 * Undo is conservative, and refuses rather than guessing:
 *
 *   - an entry is undone once;
 *   - an entry with a *later* change to the same memory still standing
 *     is refused until that one is undone first, because undoing the
 *     older one would overwrite the newer;
 *   - an add that replaced an earlier version of the same id is refused,
 *     because the earlier text is not kept and undoing would not bring
 *     it back -- it would only hide the new one;
 *   - undoing an add *archives* the memory, the same as forgetting it:
 *     nothing here deletes a row.
 *
 * This header holds the record and the words both clients and the CLI
 * use for it, so the three say the same thing about one row.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MEMORY_JOURNAL_ENTRY (clawt_memory_journal_entry_get_type())

/**
 * ClawtMemoryJournalEntry:
 * @id: the journal row's id, unique within its database
 * @at: unix seconds
 * @action: `add`, `forget`, `pin`, `unpin` or `undo`
 * @memory_id: the memory it changed
 * @actor: (nullable): who changed it -- an agent id, `operator`, or
 *   %NULL when the caller did not say
 * @excerpt: (nullable): the start of the memory's text, for a listing
 * @undoes: for an `undo` row, the row it took back; 0 otherwise
 * @undone_at: unix seconds when this row was undone, 0 if it was not
 * @undone_by: (nullable): who undid it
 * @undoable: whether clawt_memory_store_undo() would accept it now
 * @why_not: (nullable): when it would not, the sentence saying why
 * @scope: (nullable): which database it was read from -- `agent`, `team`
 *   or `fleet` -- set by the reader, never stored
 *
 * One change to one memory.
 */
typedef struct {
	gint64    id;
	gint64    at;
	gchar    *action;
	gchar    *memory_id;
	gchar    *actor;
	gchar    *excerpt;
	gint64    undoes;
	gint64    undone_at;
	gchar    *undone_by;
	gboolean  undoable;
	gchar    *why_not;
	gchar    *scope;
} ClawtMemoryJournalEntry;

GType
clawt_memory_journal_entry_get_type(
	void
) G_GNUC_CONST;

/**
 * clawt_memory_journal_entry_copy:
 * @self: an entry
 *
 * Returns: (transfer full): a deep copy
 */
ClawtMemoryJournalEntry *
clawt_memory_journal_entry_copy(
	const ClawtMemoryJournalEntry *self
);

/**
 * clawt_memory_journal_entry_free:
 * @self: (nullable): an entry
 *
 * Frees @self.
 */
void
clawt_memory_journal_entry_free(
	ClawtMemoryJournalEntry *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMemoryJournalEntry,
                              clawt_memory_journal_entry_free)

/**
 * clawt_memory_journal_action_label:
 * @action: (nullable): an entry's action
 *
 * What a person reads for an action: "Remembered", "Forgot", "Pinned",
 * "Unpinned" or "Undid", and the action itself for anything else so a
 * newer daemon's row still says something.
 *
 * Returns: (transfer none): the label
 */
const gchar *
clawt_memory_journal_action_label(
	const gchar *action
);

/**
 * clawt_memory_journal_undo_label:
 * @action: (nullable): an entry's action
 *
 * What the Undo button on that row does, in words: "Forget it again"
 * would be wrong for an undo of a forget, so each action says its own
 * reversal -- "Archive it", "Bring it back", "Unpin it", "Pin it again".
 *
 * Returns: (transfer none): the label
 */
const gchar *
clawt_memory_journal_undo_label(
	const gchar *action
);

/**
 * clawt_memory_journal_describe:
 * @entry: an entry
 *
 * One line for the row: who did what to which memory, and whether it has
 * been undone -- "chief remembered "The deploy key…" (mem-1); undone by
 * operator".
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_memory_journal_describe(
	const ClawtMemoryJournalEntry *entry
);

G_END_DECLS
