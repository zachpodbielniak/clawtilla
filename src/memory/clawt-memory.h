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
 * @scope: (nullable): which database it was read from -- "agent", "team"
 *   or "fleet" -- set by the reader, never stored
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
    /*
     * Filled in by whoever read the memory, not by the store.
     *
     * A row cannot know which file it was read out of, and the answer is
     * exactly what a person needs when a listing mixes three scopes:
     * "the fleet believes this" and "I worked this out" are different
     * claims and look identical without it.  NULL from a single-store
     * read, where there is nothing to disambiguate.
     */
    gchar    *scope;
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

/**
 * clawt_memory_importance_from_nick:
 * @nick: (nullable): the level somebody named, or %NULL if they named none
 * @out_importance: (out) (transfer full) (nullable): return location for the
 *   canonical spelling, or %NULL when @nick named none
 * @out_refusal: (out) (optional) (nullable): return location for what to tell
 *   them when @nick is not a level
 *
 * Turns the importance somebody wrote into one of the levels, or refuses.
 *
 * Every surface that lets somebody name a level goes through this -- the
 * `importance` argument of `clawtilla_memory_add` and the `importance`
 * parameter of a pod's `memory_add` -- so an agent and an automation
 * cannot come to mean different things by "critical".
 *
 * Naming none leaves @out_importance %NULL, which means "whatever
 * clawt_memory_new() chose": somebody who did not ask for a level did not
 * ask for anything.
 *
 * An unrecognised level is a **refusal**, never a fallback, and that is
 * the decision here.  The column is plain text and the store binds
 * whatever it is handed, so a mistyped `criticl` is written, sorts as
 * nothing, and is invisible from both ends -- and a pod runs unattended,
 * so it would be wrong on every run of that rule rather than once.  The
 * same answer, for the same reason, as clawt_message_priority_from_nick().
 *
 * Case is not part of it, matching clawt_enum_from_nick(): these are
 * typed by hand into a `.pod` file or written by a model.  What comes
 * back is always the canonical lowercase spelling.
 *
 * The levels come from clawt_memory_importances() rather than from a list
 * written out here, so the refusal cannot name a set that has stopped
 * being true.
 *
 * There is deliberately no twin for the category: clawt_memory_categories()
 * is a shared vocabulary and not a constraint, and refusing an unlisted one
 * would make it one.
 *
 * Returns: %TRUE if @nick names a level, or names none
 */
gboolean clawt_memory_importance_from_nick(const gchar  *nick,
                                           gchar       **out_importance,
                                           gchar       **out_refusal);

/**
 * clawt_memory_provenance_rule:
 *
 * The one sentence every agent is told about what may be remembered.
 *
 * Memory is a prompt-injection *persistence* vector, and it is the worst
 * kind: an instruction an agent copies into its own memory is read back
 * as its own conclusion, in a later session, with nothing left to say
 * where it came from.  A webhook payload and an imported skill both
 * reach an agent as text, so "I read it somewhere" and "I established
 * it" have to be distinguished by the agent at the moment it writes.
 *
 * One spelling, because this appears in the memory tool descriptions,
 * in the summariser's system prompt and in every agent's AGENTS.org --
 * and a rule that is written out three times is a rule with three
 * versions of it in the fleet.
 *
 * Returns: (transfer none): the rule
 */
const gchar *clawt_memory_provenance_rule(void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMemory, clawt_memory_free)

G_END_DECLS
