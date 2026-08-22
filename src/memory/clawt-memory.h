/*
 * clawt-memory.h - One thing an agent remembers
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

G_BEGIN_DECLS

#define CLAWT_TYPE_MEMORY (clawt_memory_get_type())

/**
 * ClawtMemory:
 * @id: sortable unique identifier
 * @content: what is being remembered, in full
 * @summary: (nullable): one line, for a listing
 * @category: what kind of thing this is
 * @importance: low, normal, high or critical
 * @tags: (nullable): comma-separated, for narrowing a search
 * @source: (nullable): where it came from -- a peer's id, a task, a person
 * @pinned: %TRUE to keep it at the top of every listing
 * @archived: %TRUE when forgotten; kept, but out of the way
 * @created_at: unix seconds
 * @updated_at: unix seconds
 * @accessed_at: unix seconds, 0 if never read back
 * @access_count: how many times it has been read back
 *
 * One memory.
 *
 * A plain record with public fields rather than an opaque type: it is
 * data, every field is read by whoever displays it, and accessors for
 * thirteen scalars would be noise.
 */
typedef struct {
    gchar    *id;
    gchar    *content;
    gchar    *summary;
    gchar    *category;
    gchar    *importance;
    gchar    *tags;
    gchar    *source;
    gboolean  pinned;
    gboolean  archived;
    gint64    created_at;
    gint64    updated_at;
    gint64    accessed_at;
    gint      access_count;
} ClawtMemory;

GType clawt_memory_get_type(void) G_GNUC_CONST;

/**
 * clawt_memory_new:
 * @content: what to remember
 *
 * A memory with sensible defaults: category "general", importance
 * "normal", created now.
 *
 * Returns: (transfer full): a new #ClawtMemory
 */
ClawtMemory *clawt_memory_new(const gchar *content);

/**
 * clawt_memory_copy:
 * @self: a #ClawtMemory
 *
 * Returns: (transfer full): a deep copy
 */
ClawtMemory *clawt_memory_copy(ClawtMemory *self);

/**
 * clawt_memory_free:
 * @self: (transfer full): a #ClawtMemory
 *
 * Frees it.
 */
void clawt_memory_free(ClawtMemory *self);

/**
 * clawt_memory_categories:
 * @n_categories: (out) (optional): how many
 *
 * The categories a client can offer.
 *
 * Not a constraint -- the column is text and anything is accepted --
 * but a shared vocabulary is what makes a category worth filtering on.
 * An agent inventing a new one per memory has a tag, not a category.
 *
 * Returns: (transfer none) (array length=n_categories zero-terminated=1):
 *   the names
 */
const gchar * const *clawt_memory_categories(gsize *n_categories);

/**
 * clawt_memory_importances:
 * @n_levels: (out) (optional): how many
 *
 * The importance levels, least first.
 *
 * Returns: (transfer none) (array length=n_levels zero-terminated=1):
 *   the names
 */
const gchar * const *clawt_memory_importances(gsize *n_levels);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMemory, clawt_memory_free)

G_END_DECLS
