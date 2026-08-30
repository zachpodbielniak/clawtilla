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

    return g_test_run();
}
