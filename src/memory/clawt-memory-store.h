/*
 * clawt-memory-store.h - Where an agent's memories live
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

#include <glib-object.h>

#include "memory/clawt-memory.h"

G_BEGIN_DECLS

/**
 * CLAWT_MEMORY_JOURNAL_KEEP_DEFAULT:
 *
 * How many journal rows a memory database keeps when nobody said, which
 * is the schema default of `memories.journal_keep`.
 */
#define CLAWT_MEMORY_JOURNAL_KEEP_DEFAULT (1000)

#define CLAWT_TYPE_MEMORY_STORE (clawt_memory_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtMemoryStore, clawt_memory_store, CLAWT,
                     MEMORY_STORE, GObject)

/**
 * clawt_memory_store_new:
 * @path: the database file
 * @error: (out) (optional): return location for a #GError
 *
 * Opens, and creates, one agent's memory database.
 *
 * One file per agent, not one table with an agent column. Isolation is
 * then a property of the filesystem rather than of every query being
 * written correctly: a missing WHERE clause cannot leak another agent's
 * memories, because they are not in the database being read.
 *
 * Returns: (transfer full) (nullable): the store, or %NULL on failure
 */
ClawtMemoryStore *clawt_memory_store_new(const gchar  *path,
                                         GError      **error);

/**
 * clawt_memory_store_add:
 * @self: a #ClawtMemoryStore
 * @memory: what to remember
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a memory, giving it an id if it has none.
 *
 * Returns: (transfer full) (nullable): the id, or %NULL on failure
 */
gchar *clawt_memory_store_add(ClawtMemoryStore  *self,
                              ClawtMemory       *memory,
                              GError           **error);

/**
 * clawt_memory_store_search:
 * @self: a #ClawtMemoryStore
 * @query: what to look for
 * @category: (nullable): narrow to one category
 * @limit: how many at most, 0 for a sensible default
 * @error: (out) (optional): return location for a #GError
 *
 * Full-text search, best match first, pinned memories ahead of the rest.
 *
 * Uses FTS5 where the sqlite build has it and falls back to a substring
 * match where it does not -- a memory store that refuses to open on an
 * older sqlite is worse than one that ranks less well.
 *
 * Reading a memory back counts as an access, which is what makes it
 * possible to tell a memory that earns its place from one that has never
 * been looked at since it was written.
 *
 * Returns: (transfer full) (element-type ClawtMemory): the matches
 */
GPtrArray *clawt_memory_store_search(ClawtMemoryStore  *self,
                                     const gchar       *query,
                                     const gchar       *category,
                                     guint              limit,
                                     GError           **error);

/**
 * clawt_memory_store_list:
 * @self: a #ClawtMemoryStore
 * @category: (nullable): narrow to one category
 * @pinned_only: only the pinned ones
 * @limit: how many at most, 0 for a sensible default
 * @error: (out) (optional): return location for a #GError
 *
 * Recent memories, newest first, pinned ones ahead of the rest.
 *
 * Returns: (transfer full) (element-type ClawtMemory): the memories
 */
GPtrArray *clawt_memory_store_list(ClawtMemoryStore  *self,
                                   const gchar       *category,
                                   gboolean           pinned_only,
                                   guint              limit,
                                   GError           **error);

/**
 * clawt_memory_store_get:
 * @self: a #ClawtMemoryStore
 * @id: which one
 * @error: (out) (optional): return location for a #GError
 *
 * One memory in full, counting as an access.
 *
 * Returns: (transfer full) (nullable): the memory, or %NULL
 */
ClawtMemory *clawt_memory_store_get(ClawtMemoryStore  *self,
                                    const gchar       *id,
                                    GError           **error);

/**
 * clawt_memory_store_forget:
 * @self: a #ClawtMemoryStore
 * @id: which one
 * @error: (out) (optional): return location for a #GError
 *
 * Archives a memory: out of every listing and every search, still on
 * disk.
 *
 * Deleting is not offered. An agent decides what to forget from inside
 * one conversation, and being wrong about that is unrecoverable; a
 * person can still go and look.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_memory_store_forget(ClawtMemoryStore  *self,
                                   const gchar       *id,
                                   GError           **error);

/**
 * clawt_memory_store_pin:
 * @self: a #ClawtMemoryStore
 * @id: which one
 * @pinned: whether to pin it
 * @error: (out) (optional): return location for a #GError
 *
 * Keeps a memory at the top of every listing.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_memory_store_pin(ClawtMemoryStore  *self,
                                const gchar       *id,
                                gboolean           pinned,
                                GError           **error);

/**
 * clawt_memory_store_forget_as:
 * @self: a #ClawtMemoryStore
 * @id: which one
 * @actor: (nullable): who is forgetting it, for the journal
 * @error: (out) (optional): return location for a #GError
 *
 * clawt_memory_store_forget(), saying who asked.  Journaled either way;
 * this only fills in the name.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_memory_store_forget_as(ClawtMemoryStore  *self,
                                      const gchar       *id,
                                      const gchar       *actor,
                                      GError           **error);

/**
 * clawt_memory_store_pin_as:
 * @self: a #ClawtMemoryStore
 * @id: which one
 * @pinned: whether to pin it
 * @actor: (nullable): who is pinning it, for the journal
 * @error: (out) (optional): return location for a #GError
 *
 * clawt_memory_store_pin(), saying who asked.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_memory_store_pin_as(ClawtMemoryStore  *self,
                                   const gchar       *id,
                                   gboolean           pinned,
                                   const gchar       *actor,
                                   GError           **error);

/**
 * clawt_memory_store_set_journal_keep:
 * @self: a #ClawtMemoryStore
 * @keep: how many journal rows to keep, 0 for every one
 *
 * The bound on the journal (`memories.journal_keep`).  The oldest rows go
 * first, on the next write.  An undo needs its row, so a change older
 * than this can no longer be taken back -- which the review page says.
 */
void clawt_memory_store_set_journal_keep(ClawtMemoryStore *self,
                                         guint             keep);

/**
 * clawt_memory_store_journal:
 * @self: a #ClawtMemoryStore
 * @actor: (nullable): only the changes this actor made
 * @limit: how many at most, newest first; 0 for a sensible default
 * @error: (out) (optional): return location for a #GError
 *
 * The journal: every add, forget, pin and unpin, and every undo, each
 * row saying whether it can be undone now and, if not, why.
 *
 * Returns: (transfer full) (element-type ClawtMemoryJournalEntry): the
 *   rows, newest first
 */
GPtrArray *clawt_memory_store_journal(ClawtMemoryStore  *self,
                                      const gchar       *actor,
                                      guint              limit,
                                      GError           **error);

/**
 * clawt_memory_store_undo:
 * @self: a #ClawtMemoryStore
 * @entry_id: the journal row to take back
 * @actor: (nullable): who is taking it back
 * @error: (out) (optional): return location for a #GError
 *
 * Reverses one change: an add is archived, a forget is brought back, a
 * pin or an unpin is flipped back.  Refused, with the reason, for a row
 * already undone, an undo row, an add that replaced an earlier version,
 * a memory no longer on disk, and a row with a later change to the same
 * memory still standing.  The change and its journal row are one
 * transaction.
 *
 * Returns: %TRUE when it was undone
 */
gboolean clawt_memory_store_undo(ClawtMemoryStore  *self,
                                 gint64             entry_id,
                                 const gchar       *actor,
                                 GError           **error);

/**
 * clawt_memory_store_count:
 * @self: a #ClawtMemoryStore
 * @include_archived: count the forgotten ones too
 *
 * Returns: how many memories there are
 */
guint clawt_memory_store_count(ClawtMemoryStore *self,
                               gboolean          include_archived);

/**
 * clawt_memory_store_has_full_text:
 * @self: a #ClawtMemoryStore
 *
 * Whether search is FTS5-ranked or a substring fallback.
 *
 * Returns: %TRUE when FTS5 is in use
 */
gboolean clawt_memory_store_has_full_text(ClawtMemoryStore *self);

G_END_DECLS
