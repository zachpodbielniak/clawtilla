/*
 * clawt-memory-scopes.h - The databases behind agent, team and fleet memory
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

#include "clawt-enums.h"
#include "memory/clawt-memory-store.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MEMORY_SCOPES (clawt_memory_scopes_get_type())

G_DECLARE_FINAL_TYPE(ClawtMemoryScopes, clawt_memory_scopes, CLAWT,
                     MEMORY_SCOPES, GObject)

/**
 * clawt_memory_scopes_new:
 * @state_dir: the daemon's state directory
 *
 * The shared memory databases: one per team, one for the fleet.
 *
 * Each scope is a separate file -- `<state_dir>/memories/fleet.db` and
 * `<state_dir>/memories/team-<id>.db`, beside the per-agent
 * `<state_dir>/agents/<id>/memory.db` that already existed.  Which
 * database is opened *is* the permission check: an agent on no team
 * cannot read a team's memories because the reader never opens that
 * file, not because a query remembered to say so.  A missing WHERE
 * clause cannot leak what is not in the connection.
 *
 * Nothing is opened here.  Databases are opened on first use and kept,
 * so a fleet with forty teams and one agent running holds one file open.
 *
 * Returns: (transfer full): a new #ClawtMemoryScopes
 */
ClawtMemoryScopes *clawt_memory_scopes_new(const gchar *state_dir);

/**
 * clawt_memory_scopes_path_for:
 * @self: a #ClawtMemoryScopes
 * @scope: which scope
 * @key: (nullable): the agent id for %CLAWT_MEMORY_SCOPE_AGENT, the team
 *   id for %CLAWT_MEMORY_SCOPE_TEAM, ignored for the fleet
 *
 * Where a scope's database lives.
 *
 * One spelling, used by the opener, by the tests and by anything that
 * needs to say on disk where a memory went.  Two spellings would differ
 * exactly once and the second would read an empty store.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL when @scope
 *   needs a @key and has none
 */
gchar *clawt_memory_scopes_path_for(ClawtMemoryScopes *self,
                                    ClawtMemoryScope   scope,
                                    const gchar       *key);

/**
 * clawt_memory_scopes_open_for_write:
 * @self: a #ClawtMemoryScopes
 * @scope: which scope
 * @key: (nullable): the agent id, or the team id, or %NULL for the fleet
 * @error: (out) (optional): return location for a #GError
 *
 * The store a new memory in @scope lands in, creating it.
 *
 * Refuses, saying which, when @scope is `team` and @key is %NULL: an
 * agent on no team asking to write a team memory has named a place that
 * does not exist, and the alternative -- quietly writing it to its own
 * store -- is a memory the agent believes it shared and nobody else can
 * see.
 *
 * Returns: (transfer none) (nullable): the store, or %NULL
 */
ClawtMemoryStore *clawt_memory_scopes_open_for_write(ClawtMemoryScopes  *self,
                                                     ClawtMemoryScope    scope,
                                                     const gchar        *key,
                                                     GError            **error);

/**
 * clawt_memory_scopes_open_for_read:
 * @self: a #ClawtMemoryScopes
 * @scope: which scope
 * @key: (nullable): the agent id, or the team id, or %NULL for the fleet
 *
 * The store for @scope if it is already on disk, and %NULL if it is not.
 *
 * A read never brings a scope into being.  sqlite3_open() creates the
 * file it is given, so a plain open here would leave a `team-x.db`
 * behind for every team anybody ever searched from -- and a listing of
 * "which scopes hold anything" would then answer "all of them".
 *
 * Returns: (transfer none) (nullable): the store, or %NULL when there is
 *   no such database
 */
ClawtMemoryStore *clawt_memory_scopes_open_for_read(ClawtMemoryScopes *self,
                                                    ClawtMemoryScope   scope,
                                                    const gchar       *key);

/**
 * clawt_memory_scopes_readable:
 * @self: a #ClawtMemoryScopes
 * @own: (nullable) (transfer none): the agent's own store, already open
 * @team_id: (nullable): the team the agent is on
 *
 * Every store this agent may read, narrowest first.
 *
 * The fan-out: its own, then its team's, then the fleet's, skipping any
 * whose database does not exist.  @own is passed in rather than opened
 * here because the agent manager already holds it -- two connections to
 * one file would be two page caches disagreeing about what was just
 * written.
 *
 * Returns: (transfer container) (element-type ClawtMemoryStore): the
 *   stores, possibly empty
 */
GPtrArray *clawt_memory_scopes_readable(ClawtMemoryScopes *self,
                                        ClawtMemoryStore  *own,
                                        const gchar       *team_id);

/**
 * clawt_memory_scopes_search:
 * @self: a #ClawtMemoryScopes
 * @own: (nullable) (transfer none): the agent's own store
 * @team_id: (nullable): the team the agent is on
 * @query: what to look for, or %NULL to list
 * @category: (nullable): narrow to one category
 * @limit: how many at most across every scope, 0 for a sensible default
 * @error: (out) (optional): return location for a #GError
 *
 * Searches every scope the agent may read and merges the results.
 *
 * Pinned memories first, then newest first, then narrowest scope first:
 * a fact an agent established itself outranks one it inherited from the
 * fleet, which is the order somebody reading the list would want.
 *
 * Returns: (transfer full) (element-type ClawtMemory): the matches
 */
GPtrArray *clawt_memory_scopes_search(ClawtMemoryScopes  *self,
                                      ClawtMemoryStore   *own,
                                      const gchar        *team_id,
                                      const gchar        *query,
                                      const gchar        *category,
                                      guint               limit,
                                      GError            **error);

/**
 * clawt_memory_scopes_list:
 * @self: a #ClawtMemoryScopes
 * @own: (nullable) (transfer none): the agent's own store
 * @team_id: (nullable): the team the agent is on
 * @category: (nullable): narrow to one category
 * @pinned_only: only the pinned ones
 * @limit: how many at most across every scope, 0 for a sensible default
 * @error: (out) (optional): return location for a #GError
 *
 * The recent memories from every scope the agent may read.
 *
 * Returns: (transfer full) (element-type ClawtMemory): the memories
 */
GPtrArray *clawt_memory_scopes_list(ClawtMemoryScopes  *self,
                                    ClawtMemoryStore   *own,
                                    const gchar        *team_id,
                                    const gchar        *category,
                                    gboolean            pinned_only,
                                    guint               limit,
                                    GError            **error);

G_END_DECLS
