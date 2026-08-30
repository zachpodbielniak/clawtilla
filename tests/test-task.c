/*
 * test-task.c - What a task is, and how it reads
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

/*
 * Every state the enum knows has a tone, and it is one of the five.
 *
 * Walked from CLAWT_TYPE_TASK_STATE rather than written out, which is
 * the whole point: the code this replaces compared the state against
 * "done" and "complete", neither of which the enum has ever produced, so
 * a completed task was drawn in the fallback colour and no test noticed
 * because no test asked about a state the author had not thought of.
 *
 * A state added later and left unclassified also draws a -Wswitch warning
 * from clawt_task_state_tone(), which names every value and has no
 * `default:`. That is a warning and not an error -- this tree does not
 * build with -Werror -- so it stops somebody only by the zero-warning
 * rule. This test is the half that fails on its own, and it catches the
 * other shape too: a state handled with a tone that is not a tone.
 */
static void
test_every_state_has_a_tone(void)
{
    g_autoptr(GEnumClass) states = g_type_class_ref(CLAWT_TYPE_TASK_STATE);
    static const gchar * const tones[] = {
        "neutral", "good", "warn", "bad", "info", NULL
    };
    guint i;

    g_assert_cmpuint(states->n_values, >, 0);

    for (i = 0; i < states->n_values; i++) {
        const GEnumValue *value = &states->values[i];
        const gchar *tone = clawt_task_state_tone(value->value);
        gboolean known = FALSE;
        guint t;

        g_assert_nonnull(tone);

        for (t = 0; tones[t] != NULL; t++) {
            if (g_strcmp0(tone, tones[t]) == 0)
                known = TRUE;
        }

        /*
         * Named in the failure, because "a tone was not one of the
         * tones" sends the reader to the vocabulary and not to the
         * state that has no entry.
         */
        if (!known)
            g_error("state '%s' has tone '%s', which is not a tone",
                    value->value_nick, tone);
    }
}

/*
 * And the particular answers, so a refactor cannot quietly make them all
 * neutral and still pass the test above.
 *
 * The completed case is the regression: it is what the web client got
 * wrong for as long as that badge has existed.
 */
static void
test_the_tones_are_the_ones_intended(void)
{
    g_assert_cmpstr(clawt_task_state_tone(CLAWT_TASK_COMPLETED), ==, "good");
    g_assert_cmpstr(clawt_task_state_tone(CLAWT_TASK_FAILED), ==, "bad");
    g_assert_cmpstr(clawt_task_state_tone(CLAWT_TASK_CANCELLED), ==, "bad");
    g_assert_cmpstr(clawt_task_state_tone(CLAWT_TASK_STALLED), ==, "warn");
    g_assert_cmpstr(clawt_task_state_tone(CLAWT_TASK_RUNNING), ==, "info");
    g_assert_cmpstr(clawt_task_state_tone(CLAWT_TASK_PENDING), ==, "neutral");
}

/*
 * A finished task and an unfinished one do not share a tone.
 *
 * The property behind the bug, rather than the spelling of it: the
 * reader of a task list is asking which rows are still going, and the
 * failure was that "completed" answered the same way "pending" does.
 * This holds whatever the palette becomes.
 */
static void
test_finished_never_reads_as_pending(void)
{
    g_autoptr(GEnumClass) states = g_type_class_ref(CLAWT_TYPE_TASK_STATE);
    const gchar *pending = clawt_task_state_tone(CLAWT_TASK_PENDING);
    guint i;

    for (i = 0; i < states->n_values; i++) {
        g_autoptr(ClawtTask) task = clawt_task_new("someone", "someone-else",
                                                   "do the thing");

        clawt_task_set_state(task, states->values[i].value);

        if (!clawt_task_is_finished(task))
            continue;

        if (g_strcmp0(clawt_task_state_tone(states->values[i].value),
                      pending) == 0)
            g_error("finished state '%s' reads exactly like pending",
                    states->values[i].value_nick);
    }
}

/*
 * The nickname the daemon sends round-trips into the enum.
 *
 * Both clients turn the wire's string into a #ClawtTaskState and ask the
 * library; if that lookup failed they would both fall back to neutral
 * and the bug would be intact behind a nicer-looking function.
 */
static void
test_every_nick_resolves_back(void)
{
    g_autoptr(GEnumClass) states = g_type_class_ref(CLAWT_TYPE_TASK_STATE);
    guint i;

    for (i = 0; i < states->n_values; i++) {
        const gchar *nick = states->values[i].value_nick;
        gint value = 0;

        g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, nick,
                                           &value));
        g_assert_cmpint(value, ==, states->values[i].value);
        g_assert_cmpstr(clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, value),
                        ==, nick);
    }

    /*
     * And the two the old code believed in are still not states, which
     * is what made it wrong.  Asserted rather than assumed: if either
     * were ever added, this test is where somebody finds out that the
     * fix and the addition disagree.
     */
    {
        gint value = 0;

        g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, "done",
                                            &value));
        g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE,
                                            "complete", &value));
    }
}


/*
 * A chain of delegations is a chain in the record too.
 *
 * clawtilla_delegate passed NULL as the parent for its whole life, so
 * every task an agent created was a root: this measured 0 for all of
 * them and clawt_task_manager_create()'s own comment -- "a task spawning
 * a task spawning a task is three levels deep" -- described the case it
 * was failing to catch.
 */
static void
test_a_delegation_chain_records_its_depth(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    ClawtTask *root;
    ClawtTask *child;
    ClawtTask *grandchild;

    root = clawt_task_manager_create(tasks, "chief", "lead", "survey it",
                                     NULL, NULL);
    g_assert_nonnull(root);
    g_assert_cmpint(clawt_task_get_depth(root), ==, 0);

    child = clawt_task_manager_create(tasks, "lead", "worker", "one half",
                                      clawt_task_get_id(root), NULL);
    g_assert_nonnull(child);
    g_assert_cmpint(clawt_task_get_depth(child), ==, 1);
    g_assert_cmpstr(clawt_task_get_parent_id(child), ==,
                    clawt_task_get_id(root));

    grandchild = clawt_task_manager_create(tasks, "worker", "specialist",
                                           "the awkward bit",
                                           clawt_task_get_id(child), NULL);
    g_assert_nonnull(grandchild);
    g_assert_cmpint(clawt_task_get_depth(grandchild), ==, 2);
}

/*
 * And the limit that depth exists for can actually be reached.
 *
 * A limit needs a test that reaches it: this one could not fire at all
 * while every task was a root, so the guard was live code that nothing
 * could trip.
 */
static void
test_the_depth_limit_stops_a_chain(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    g_autoptr(GError) error = NULL;
    ClawtTask *root;
    ClawtTask *child;
    ClawtTask *refused;

    clawt_task_manager_set_max_depth(tasks, 2);

    root = clawt_task_manager_create(tasks, "chief", "lead", "survey it",
                                     NULL, NULL);
    g_assert_nonnull(root);

    child = clawt_task_manager_create(tasks, "lead", "worker", "one half",
                                      clawt_task_get_id(root), NULL);
    g_assert_nonnull(child);

    refused = clawt_task_manager_create(tasks, "worker", "specialist",
                                        "deeper still",
                                        clawt_task_get_id(child), &error);

    g_assert_null(refused);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);

    /* A sentence an agent can act on, not a code. */
    g_assert_nonnull(strstr(error->message, "yourself"));
}

/*
 * Cancelling a task takes its whole subtree with it.
 *
 * clawtilla_task_cancel promises "and everything it spawned" and the
 * walk that delivers it matches on parent_id, so with every task a root
 * it cancelled exactly one thing while the fan-out carried on reporting
 * into a task nobody was waiting for.
 */
static void
test_cancelling_reaches_the_whole_subtree(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    ClawtTask *root;
    ClawtTask *child;
    ClawtTask *sibling;
    ClawtTask *grandchild;
    ClawtTask *unrelated;

    root = clawt_task_manager_create(tasks, "chief", "lead", "survey it",
                                     NULL, NULL);
    child = clawt_task_manager_create(tasks, "lead", "kudu", "half of it",
                                      clawt_task_get_id(root), NULL);
    sibling = clawt_task_manager_create(tasks, "lead", "mamba", "the rest",
                                        clawt_task_get_id(root), NULL);
    grandchild = clawt_task_manager_create(tasks, "kudu", "oryx", "a detail",
                                           clawt_task_get_id(child), NULL);
    unrelated = clawt_task_manager_create(tasks, "chief", "scribe",
                                          "something else", NULL, NULL);

    g_assert_cmpuint(clawt_task_manager_cancel(tasks,
                                               clawt_task_get_id(root),
                                               "operator changed their mind"),
                     ==, 4);

    g_assert_cmpint(clawt_task_get_state(root), ==, CLAWT_TASK_CANCELLED);
    g_assert_cmpint(clawt_task_get_state(child), ==, CLAWT_TASK_CANCELLED);
    g_assert_cmpint(clawt_task_get_state(sibling), ==, CLAWT_TASK_CANCELLED);
    g_assert_cmpint(clawt_task_get_state(grandchild), ==,
                    CLAWT_TASK_CANCELLED);

    /* And nothing outside the subtree. */
    g_assert_cmpint(clawt_task_get_state(unrelated), ==, CLAWT_TASK_PENDING);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/task/every-state-has-a-tone",
                    test_every_state_has_a_tone);
    g_test_add_func("/task/tones-are-the-ones-intended",
                    test_the_tones_are_the_ones_intended);
    g_test_add_func("/task/finished-never-reads-as-pending",
                    test_finished_never_reads_as_pending);
    g_test_add_func("/task/every-nick-resolves-back",
                    test_every_nick_resolves_back);
    g_test_add_func("/task/a-delegation-chain-records-its-depth",
                    test_a_delegation_chain_records_its_depth);
    g_test_add_func("/task/the-depth-limit-stops-a-chain",
                    test_the_depth_limit_stops_a_chain);
    g_test_add_func("/task/cancelling-reaches-the-whole-subtree",
                    test_cancelling_reaches_the_whole_subtree);

    return g_test_run();
}
