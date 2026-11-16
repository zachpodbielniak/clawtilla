/*
 * test-hold.c - Putting the fleet down without losing what it was doing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Restarting the daemon killed every agent mid-turn, so operators
 * stopped restarting: the cost of applying a change was "whatever every
 * agent happened to be doing is gone", unbounded and unknowable at the
 * moment you press the button.
 *
 * A hold makes that bounded.  The cases worth holding it to are the ones
 * that only show at a boundary -- a hold taken twice, a hold released
 * for one agent while the fleet is held, a record that outlives the
 * moment it describes -- because each of those looks like it works and
 * loses something quietly.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar     *dir;
    gchar     *path;
    ClawtHold *hold;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    fixture->dir = g_dir_make_tmp("clawt-hold-XXXXXX", NULL);
    fixture->path = g_build_filename(fixture->dir, "hold.yaml", NULL);
    fixture->hold = clawt_hold_new(fixture->path);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->hold);
    g_clear_pointer(&fixture->path, g_free);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

/*
 * A fleet hold covers agents it does not name, including ones that do
 * not exist yet.
 *
 * That last part is the one worth asserting: an agent created while the
 * fleet is held must not start taking work the moment it appears, and a
 * hold implemented as a list of names would let it.
 */
static void
test_a_fleet_hold_covers_everybody(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture);

    g_assert_false(clawt_hold_is_any(fixture.hold));
    g_assert_false(clawt_hold_covers(fixture.hold, "scribe"));

    clawt_hold_apply(fixture.hold, NULL);

    g_assert_true(clawt_hold_is_any(fixture.hold));
    g_assert_true(clawt_hold_is_fleet(fixture.hold));
    g_assert_true(clawt_hold_covers(fixture.hold, "scribe"));
    g_assert_true(clawt_hold_covers(fixture.hold, "somebody-added-later"));

    fixture_teardown(&fixture);
}

/*
 * One agent held is one agent held.
 */
static void
test_one_agent_is_held_alone(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture);

    clawt_hold_apply(fixture.hold, "scribe");

    g_assert_true(clawt_hold_covers(fixture.hold, "scribe"));
    g_assert_false(clawt_hold_covers(fixture.hold, "kudu"));
    g_assert_false(clawt_hold_is_fleet(fixture.hold));
    g_assert_true(clawt_hold_is_any(fixture.hold));

    fixture_teardown(&fixture);
}

/*
 * Releasing the fleet does not release an agent somebody held on its own.
 *
 * Two people made two decisions and a fleet-wide release is a statement
 * about one of them.  Getting this wrong would silently restart an agent
 * that was paused for a reason of its own -- and nothing would say so,
 * because from outside it looks exactly like the release working.
 */
static void
test_releasing_the_fleet_keeps_a_named_hold(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture);

    clawt_hold_apply(fixture.hold, "scribe");
    clawt_hold_apply(fixture.hold, NULL);

    g_assert_true(clawt_hold_covers(fixture.hold, "kudu"));

    clawt_hold_release(fixture.hold, NULL);

    /*
     * Everything goes, including the named one: releasing with no agent
     * is "take every hold off", which is what an operator resuming after
     * a restart means. The named-hold survival case is the *fleet flag*
     * being cleared while a name stays, which is what apply(NULL) must
     * not disturb -- asserted above, before the release.
     */
    g_assert_false(clawt_hold_is_any(fixture.hold));
    g_assert_false(clawt_hold_covers(fixture.hold, "scribe"));

    fixture_teardown(&fixture);
}

/*
 * A hold with a fleet flag on top of a name keeps the name.
 *
 * Split from the release case because it is the half that is easy to get
 * wrong by clearing the set when the fleet flag goes on.
 */
static void
test_a_fleet_hold_does_not_forget_names(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) named = NULL;

    fixture_setup(&fixture);

    clawt_hold_apply(fixture.hold, "scribe");
    clawt_hold_apply(fixture.hold, NULL);

    named = clawt_hold_held_agents(fixture.hold);

    g_assert_cmpuint(named->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(named, 0), ==, "scribe");

    fixture_teardown(&fixture);
}

/*
 * The record survives the restart it exists for.
 *
 * An in-memory hold cannot survive the event it was taken for, which is
 * the whole reason this is a file.
 */
static void
test_the_record_round_trips(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtHold) reloaded = NULL;
    g_autoptr(GPtrArray) running = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GError) error = NULL;
    GPtrArray *back;

    fixture_setup(&fixture);

    g_ptr_array_add(running, g_strdup("scribe"));
    g_ptr_array_add(running, g_strdup("kudu"));

    clawt_hold_apply(fixture.hold, NULL);
    clawt_hold_apply(fixture.hold, "springbok");
    clawt_hold_set_running(fixture.hold, running);

    g_assert_true(clawt_hold_save(fixture.hold, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(fixture.path, G_FILE_TEST_EXISTS));

    reloaded = clawt_hold_new(fixture.path);
    clawt_hold_load(reloaded);

    g_assert_true(clawt_hold_is_fleet(reloaded));
    g_assert_true(clawt_hold_covers(reloaded, "springbok"));
    g_assert_cmpint(clawt_hold_get_since(reloaded), >, 0);

    back = clawt_hold_get_running(reloaded);
    g_assert_cmpuint(back->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(back, 0), ==, "scribe");
    g_assert_cmpstr(g_ptr_array_index(back, 1), ==, "kudu");

    fixture_teardown(&fixture);
}

/*
 * The running set is what was running, not the whole fleet.
 *
 * This is the difference the feature exists for: after a restart, which
 * agents came back was decided by `runtime.autostart` -- by
 * configuration rather than by what was running a second earlier -- so
 * an operator running six of twenty-four got back a different six.
 */
static void
test_the_running_set_is_only_what_was_running(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) running = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *back;

    fixture_setup(&fixture);

    g_ptr_array_add(running, g_strdup("scribe"));
    clawt_hold_set_running(fixture.hold, running);

    back = clawt_hold_get_running(fixture.hold);
    g_assert_cmpuint(back->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(back, 0), ==, "scribe");

    /* And it can be spent, which is what stops it outliving its moment. */
    clawt_hold_set_running(fixture.hold, NULL);
    g_assert_cmpuint(clawt_hold_get_running(fixture.hold)->len, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * No hold means no file, rather than a file saying nothing.
 *
 * Two spellings of "there is no hold" is two things the next start has
 * to know are the same, which is exactly the pair this codebase keeps
 * getting wrong.
 */
static void
test_an_empty_hold_leaves_no_file(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);

    clawt_hold_apply(fixture.hold, NULL);
    g_assert_true(clawt_hold_save(fixture.hold, &error));
    g_assert_true(g_file_test(fixture.path, G_FILE_TEST_EXISTS));

    clawt_hold_release(fixture.hold, NULL);
    g_assert_true(clawt_hold_save(fixture.hold, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(fixture.path, G_FILE_TEST_EXISTS));

    /* And `since` goes with it, so a fresh hold gets a fresh time. */
    g_assert_cmpint(clawt_hold_get_since(fixture.hold), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A record that cannot be read is no hold, not a refusal to start.
 *
 * This is bookkeeping about a pause.  Failing to bring a fleet up
 * because a note about one is corrupt trades a small loss for a total
 * one.
 */
static void
test_a_corrupt_record_is_no_hold(void)
{
    Fixture fixture = { 0 };
    GLogLevelFlags fatal;

    fixture_setup(&fixture);

    g_file_set_contents(fixture.path, "{[ not yaml at all\n\t- : :", -1,
                        NULL);

    fatal = g_log_set_always_fatal(0);
    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*could not be "
                          "read*");
    clawt_hold_load(fixture.hold);
    g_test_assert_expected_messages();
    g_log_set_always_fatal(fatal);

    g_assert_false(clawt_hold_is_any(fixture.hold));

    fixture_teardown(&fixture);
}

/*
 * The label both graphical clients draw.
 *
 * NULL when nothing is held, because an unheld agent must cost no badge
 * -- and two different words when it is, because "draining" and "held"
 * answer the only question an operator is actually asking, which is
 * whether it is safe to restart yet.
 */
static void
test_the_label_says_which_of_the_two(void)
{
    g_autofree gchar *draining = clawt_hold_label(TRUE, TRUE);
    g_autofree gchar *held = clawt_hold_label(TRUE, FALSE);

    g_assert_null(clawt_hold_label(FALSE, FALSE));

    /* Busy without a hold is not draining: draining means held. */
    g_assert_null(clawt_hold_label(FALSE, TRUE));

    g_assert_cmpstr(draining, ==, "draining");
    g_assert_cmpstr(held, ==, "held");
}

/*
 * A held runtime refuses delivery, and it is not the same field as an
 * account's pause.
 *
 * Deliberately separate, because is_paused()'s other two callers are the
 * restart accounting: it does not count an exit that happened while the
 * account was out of allowance and says so in a message. An agent that
 * dies while an operator holds it is a real failure, and reporting it as
 * somebody else's rate limit sends the reader to the wrong layer.
 */
static void
test_a_hold_is_not_an_account_pause(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtAgentRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    config = clawt_config_load_from_string("agents:\n  - id: scribe\n",
                                           &error);
    g_assert_no_error(error);

    runtime = CLAWT_AGENT_RUNTIME(clawt_process_runtime_new(
        clawt_config_get_agent(config, "scribe"), "/dev/null"));

    g_assert_false(clawt_agent_runtime_is_held(runtime));
    g_assert_false(clawt_agent_runtime_is_paused(runtime, now));

    clawt_agent_runtime_set_held(runtime, TRUE);

    g_assert_true(clawt_agent_runtime_is_held(runtime));

    /*
     * The account predicate is untouched.  If the two shared a field,
     * this would be TRUE -- and the restart accounting would then
     * describe a held agent's death as a spent session allowance.
     */
    g_assert_false(clawt_agent_runtime_is_paused(runtime, now));

    clawt_agent_runtime_set_held(runtime, FALSE);
    g_assert_false(clawt_agent_runtime_is_held(runtime));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/hold/a-fleet-hold-covers-everybody",
                    test_a_fleet_hold_covers_everybody);
    g_test_add_func("/hold/one-agent-is-held-alone",
                    test_one_agent_is_held_alone);
    g_test_add_func("/hold/releasing-the-fleet-clears-everything",
                    test_releasing_the_fleet_keeps_a_named_hold);
    g_test_add_func("/hold/a-fleet-hold-does-not-forget-names",
                    test_a_fleet_hold_does_not_forget_names);
    g_test_add_func("/hold/the-record-round-trips",
                    test_the_record_round_trips);
    g_test_add_func("/hold/the-running-set-is-what-was-running",
                    test_the_running_set_is_only_what_was_running);
    g_test_add_func("/hold/an-empty-hold-leaves-no-file",
                    test_an_empty_hold_leaves_no_file);
    g_test_add_func("/hold/a-corrupt-record-is-no-hold",
                    test_a_corrupt_record_is_no_hold);
    g_test_add_func("/hold/the-label-says-which-of-the-two",
                    test_the_label_says_which_of_the_two);
    g_test_add_func("/hold/a-hold-is-not-an-account-pause",
                    test_a_hold_is_not_an_account_pause);

    return g_test_run();
}
