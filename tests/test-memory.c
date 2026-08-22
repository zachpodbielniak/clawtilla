/*
 * test-memory.c - What an agent remembers, and who can read it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar *dir;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    fixture->dir = g_dir_make_tmp("clawt-mem-XXXXXX", NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

static ClawtMemoryStore *
store_for(Fixture *fixture, const gchar *agent)
{
    g_autofree gchar *path = g_build_filename(fixture->dir, agent,
                                              "memory.db", NULL);
    g_autoptr(GError) error = NULL;
    ClawtMemoryStore *store = clawt_memory_store_new(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(store);

    return store;
}

static gchar *
remember(ClawtMemoryStore *store, const gchar *content, const gchar *category,
         const gchar *tags)
{
    g_autoptr(ClawtMemory) memory = clawt_memory_new(content);
    g_autoptr(GError) error = NULL;
    gchar *id;

    if (category != NULL) {
        g_free(memory->category);
        memory->category = g_strdup(category);
    }

    memory->tags = g_strdup(tags);

    id = clawt_memory_store_add(store, memory, &error);
    g_assert_no_error(error);
    g_assert_nonnull(id);

    return id;
}

static void
test_a_memory_survives_being_written_and_read(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMemoryStore) store = NULL;
    g_autoptr(ClawtMemory) read = NULL;
    g_autofree gchar *id = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    store = store_for(&fixture, "alpha");

    id = remember(store, "the nightly build fails when the disk is slow",
                  "debug", "ci,flaky");

    read = clawt_memory_store_get(store, id, &error);
    g_assert_no_error(error);
    g_assert_nonnull(read);

    g_assert_cmpstr(read->content, ==,
                    "the nightly build fails when the disk is slow");
    g_assert_cmpstr(read->category, ==, "debug");
    g_assert_cmpstr(read->tags, ==, "ci,flaky");
    g_assert_cmpstr(read->importance, ==, "normal");
    g_assert_cmpint(read->created_at, >, 0);

    /* Reading counts, which is what separates a memory that earns its
     * place from one nobody has looked at since it was written. */
    g_assert_cmpint(read->access_count, ==, 0);

    {
        g_autoptr(ClawtMemory) again = clawt_memory_store_get(store, id, NULL);

        g_assert_cmpint(again->access_count, ==, 1);
    }

    fixture_teardown(&fixture);
}

/*
 * The point of the whole design: one agent's memories are in one agent's
 * database, so a missing WHERE clause cannot leak them. There is no
 * query that reaches the other file.
 */
static void
test_one_agent_cannot_read_anothers(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMemoryStore) alpha = NULL;
    g_autoptr(ClawtMemoryStore) beta = NULL;
    g_autoptr(GPtrArray) found = NULL;
    g_autofree gchar *id = NULL;

    fixture_setup(&fixture);
    alpha = store_for(&fixture, "alpha");
    beta = store_for(&fixture, "beta");

    id = remember(alpha, "the deploy key is rotated on Mondays", "fact",
                  NULL);

    g_assert_cmpuint(clawt_memory_store_count(alpha, FALSE), ==, 1);
    g_assert_cmpuint(clawt_memory_store_count(beta, FALSE), ==, 0);

    found = clawt_memory_store_search(beta, "deploy key", NULL, 0, NULL);
    g_assert_cmpuint(found->len, ==, 0);

    /* Not even by id, which is the only handle that crosses. */
    g_assert_null(clawt_memory_store_get(beta, id, NULL));

    fixture_teardown(&fixture);
}

static void
test_search_finds_by_content_summary_and_tags(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMemoryStore) store = NULL;
    g_autoptr(GPtrArray) by_content = NULL;
    g_autoptr(GPtrArray) by_tag = NULL;
    g_autoptr(GPtrArray) narrowed = NULL;
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;

    fixture_setup(&fixture);
    store = store_for(&fixture, "alpha");

    a = remember(store, "podman needs the rootless socket, not the root one",
                 "technical", "podman,containers");
    b = remember(store, "the operator prefers org-mode over markdown",
                 "preference", "notes");

    by_content = clawt_memory_store_search(store, "rootless", NULL, 0, NULL);
    g_assert_cmpuint(by_content->len, ==, 1);
    g_assert_cmpstr(((ClawtMemory *)g_ptr_array_index(by_content, 0))->id,
                    ==, a);

    by_tag = clawt_memory_store_search(store, "podman", NULL, 0, NULL);
    g_assert_cmpuint(by_tag->len, >=, 1);

    /* And a category narrows it rather than being a second search. */
    narrowed = clawt_memory_store_search(store, "the", "preference", 0, NULL);
    g_assert_cmpuint(narrowed->len, ==, 1);
    g_assert_cmpstr(((ClawtMemory *)g_ptr_array_index(narrowed, 0))->id,
                    ==, b);

    fixture_teardown(&fixture);
}

/*
 * A query is whatever a model typed, and unquoted it is FTS5 syntax --
 * a stray quote or a bare NOT is a parse error rather than a search.
 */
static void
test_search_survives_hostile_queries(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMemoryStore) store = NULL;
    g_autofree gchar *id = NULL;
    static const gchar *queries[] = {
        "\"", "NOT", "a AND", "*", "(", "x OR", "\"unclosed", "^", NULL
    };
    gsize i;

    fixture_setup(&fixture);
    store = store_for(&fixture, "alpha");

    id = remember(store, "something to find", NULL, NULL);

    for (i = 0; queries[i] != NULL; i++) {
        g_autoptr(GPtrArray) found = NULL;
        g_autoptr(GError) error = NULL;

        found = clawt_memory_store_search(store, queries[i], NULL, 0, &error);

        /* No crash, no error -- a bad query is nought results. */
        g_assert_no_error(error);
        g_assert_nonnull(found);
    }

    fixture_teardown(&fixture);
}

static void
test_forgetting_hides_but_keeps(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMemoryStore) store = NULL;
    g_autoptr(GPtrArray) listed = NULL;
    g_autofree gchar *id = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    store = store_for(&fixture, "alpha");

    id = remember(store, "a thing that turned out to be wrong", NULL, NULL);

    g_assert_true(clawt_memory_store_forget(store, id, &error));
    g_assert_no_error(error);

    listed = clawt_memory_store_list(store, NULL, FALSE, 0, NULL);
    g_assert_cmpuint(listed->len, ==, 0);

    /*
     * Gone from every listing, still on disk. An agent decides what to
     * forget from inside one conversation and being wrong about that
     * would otherwise be unrecoverable.
     */
    g_assert_cmpuint(clawt_memory_store_count(store, FALSE), ==, 0);
    g_assert_cmpuint(clawt_memory_store_count(store, TRUE), ==, 1);

    fixture_teardown(&fixture);
}

static void
test_pinned_memories_come_first(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMemoryStore) store = NULL;
    g_autoptr(GPtrArray) listed = NULL;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;

    fixture_setup(&fixture);
    store = store_for(&fixture, "alpha");

    first = remember(store, "written first", NULL, NULL);
    second = remember(store, "written second", NULL, NULL);

    /* Newest first, until something is pinned. */
    listed = clawt_memory_store_list(store, NULL, FALSE, 0, NULL);
    g_assert_cmpuint(listed->len, ==, 2);
    g_assert_cmpstr(((ClawtMemory *)g_ptr_array_index(listed, 0))->id,
                    ==, second);

    g_assert_true(clawt_memory_store_pin(store, first, TRUE, NULL));
    g_clear_pointer(&listed, g_ptr_array_unref);

    listed = clawt_memory_store_list(store, NULL, FALSE, 0, NULL);
    g_assert_cmpstr(((ClawtMemory *)g_ptr_array_index(listed, 0))->id,
                    ==, first);

    fixture_teardown(&fixture);
}

/* Memories are the point of a memory store; they must outlive the process. */
static void
test_memories_survive_reopening(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *path = NULL;
    g_autofree gchar *id = NULL;

    fixture_setup(&fixture);
    path = g_build_filename(fixture.dir, "alpha", "memory.db", NULL);

    {
        g_autoptr(ClawtMemoryStore) store = clawt_memory_store_new(path, NULL);

        id = remember(store, "worth keeping", "fact", "durable");
    }

    {
        g_autoptr(ClawtMemoryStore) reopened =
            clawt_memory_store_new(path, NULL);
        g_autoptr(ClawtMemory) read = NULL;
        g_autoptr(GPtrArray) found = NULL;

        read = clawt_memory_store_get(reopened, id, NULL);
        g_assert_nonnull(read);
        g_assert_cmpstr(read->content, ==, "worth keeping");

        /* The full-text index too, not only the table. */
        found = clawt_memory_store_search(reopened, "keeping", NULL, 0, NULL);
        g_assert_cmpuint(found->len, ==, 1);
    }

    fixture_teardown(&fixture);
}

static void
test_empty_content_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMemoryStore) store = NULL;
    g_autoptr(ClawtMemory) empty = clawt_memory_new("");
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    store = store_for(&fixture, "alpha");

    g_assert_null(clawt_memory_store_add(store, empty, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/memory/round-trip",
                    test_a_memory_survives_being_written_and_read);
    g_test_add_func("/memory/isolated-per-agent",
                    test_one_agent_cannot_read_anothers);
    g_test_add_func("/memory/search",
                    test_search_finds_by_content_summary_and_tags);
    g_test_add_func("/memory/search-hostile-queries",
                    test_search_survives_hostile_queries);
    g_test_add_func("/memory/forget-hides-but-keeps",
                    test_forgetting_hides_but_keeps);
    g_test_add_func("/memory/pinned-first", test_pinned_memories_come_first);
    g_test_add_func("/memory/survives-reopening",
                    test_memories_survive_reopening);
    g_test_add_func("/memory/empty-refused", test_empty_content_is_refused);

    return g_test_run();
}
