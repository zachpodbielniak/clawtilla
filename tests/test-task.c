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

/*
 * A task is not finished because its assignee stopped talking.
 *
 * The daemon completes a task from the message that ends its assignee's
 * turn, since an AI CLI cannot end one without writing something.  An
 * assignee that finishes its share, hands the rest on and reports once
 * at the end is not busy when its turn ends -- so the task closed
 * carrying a status note that said in so many words that the report had
 * not been sent yet, and the delegator stopped polling.
 */
static void
test_a_task_with_children_running_does_not_auto_complete(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    ClawtTask *root;
    ClawtTask *child;
    g_autofree gchar *held = NULL;
    g_autofree gchar *held_again = NULL;

    root = clawt_task_manager_create(tasks, "chief", "oryx", "verify it",
                                     NULL, NULL);
    child = clawt_task_manager_create(tasks, "oryx", "kudu", "the same, here",
                                      clawt_task_get_id(root), NULL);

    g_assert_cmpuint(clawt_task_manager_count_unfinished_children(
                         tasks, clawt_task_get_id(root)), ==, 1);

    g_assert_false(clawt_task_manager_complete_on_turn_end(
                       tasks, clawt_task_get_id(root), "oryx",
                       "mine is done, kudu is still going", &held));

    g_assert_nonnull(held);
    g_assert_nonnull(strstr(held, "still running"));
    g_assert_false(clawt_task_is_finished(root));

    /*
     * And what it said is kept, because it is still the freshest thing
     * anybody knows about that task.
     */
    g_assert_cmpstr(clawt_task_get_progress_note(root), ==,
                    "mine is done, kudu is still going");

    /* Once the child ends, the same turn ending does complete it. */
    g_assert_true(clawt_task_manager_complete(tasks,
                                              clawt_task_get_id(child),
                                              "clean here"));

    g_assert_true(clawt_task_manager_complete_on_turn_end(
                      tasks, clawt_task_get_id(root), "oryx",
                      "all three verified", &held_again));

    g_assert_null(held_again);
    g_assert_cmpint(clawt_task_get_state(root), ==, CLAWT_TASK_COMPLETED);
    g_assert_cmpstr(clawt_task_get_result(root), ==, "all three verified");
}

/*
 * And an assignee can say so itself, for the turn where it has no
 * children to point at -- it scheduled a wakeup, or is waiting on
 * somebody outside the fleet.
 */
static void
test_a_progress_note_holds_the_task_open_for_one_turn(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    ClawtTask *task;
    g_autofree gchar *held = NULL;
    g_autofree gchar *held_again = NULL;

    task = clawt_task_manager_create(tasks, "chief", "oryx", "verify it",
                                     NULL, NULL);

    g_assert_true(clawt_task_manager_note_progress(
                      tasks, clawt_task_get_id(task),
                      "waiting on the guest to finish booting"));

    /*
     * Picked up, and said so: clawtilla_delegate marked nothing running,
     * so a delegator reading `pending` concluded nobody had started and
     * delegated it again.
     */
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);
    g_assert_cmpstr(clawt_task_get_progress_note(task), ==,
                    "waiting on the guest to finish booting");

    g_assert_false(clawt_task_manager_complete_on_turn_end(
                       tasks, clawt_task_get_id(task), "oryx",
                       "back shortly", &held));
    g_assert_nonnull(held);
    g_assert_false(clawt_task_is_finished(task));

    /*
     * One turn, not for ever.  A hold that outlived its turn would mean
     * a task could never finish by inference again, and the next turn
     * ending is exactly when the work usually is done.
     */
    g_assert_true(clawt_task_manager_complete_on_turn_end(
                      tasks, clawt_task_get_id(task), "oryx",
                      "all three verified", &held_again));
    g_assert_null(held_again);
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
}

/*
 * "They reported a result" and "they stopped talking" are different
 * facts, and a delegator that cannot tell them apart either waits on
 * finished work or re-runs work that is done.
 */
static void
test_an_inferred_result_says_so(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    ClawtTask *inferred;
    ClawtTask *reported;

    inferred = clawt_task_manager_create(tasks, "chief", "oryx", "one",
                                         NULL, NULL);
    reported = clawt_task_manager_create(tasks, "chief", "kudu", "two",
                                         NULL, NULL);

    g_assert_true(clawt_task_manager_complete_on_turn_end(
                      tasks, clawt_task_get_id(inferred), "oryx",
                      "seems fine", NULL));
    g_assert_true(clawt_task_get_result_inferred(inferred));

    g_assert_true(clawt_task_manager_complete(tasks,
                                              clawt_task_get_id(reported),
                                              "verified, all clean"));
    g_assert_false(clawt_task_get_result_inferred(reported));
}

/*
 * A thread is not only the assignee's, and a turn ending in it is only
 * evidence about the work when it is the assignee's own.
 *
 * The delegator ends turns in the thread too -- reading a progress note
 * is a turn -- and completing on whichever turn ended next recorded
 * "Thanks, carry on" as the result of work that was still running.
 * Worse, the progress hold consumes itself when checked: the
 * delegator's turn spent the hold the assignee had armed, so the
 * assignee's next status note completed the task it had just, in so
 * many words, asked to keep open.
 */
static void
test_only_the_assignees_turn_ends_a_task(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    ClawtTask *task;
    g_autofree gchar *reason = NULL;
    g_autofree gchar *held = NULL;

    task = clawt_task_manager_create(tasks, "chief", "oryx", "verify it",
                                     NULL, NULL);

    g_assert_true(clawt_task_manager_note_progress(
                      tasks, clawt_task_get_id(task),
                      "fedora is clean; two guests to go"));

    /* The delegator read the note, and its turn ended. */
    g_assert_false(clawt_task_manager_complete_on_turn_end(
                       tasks, clawt_task_get_id(task), "chief",
                       "Thanks -- carry on.", &reason));
    g_assert_nonnull(reason);
    g_assert_nonnull(strstr(reason, "oryx"));
    g_assert_false(clawt_task_is_finished(task));
    g_assert_null(clawt_task_get_result(task));

    /* And it did not spend the hold the assignee armed. */
    g_assert_false(clawt_task_manager_complete_on_turn_end(
                       tasks, clawt_task_get_id(task), "oryx",
                       "back shortly", &held));
    g_assert_nonnull(held);
    g_assert_nonnull(strstr(held, "progress"));
    g_assert_false(clawt_task_is_finished(task));

    /* The assignee's own next turn end completes it, as ever. */
    g_assert_true(clawt_task_manager_complete_on_turn_end(
                      tasks, clawt_task_get_id(task), "oryx",
                      "all three verified", NULL));
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
}

/*
 * A delegator can see the whole tree under the work it handed out.
 *
 * clawt_task_manager_list_involving() stops one level down, so a chief
 * that gave a lead a job could not list what the lead gave anybody: the
 * parent read `completed`, the children were invisible, and nothing
 * distinguished finished from evaporated from still-running.
 */
static void
test_a_delegator_sees_what_its_work_turned_into(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    g_autoptr(GPtrArray) below = NULL;
    g_autoptr(GPtrArray) leads = NULL;
    ClawtTask *root;
    ClawtTask *child;
    ClawtTask *grandchild;

    root = clawt_task_manager_create(tasks, "chief", "oryx", "verify it",
                                     NULL, NULL);
    child = clawt_task_manager_create(tasks, "oryx", "kudu", "here too",
                                      clawt_task_get_id(root), NULL);
    grandchild = clawt_task_manager_create(tasks, "kudu", "mamba",
                                           "and here",
                                           clawt_task_get_id(child), NULL);

    /* Somebody else's chain entirely. */
    clawt_task_manager_create(tasks, "scribe", "mamba", "unrelated",
                              NULL, NULL);

    below = clawt_task_manager_list_descendants(tasks, "chief", FALSE);

    g_assert_cmpuint(below->len, ==, 2);
    g_assert_true(g_ptr_array_find(below, child, NULL));
    g_assert_true(g_ptr_array_find(below, grandchild, NULL));

    /*
     * And not the chief's own task, which belongs to the other listing.
     * Returned by both, it would be drawn twice under two headings that
     * mean different things.
     */
    g_assert_false(g_ptr_array_find(below, root, NULL));

    /* The lead sees its own fan-out and nothing above it. */
    leads = clawt_task_manager_list_descendants(tasks, "oryx", FALSE);
    g_assert_cmpuint(leads->len, ==, 1);
    g_assert_true(g_ptr_array_find(leads, grandchild, NULL));
}

/*
 * A parent id naming a task that is not there stops the walk rather
 * than the daemon.  Tasks are in memory and a restart clears them, so a
 * dangling parent is an ordinary state and not a corruption.
 */
static void
test_a_missing_parent_ends_the_walk(void)
{
    g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
    g_autoptr(GPtrArray) below = NULL;
    ClawtTask *orphan;

    orphan = clawt_task_manager_create(tasks, "kudu", "mamba", "a detail",
                                       NULL, NULL);
    clawt_task_set_parent_id(orphan, "task-that-went-away");

    below = clawt_task_manager_list_descendants(tasks, "chief", FALSE);

    g_assert_cmpuint(below->len, ==, 0);
    g_assert_cmpuint(clawt_task_manager_count_unfinished_children(
                         tasks, "task-that-went-away"), ==, 1);
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
    g_test_add_func("/task/children-running-blocks-auto-complete",
                    test_a_task_with_children_running_does_not_auto_complete);
    g_test_add_func("/task/a-progress-note-holds-it-open",
                    test_a_progress_note_holds_the_task_open_for_one_turn);
    g_test_add_func("/task/only-the-assignee-ends-it",
                    test_only_the_assignees_turn_ends_a_task);
    g_test_add_func("/task/an-inferred-result-says-so",
                    test_an_inferred_result_says_so);
    g_test_add_func("/task/a-delegator-sees-the-fan-out",
                    test_a_delegator_sees_what_its_work_turned_into);
    g_test_add_func("/task/a-missing-parent-ends-the-walk",
                    test_a_missing_parent_ends_the_walk);

    return g_test_run();
}
