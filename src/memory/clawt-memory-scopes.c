/*
 * clawt-memory-scopes.c - The databases behind agent, team and fleet memory
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "memory/clawt-memory-scopes.h"

#include <string.h>

struct _ClawtMemoryScopes {
    GObject parent_instance;

    gchar      *state_dir;
    GHashTable *stores;   /* path -> ClawtMemoryStore*, owned */
};

G_DEFINE_FINAL_TYPE(ClawtMemoryScopes, clawt_memory_scopes, G_TYPE_OBJECT)

#define DEFAULT_LIMIT 20

ClawtMemoryScopes *
clawt_memory_scopes_new(const gchar *state_dir)
{
    ClawtMemoryScopes *self;

    g_return_val_if_fail(state_dir != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_MEMORY_SCOPES, NULL);
    self->state_dir = g_strdup(state_dir);

    return self;
}

gchar *
clawt_memory_scopes_path_for(ClawtMemoryScopes *self, ClawtMemoryScope scope,
                             const gchar *key)
{
    g_return_val_if_fail(CLAWT_IS_MEMORY_SCOPES(self), NULL);

    switch (scope) {
    case CLAWT_MEMORY_SCOPE_AGENT:
        if (key == NULL || *key == '\0')
            return NULL;

        /*
         * Where the agent manager already puts it.  Written here as well
         * so the fan-out and the manager cannot disagree about where an
         * agent's own memories are.
         */
        return g_build_filename(self->state_dir, "agents", key,
                                "memory.db", NULL);

    case CLAWT_MEMORY_SCOPE_TEAM: {
        g_autofree gchar *name = NULL;

        if (key == NULL || *key == '\0')
            return NULL;

        name = g_strdup_printf("team-%s.db", key);
        return g_build_filename(self->state_dir, "memories", name, NULL);
    }

    case CLAWT_MEMORY_SCOPE_FLEET:
        return g_build_filename(self->state_dir, "memories", "fleet.db",
                                NULL);
    }

    /*
     * Every value is named above, so -Wswitch catches a scope added
     * later rather than this line answering for it.
     */
    return NULL;
}

/*
 * Opens a database, or hands back the one already open for that path.
 *
 * Cached by path rather than by scope, because two teams are two files
 * and a fleet with forty of them should not hold forty connections it
 * has never read.
 */
static ClawtMemoryStore *
open_cached(ClawtMemoryScopes *self, const gchar *path, GError **error)
{
    ClawtMemoryStore *store = g_hash_table_lookup(self->stores, path);

    if (store != NULL)
        return store;

    store = clawt_memory_store_new(path, error);

    if (store == NULL)
        return NULL;

    g_hash_table_insert(self->stores, g_strdup(path), store);

    return store;
}

ClawtMemoryStore *
clawt_memory_scopes_open_for_write(ClawtMemoryScopes *self,
                                   ClawtMemoryScope scope, const gchar *key,
                                   GError **error)
{
    g_autofree gchar *path = NULL;

    g_return_val_if_fail(CLAWT_IS_MEMORY_SCOPES(self), NULL);

    path = clawt_memory_scopes_path_for(self, scope, key);

    if (path == NULL) {
        /*
         * Named rather than fallen back on.  Writing a team memory into
         * the agent's own store would leave the agent believing it had
         * shared something nobody else can see -- and a memory whose
         * whole purpose was to reach the team is worse in the wrong
         * place than not written at all.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "there is no %s memory to write to: %s",
                    clawt_enum_to_nick(CLAWT_TYPE_MEMORY_SCOPE, scope),
                    scope == CLAWT_MEMORY_SCOPE_TEAM
                    ? "this agent is not on a team"
                    : "no agent was named");
        return NULL;
    }

    return open_cached(self, path, error);
}

ClawtMemoryStore *
clawt_memory_scopes_open_for_read(ClawtMemoryScopes *self,
                                  ClawtMemoryScope scope, const gchar *key)
{
    g_autofree gchar *path = NULL;
    ClawtMemoryStore *store;

    g_return_val_if_fail(CLAWT_IS_MEMORY_SCOPES(self), NULL);

    path = clawt_memory_scopes_path_for(self, scope, key);

    if (path == NULL)
        return NULL;

    store = g_hash_table_lookup(self->stores, path);

    if (store != NULL)
        return store;

    /*
     * The file is asked about before it is opened, because
     * sqlite3_open() *creates* what it is given.  Without this, every
     * search from an agent on a team with no shared memories would leave
     * a `team-x.db` behind -- and the question "which scopes hold
     * anything" would answer "all of them, all empty".
     */
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return NULL;

    return open_cached(self, path, NULL);
}

GPtrArray *
clawt_memory_scopes_readable(ClawtMemoryScopes *self, ClawtMemoryStore *own,
                             const gchar *team_id)
{
    GPtrArray *out;
    ClawtMemoryStore *store;

    g_return_val_if_fail(CLAWT_IS_MEMORY_SCOPES(self), NULL);

    /*
     * A plain array: the stores are owned by the cache above and by the
     * agent manager, and a free func here would take them from both.
     */
    out = g_ptr_array_new();

    if (own != NULL)
        g_ptr_array_add(out, own);

    if (team_id != NULL && *team_id != '\0') {
        store = clawt_memory_scopes_open_for_read(
            self, CLAWT_MEMORY_SCOPE_TEAM, team_id);

        if (store != NULL)
            g_ptr_array_add(out, store);
    }

    store = clawt_memory_scopes_open_for_read(self, CLAWT_MEMORY_SCOPE_FLEET,
                                              NULL);

    if (store != NULL)
        g_ptr_array_add(out, store);

    return out;
}

/*
 * Pinned first, then newest, then the scope it came from.
 *
 * The scope is the array's own order -- narrowest first -- so a fact the
 * agent established itself comes ahead of an identically stamped one it
 * inherited from the fleet.  Carried in a parallel array rather than on
 * the memory, because #ClawtMemory is what a store round-trips and a
 * field only the merge uses would have to be stored and read back.
 */
typedef struct {
    ClawtMemory *memory;
    guint        scope_rank;
} Ranked;

static gint
compare_ranked(gconstpointer a, gconstpointer b)
{
    const Ranked *left = *(const Ranked * const *)a;
    const Ranked *right = *(const Ranked * const *)b;

    if (left->memory->pinned != right->memory->pinned)
        return left->memory->pinned ? -1 : 1;

    if (left->memory->created_at != right->memory->created_at)
        return (left->memory->created_at > right->memory->created_at) ? -1 : 1;

    if (left->scope_rank != right->scope_rank)
        return (left->scope_rank < right->scope_rank) ? -1 : 1;

    return 0;
}

/*
 * Runs one read across every readable scope and merges the results.
 *
 * One function for search and list, because the merge, the ranking and
 * the limit are the whole of what either does beyond the call itself --
 * and two copies of a merge is two orderings.
 *
 * The three scopes are walked explicitly rather than through
 * clawt_memory_scopes_readable(), because each memory has to be tagged
 * with the scope it came out of and an array of stores has forgotten
 * that by the time it is returned.
 */
static GPtrArray *
fan_out(ClawtMemoryScopes *self, ClawtMemoryStore *own, const gchar *team_id,
        const gchar *query, const gchar *category, gboolean listing,
        gboolean pinned_only, guint limit, GError **error)
{
    struct {
        ClawtMemoryStore *store;
        const gchar      *nick;
    } sources[3];
    gsize n_sources = 0;
    g_autoptr(GPtrArray) ranked = NULL;
    g_autoptr(GPtrArray) out = NULL;
    ClawtMemoryStore *store;
    gsize source_index;
    guint i;

    if (limit == 0)
        limit = DEFAULT_LIMIT;

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_memory_free);
    ranked = g_ptr_array_new_with_free_func(g_free);

    /* Narrowest first, so the array index is also the scope ranking. */
    if (own != NULL) {
        sources[n_sources].store = own;
        sources[n_sources].nick =
            clawt_enum_to_nick(CLAWT_TYPE_MEMORY_SCOPE,
                               CLAWT_MEMORY_SCOPE_AGENT);
        n_sources++;
    }

    if (team_id != NULL && *team_id != '\0') {
        store = clawt_memory_scopes_open_for_read(
            self, CLAWT_MEMORY_SCOPE_TEAM, team_id);

        if (store != NULL) {
            sources[n_sources].store = store;
            sources[n_sources].nick =
                clawt_enum_to_nick(CLAWT_TYPE_MEMORY_SCOPE,
                                   CLAWT_MEMORY_SCOPE_TEAM);
            n_sources++;
        }
    }

    store = clawt_memory_scopes_open_for_read(self, CLAWT_MEMORY_SCOPE_FLEET,
                                              NULL);

    if (store != NULL) {
        sources[n_sources].store = store;
        sources[n_sources].nick =
            clawt_enum_to_nick(CLAWT_TYPE_MEMORY_SCOPE,
                               CLAWT_MEMORY_SCOPE_FLEET);
        n_sources++;
    }

    for (source_index = 0; source_index < n_sources; source_index++) {
        g_autoptr(GPtrArray) found = NULL;

        /*
         * Each scope is asked for the whole limit rather than a share of
         * it.  A fleet store holding nothing relevant should not cost
         * the agent's own store half its results, and the merge below
         * cuts the total back down.
         */
        found = listing
            ? clawt_memory_store_list(sources[source_index].store, category,
                                      pinned_only, limit, error)
            : clawt_memory_store_search(sources[source_index].store, query,
                                        category, limit, error);

        /*
         * The free func goes *before* anything is taken out.
         *
         * g_ptr_array_set_size() to a smaller length frees the removed
         * elements when there is one, so emptying the array afterwards
         * would free the very memories that have just been handed to
         * `ranked` -- and the read would come back holding pointers to
         * memory sqlite had filled and glib had released.  Dropping the
         * func first is what turns the array back into a plain list of
         * borrowed pointers.
         */
        if (found != NULL)
            g_ptr_array_set_free_func(found, NULL);

        for (i = 0; found != NULL && i < found->len; i++) {
            ClawtMemory *memory = g_ptr_array_index(found, i);
            Ranked *entry = g_new0(Ranked, 1);

            /*
             * Tagged here, because this is the last point that knows
             * which file the row came out of.
             */
            g_free(memory->scope);
            memory->scope = g_strdup(sources[source_index].nick);

            entry->memory = memory;
            entry->scope_rank = (guint)source_index;
            g_ptr_array_add(ranked, entry);
        }
    }

    g_ptr_array_sort(ranked, compare_ranked);

    for (i = 0; i < ranked->len; i++) {
        Ranked *entry = g_ptr_array_index(ranked, i);

        if (i < limit)
            g_ptr_array_add(out, entry->memory);
        else
            clawt_memory_free(entry->memory);
    }

    return g_steal_pointer(&out);
}

GPtrArray *
clawt_memory_scopes_search(ClawtMemoryScopes *self, ClawtMemoryStore *own,
                           const gchar *team_id, const gchar *query,
                           const gchar *category, guint limit, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_MEMORY_SCOPES(self), NULL);

    return fan_out(self, own, team_id, query, category, FALSE, FALSE, limit,
                   error);
}

GPtrArray *
clawt_memory_scopes_list(ClawtMemoryScopes *self, ClawtMemoryStore *own,
                         const gchar *team_id, const gchar *category,
                         gboolean pinned_only, guint limit, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_MEMORY_SCOPES(self), NULL);

    return fan_out(self, own, team_id, NULL, category, TRUE, pinned_only,
                   limit, error);
}

static void
clawt_memory_scopes_dispose(GObject *object)
{
    ClawtMemoryScopes *self = CLAWT_MEMORY_SCOPES(object);

    g_clear_pointer(&self->stores, g_hash_table_unref);
    g_clear_pointer(&self->state_dir, g_free);

    G_OBJECT_CLASS(clawt_memory_scopes_parent_class)->dispose(object);
}

static void
clawt_memory_scopes_class_init(ClawtMemoryScopesClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_memory_scopes_dispose;
}

static void
clawt_memory_scopes_init(ClawtMemoryScopes *self)
{
    self->stores = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         g_object_unref);
}
