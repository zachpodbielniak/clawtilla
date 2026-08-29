/*
 * test-memory-scope.c - Which database an agent's memories land in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The permission here is which *file* is opened, which is what makes it
 * survivable: a missing condition in a query cannot leak memories that
 * are not in the connection being read.  So these tests are mostly about
 * files -- that a read never creates one, that a write to a scope the
 * agent has no key for is refused rather than redirected, and that the
 * fan-out reads exactly the ones it should and no more.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar             *dir;
    ClawtMemoryScopes *scopes;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    fixture->dir = g_dir_make_tmp("clawt-scope-XXXXXX", NULL);
    fixture->scopes = clawt_memory_scopes_new(fixture->dir);
    g_assert_nonnull(fixture->scopes);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->scopes);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

static void
remember(ClawtMemoryStore *store, const gchar *content, gint64 at)
{
    g_autoptr(ClawtMemory) memory = clawt_memory_new(content);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    memory->created_at = at;
    id = clawt_memory_store_add(store, memory, &error);

    g_assert_no_error(error);
    g_assert_nonnull(id);
}

/*
 * Each scope is its own file, and the paths are the ones the rest of the
 * tree already uses.
 *
 * Written down here because two spellings of where a fleet's memories
 * live would differ exactly once, and the half that differed would read
 * an empty store and report it as an empty store.
 */
static void
test_each_scope_is_its_own_file(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *agent = NULL;
    g_autofree gchar *team = NULL;
    g_autofree gchar *fleet = NULL;
    g_autofree gchar *expected_agent = NULL;
    g_autofree gchar *expected_team = NULL;
    g_autofree gchar *expected_fleet = NULL;

    fixture_setup(&fixture);

    agent = clawt_memory_scopes_path_for(fixture.scopes,
                                         CLAWT_MEMORY_SCOPE_AGENT, "alpha");
    team = clawt_memory_scopes_path_for(fixture.scopes,
                                        CLAWT_MEMORY_SCOPE_TEAM, "build");
    fleet = clawt_memory_scopes_path_for(fixture.scopes,
                                         CLAWT_MEMORY_SCOPE_FLEET, NULL);

    expected_agent = g_build_filename(fixture.dir, "agents", "alpha",
                                      "memory.db", NULL);
    expected_team = g_build_filename(fixture.dir, "memories", "team-build.db",
                                     NULL);
    expected_fleet = g_build_filename(fixture.dir, "memories", "fleet.db",
                                      NULL);

    g_assert_cmpstr(agent, ==, expected_agent);
    g_assert_cmpstr(team, ==, expected_team);
    g_assert_cmpstr(fleet, ==, expected_fleet);

    /* And a scope with no key has no path rather than a wrong one. */
    {
        g_autofree gchar *nowhere = clawt_memory_scopes_path_for(
            fixture.scopes, CLAWT_MEMORY_SCOPE_TEAM, NULL);

        g_assert_null(nowhere);
    }

    fixture_teardown(&fixture);
}

/*
 * A read never brings a scope into being.
 *
 * sqlite3_open() creates whatever it is given, so a plain open here
 * would leave a team-<id>.db behind for every team anybody ever searched
 * from -- and "which scopes hold anything" would then answer "all of
 * them, all empty".
 */
static void
test_reading_a_missing_scope_creates_nothing(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *path = NULL;

    fixture_setup(&fixture);

    path = clawt_memory_scopes_path_for(fixture.scopes,
                                        CLAWT_MEMORY_SCOPE_TEAM, "build");

    g_assert_null(clawt_memory_scopes_open_for_read(
        fixture.scopes, CLAWT_MEMORY_SCOPE_TEAM, "build"));
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

    /* And the fan-out that consults it leaves nothing behind either. */
    {
        g_autoptr(GPtrArray) found = clawt_memory_scopes_list(
            fixture.scopes, NULL, "build", NULL, FALSE, 0, NULL);

        g_assert_cmpuint(found->len, ==, 0);
        g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    }

    fixture_teardown(&fixture);
}

/*
 * A write to a scope the agent has no key for is refused, and says why.
 *
 * Falling back to the agent's own store would leave it believing it had
 * shared something nobody else can see, which is worse than not writing
 * it: the whole purpose of a team memory is that the team reads it.
 */
static void
test_team_scope_without_a_team_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);

    g_assert_null(clawt_memory_scopes_open_for_write(
        fixture.scopes, CLAWT_MEMORY_SCOPE_TEAM, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    /* And the reason names the team, not just "invalid". */
    g_assert_nonnull(strstr(error->message, "team"));

    fixture_teardown(&fixture);
}

/*
 * The fan-out reads three databases and each result says which.
 *
 * The scope tag is not decoration: a listing that mixes an agent's own
 * conclusion with something the fleet believes, and does not say which
 * is which, has turned two different claims into the same row.
 */
static void
test_reading_fans_out_and_says_where_from(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *own_path = NULL;
    g_autoptr(ClawtMemoryStore) own = NULL;
    ClawtMemoryStore *team;
    ClawtMemoryStore *fleet;
    g_autoptr(GPtrArray) found = NULL;
    gboolean saw_agent = FALSE;
    gboolean saw_team = FALSE;
    gboolean saw_fleet = FALSE;
    guint i;

    fixture_setup(&fixture);

    own_path = clawt_memory_scopes_path_for(fixture.scopes,
                                            CLAWT_MEMORY_SCOPE_AGENT,
                                            "alpha");
    own = clawt_memory_store_new(own_path, NULL);
    g_assert_nonnull(own);

    team = clawt_memory_scopes_open_for_write(fixture.scopes,
                                              CLAWT_MEMORY_SCOPE_TEAM,
                                              "build", NULL);
    fleet = clawt_memory_scopes_open_for_write(fixture.scopes,
                                               CLAWT_MEMORY_SCOPE_FLEET,
                                               NULL, NULL);
    g_assert_nonnull(team);
    g_assert_nonnull(fleet);

    remember(own, "the runner needs a bigger disk", 3000);
    remember(team, "the build box is on the tailnet", 2000);
    remember(fleet, "the operator works UTC-5", 1000);

    found = clawt_memory_scopes_list(fixture.scopes, own, "build", NULL,
                                     FALSE, 0, NULL);

    g_assert_cmpuint(found->len, ==, 3);

    for (i = 0; i < found->len; i++) {
        ClawtMemory *memory = g_ptr_array_index(found, i);

        g_assert_nonnull(memory->scope);

        if (g_strcmp0(memory->scope, "agent") == 0)
            saw_agent = TRUE;
        else if (g_strcmp0(memory->scope, "team") == 0)
            saw_team = TRUE;
        else if (g_strcmp0(memory->scope, "fleet") == 0)
            saw_fleet = TRUE;
    }

    g_assert_true(saw_agent);
    g_assert_true(saw_team);
    g_assert_true(saw_fleet);

    /*
     * And an agent on a different team reads two of the three.  The one
     * it cannot see is not filtered out of the result -- the file it
     * lives in is never opened.
     */
    {
        g_autoptr(GPtrArray) elsewhere = clawt_memory_scopes_list(
            fixture.scopes, own, "research", NULL, FALSE, 0, NULL);
        guint j;

        g_assert_cmpuint(elsewhere->len, ==, 2);

        for (j = 0; j < elsewhere->len; j++) {
            ClawtMemory *memory = g_ptr_array_index(elsewhere, j);

            g_assert_cmpstr(memory->scope, !=, "team");
        }
    }

    fixture_teardown(&fixture);
}

/*
 * Search fans out the same way listing does.
 *
 * Two implementations of the merge would be two orderings, and the one
 * nobody looked at would be the one somebody hit.
 */
static void
test_search_fans_out_too(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *own_path = NULL;
    g_autoptr(ClawtMemoryStore) own = NULL;
    ClawtMemoryStore *fleet;
    g_autoptr(GPtrArray) found = NULL;

    fixture_setup(&fixture);

    own_path = clawt_memory_scopes_path_for(fixture.scopes,
                                            CLAWT_MEMORY_SCOPE_AGENT,
                                            "alpha");
    own = clawt_memory_store_new(own_path, NULL);
    fleet = clawt_memory_scopes_open_for_write(fixture.scopes,
                                               CLAWT_MEMORY_SCOPE_FLEET,
                                               NULL, NULL);

    remember(own, "postgres runs as a quadlet", 2000);
    remember(fleet, "postgres is the fleet's memory backend", 1000);
    remember(own, "lunch is at one", 3000);

    found = clawt_memory_scopes_search(fixture.scopes, own, NULL, "postgres",
                                       NULL, 0, NULL);

    g_assert_cmpuint(found->len, ==, 2);

    fixture_teardown(&fixture);
}

/*
 * The limit applies to the merged result, not to each scope.
 *
 * Asking each store for a share of it would let a fleet database holding
 * nothing relevant cost the agent's own store half its results.
 */
static void
test_the_limit_is_on_the_merge(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *own_path = NULL;
    g_autoptr(ClawtMemoryStore) own = NULL;
    ClawtMemoryStore *fleet;
    g_autoptr(GPtrArray) found = NULL;
    gint i;

    fixture_setup(&fixture);

    own_path = clawt_memory_scopes_path_for(fixture.scopes,
                                            CLAWT_MEMORY_SCOPE_AGENT,
                                            "alpha");
    own = clawt_memory_store_new(own_path, NULL);
    fleet = clawt_memory_scopes_open_for_write(fixture.scopes,
                                               CLAWT_MEMORY_SCOPE_FLEET,
                                               NULL, NULL);

    for (i = 0; i < 5; i++) {
        g_autofree gchar *mine = g_strdup_printf("mine %d", i);
        g_autofree gchar *theirs = g_strdup_printf("theirs %d", i);

        remember(own, mine, 1000 + i);
        remember(fleet, theirs, 2000 + i);
    }

    found = clawt_memory_scopes_list(fixture.scopes, own, NULL, NULL, FALSE,
                                     3, NULL);

    g_assert_cmpuint(found->len, ==, 3);

    fixture_teardown(&fixture);
}

/*
 * A budget applied to UTF-8 lands mid-sequence about half the time.
 *
 * What comes out is not shorter text: it is text ending in half a
 * character, which reads as a corrupt transcript rather than a truncated
 * one -- and json-glib and sqlite both carry it onwards without
 * complaining.  Here because the summariser's budget is the caller that
 * made it matter.
 */
static void
test_a_budget_never_slices_a_character(void)
{
    /* Three bytes each, so every budget that is not a multiple of 3 cuts. */
    const gchar *text = "日本語のテキストです";
    gsize budget;

    for (budget = 1; budget <= strlen(text) + 2; budget++) {
        g_autofree gchar *head = clawt_utf8_truncate(text, budget, FALSE);
        g_autofree gchar *tail = clawt_utf8_truncate(text, budget, TRUE);

        g_assert_nonnull(head);
        g_assert_nonnull(tail);

        /* Valid, and inside the budget rather than one character over. */
        g_assert_true(g_utf8_validate(head, -1, NULL));
        g_assert_true(g_utf8_validate(tail, -1, NULL));
        g_assert_cmpuint(strlen(head), <=, budget);
        g_assert_cmpuint(strlen(tail), <=, budget);
    }

    /* Text already inside the budget is copied whole. */
    {
        g_autofree gchar *whole = clawt_utf8_truncate(text, 4096, TRUE);

        g_assert_cmpstr(whole, ==, text);
    }
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/memory-scope/each-scope-is-a-file",
                    test_each_scope_is_its_own_file);
    g_test_add_func("/memory-scope/read-creates-nothing",
                    test_reading_a_missing_scope_creates_nothing);
    g_test_add_func("/memory-scope/team-without-a-team-refused",
                    test_team_scope_without_a_team_is_refused);
    g_test_add_func("/memory-scope/fan-out-says-where-from",
                    test_reading_fans_out_and_says_where_from);
    g_test_add_func("/memory-scope/search-fans-out",
                    test_search_fans_out_too);
    g_test_add_func("/memory-scope/limit-is-on-the-merge",
                    test_the_limit_is_on_the_merge);
    g_test_add_func("/memory-scope/budget-keeps-utf8-whole",
                    test_a_budget_never_slices_a_character);

    return g_test_run();
}
