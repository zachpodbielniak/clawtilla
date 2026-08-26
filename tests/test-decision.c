/*
 * test-decision.c - The decision inbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A decision is a choice an agent needs a human to make, posted without
 * stalling the agent.  What makes that honest rather than a queue of
 * stalled work with a nicer name is the *default*: the agent states what
 * it will do anyway, so the operator's answer redirects rather than
 * unblocks.  Everything worth testing here is about that.
 */

#include <clawtilla.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#define NOW G_GINT64_CONSTANT(1787659200)
#define HOUR G_GINT64_CONSTANT(3600)
#define DAY  (24 * HOUR)

static void
test_a_decision_carries_its_default(void)
{
    g_autoptr(ClawtDecision) decision =
        clawt_decision_new(NULL, "chief", "Take the outage now?");
    const gchar *options[] = { "now", "after the release", NULL };

    clawt_decision_set_options(decision, options);
    clawt_decision_set_default(decision, "after the release",
                               "nothing is broken and the window is short");

    g_assert_cmpstr(clawt_decision_get_default(decision), ==,
                    "after the release");
    g_assert_nonnull(strstr(clawt_decision_get_default_reason(decision),
                            "nothing is broken"));
    g_assert_cmpstr(clawt_decision_get_options(decision)[0], ==, "now");

    /* An id is generated when none is given, so a caller cannot collide. */
    g_assert_nonnull(clawt_decision_get_id(decision));
    g_assert_cmpuint(strlen(clawt_decision_get_id(decision)), >, 8);
}

/*
 * Urgency is about the deadline, and an agent that stated none cannot
 * have its silence read as pressure -- otherwise every lazily-filed item
 * sorts to the top and the ordering stops meaning anything within a day.
 */
static void
test_urgency_needs_a_stated_deadline(void)
{
    g_autoptr(ClawtDecision) vague =
        clawt_decision_new(NULL, "chief", "Which way?");
    g_autoptr(ClawtDecision) soon =
        clawt_decision_new(NULL, "chief", "Which way?");
    g_autoptr(ClawtDecision) later =
        clawt_decision_new(NULL, "chief", "Which way?");

    clawt_decision_set_reversible_until(soon, NOW + HOUR);
    clawt_decision_set_reversible_until(later, NOW + (5 * DAY));

    g_assert_false(clawt_decision_is_urgent(vague, NOW));
    g_assert_true(clawt_decision_is_urgent(soon, NOW));
    g_assert_false(clawt_decision_is_urgent(later, NOW));
}

/*
 * The boundary, from both sides.  A day is the horizon an operator can
 * act inside without rearranging anything; getting it wrong in either
 * direction makes the ordering useless rather than merely imprecise.
 */
static void
test_the_urgency_boundary(void)
{
    g_autoptr(ClawtDecision) decision =
        clawt_decision_new(NULL, "chief", "Which way?");

    clawt_decision_set_reversible_until(decision, NOW + DAY);
    g_assert_true(clawt_decision_is_urgent(decision, NOW));

    clawt_decision_set_reversible_until(decision, NOW + DAY + 1);
    g_assert_false(clawt_decision_is_urgent(decision, NOW));
}

/*
 * Past the deadline it is not urgent, it is over -- and saying so is the
 * difference between an inbox and a list of regrets.  An operator who
 * answers a stale item should be told the work already went the other
 * way, not thanked.
 */
static void
test_a_passed_deadline_means_the_default_happened(void)
{
    g_autoptr(ClawtDecision) decision =
        clawt_decision_new(NULL, "chief", "Which way?");

    clawt_decision_set_reversible_until(decision, NOW + HOUR);

    g_assert_false(clawt_decision_default_has_taken_effect(decision, NOW));
    g_assert_true(clawt_decision_default_has_taken_effect(decision,
                                                          NOW + HOUR));
    g_assert_false(clawt_decision_is_urgent(decision, NOW + HOUR));

    /* And one nobody put a deadline on is never overtaken. */
    {
        g_autoptr(ClawtDecision) vague =
            clawt_decision_new(NULL, "chief", "Which way?");

        g_assert_false(clawt_decision_default_has_taken_effect(vague,
                                                               G_MAXINT64));
    }
}

/*
 * A decision somebody answered is theirs whatever the clock says.  Only
 * an open one can be overtaken by its own default -- otherwise an
 * answered item would start reporting that it had defaulted, which is
 * the one distinction the states exist to keep.
 */
static void
test_an_answered_decision_is_not_overtaken(void)
{
    g_autoptr(ClawtDecision) decision =
        clawt_decision_new(NULL, "chief", "Which way?");

    clawt_decision_set_reversible_until(decision, NOW + HOUR);
    clawt_decision_answer(decision, "left", NOW);

    g_assert_cmpint(clawt_decision_get_state(decision), ==,
                    CLAWT_DECISION_ANSWERED);
    g_assert_false(clawt_decision_default_has_taken_effect(
        decision, NOW + (10 * DAY)));
}

/* ── The store ─────────────────────────────────────────────────────── */

static ClawtDecisionStore *
store_new(gchar **dir_out)
{
    g_autoptr(GError) error = NULL;
    gchar *dir = g_dir_make_tmp("clawt-decision-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "decisions.db", NULL);
    ClawtDecisionStore *store = clawt_decision_store_new(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(store);

    *dir_out = dir;
    return store;
}

static void
test_a_decision_survives_the_store(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDecisionStore) store = store_new(&dir);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    const gchar *options[] = { "yes", "no, with caveats", NULL };

    {
        g_autoptr(ClawtDecision) decision =
            clawt_decision_new(NULL, "chief", "Mark it draft?");

        clawt_decision_set_options(decision, options);
        clawt_decision_set_default(decision, "yes", "it is not ready");
        clawt_decision_set_reversible_until(decision, NOW + HOUR);
        clawt_decision_set_task(decision, "task-7");

        id = clawt_decision_store_post(store, decision, &error);
        g_assert_no_error(error);
        g_assert_nonnull(id);
    }

    {
        g_autoptr(ClawtDecision) back = clawt_decision_store_get(store, id);

        g_assert_nonnull(back);
        g_assert_cmpstr(clawt_decision_get_agent(back), ==, "chief");
        g_assert_cmpstr(clawt_decision_get_question(back), ==,
                        "Mark it draft?");
        g_assert_cmpstr(clawt_decision_get_default(back), ==, "yes");
        g_assert_cmpstr(clawt_decision_get_task(back), ==, "task-7");
        g_assert_cmpint(clawt_decision_get_reversible_until(back), ==,
                        NOW + HOUR);
        g_assert_cmpint(clawt_decision_get_state(back), ==,
                        CLAWT_DECISION_OPEN);

        /*
         * An option with a comma in it survives, because options are
         * joined on a newline rather than a comma -- "no, with caveats"
         * is exactly the kind of thing an agent writes and a comma would
         * split it in half on the way back.
         */
        g_assert_cmpstr(clawt_decision_get_options(back)[1], ==,
                        "no, with caveats");
        g_assert_null(clawt_decision_get_options(back)[2]);
    }

    clawt_test_remove_tree(dir);
}

/*
 * The urgent one comes first, and an item that named no deadline sorts
 * after every item that did -- a plain ASC would put the undated ones
 * on top, since 0 is smaller than any real time.
 */
static void
test_the_list_puts_the_deadline_first(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDecisionStore) store = store_new(&dir);
    g_autoptr(GPtrArray) open = NULL;
    guint i;
    struct { const gchar *q; gint64 until; } items[] = {
        { "undated",  0 },
        { "next week", NOW + (7 * DAY) },
        { "this hour", NOW + HOUR }
    };

    for (i = 0; i < G_N_ELEMENTS(items); i++) {
        g_autoptr(ClawtDecision) decision =
            clawt_decision_new(NULL, "chief", items[i].q);
        g_autofree gchar *id = NULL;

        clawt_decision_set_reversible_until(decision, items[i].until);
        id = clawt_decision_store_post(store, decision, NULL);
        g_assert_nonnull(id);
    }

    open = clawt_decision_store_list(store, TRUE);

    g_assert_cmpuint(open->len, ==, 3);
    g_assert_cmpstr(clawt_decision_get_question(
        g_ptr_array_index(open, 0)), ==, "this hour");
    g_assert_cmpstr(clawt_decision_get_question(
        g_ptr_array_index(open, 1)), ==, "next week");
    g_assert_cmpstr(clawt_decision_get_question(
        g_ptr_array_index(open, 2)), ==, "undated");

    clawt_test_remove_tree(dir);
}

static void
test_answering_settles_it_once(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDecisionStore) store = store_new(&dir);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtDecision) answered = NULL;

    {
        g_autoptr(ClawtDecision) decision =
            clawt_decision_new(NULL, "chief", "Which way?");

        id = clawt_decision_store_post(store, decision, NULL);
    }

    g_assert_cmpuint(clawt_decision_store_count_open(store), ==, 1);

    answered = clawt_decision_store_answer(store, id, "neither, do X",
                                           &error);
    g_assert_no_error(error);
    g_assert_nonnull(answered);

    /*
     * Free text, not an index into the options.  An operator whose
     * answer is "neither, do X" is giving the most valuable answer there
     * is, and an inbox that could not carry it would push them back into
     * the conversation this exists to keep them out of.
     */
    g_assert_cmpstr(clawt_decision_get_answer(answered), ==,
                    "neither, do X");

    /* The agent is on it, so the caller can route without a second look. */
    g_assert_cmpstr(clawt_decision_get_agent(answered), ==, "chief");

    g_assert_cmpuint(clawt_decision_store_count_open(store), ==, 0);

    /*
     * And a second answer is refused rather than overwriting.  The first
     * has already reached the agent and may have changed what it did, so
     * a silent overwrite would be a change of mind nothing downstream
     * ever hears about.
     */
    {
        g_autoptr(GError) again = NULL;
        g_autoptr(ClawtDecision) twice =
            clawt_decision_store_answer(store, id, "left", &again);

        g_assert_null(twice);
        g_assert_nonnull(again);
        g_assert_nonnull(strstr(again->message, "already been settled"));
    }

    clawt_test_remove_tree(dir);
}

static void
test_dismissing_and_missing_ids(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDecisionStore) store = store_new(&dir);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    {
        g_autoptr(ClawtDecision) decision =
            clawt_decision_new(NULL, "chief", "Which way?");

        id = clawt_decision_store_post(store, decision, NULL);
    }

    g_assert_true(clawt_decision_store_dismiss(store, id, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(clawt_decision_store_count_open(store), ==, 0);

    {
        g_autoptr(ClawtDecision) back = clawt_decision_store_get(store, id);

        /*
         * Dismissed, not answered.  Both mean the work went a particular
         * way; only one of them means somebody knew, and collapsing them
         * would make the inbox unable to answer the question it exists
         * for.
         */
        g_assert_cmpint(clawt_decision_get_state(back), ==,
                        CLAWT_DECISION_DISMISSED);
        g_assert_null(clawt_decision_get_answer(back));
    }

    {
        g_autoptr(GError) missing = NULL;

        g_assert_false(clawt_decision_store_dismiss(store, "nope", &missing));
        g_assert_nonnull(missing);
        g_assert_null(clawt_decision_store_answer(store, "nope", "x", NULL));
    }

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/decision/carries-its-default",
                    test_a_decision_carries_its_default);
    g_test_add_func("/decision/urgency-needs-a-deadline",
                    test_urgency_needs_a_stated_deadline);
    g_test_add_func("/decision/urgency-boundary",
                    test_the_urgency_boundary);
    g_test_add_func("/decision/passed-deadline-is-the-default",
                    test_a_passed_deadline_means_the_default_happened);
    g_test_add_func("/decision/answered-is-not-overtaken",
                    test_an_answered_decision_is_not_overtaken);

    g_test_add_func("/decision/survives-the-store",
                    test_a_decision_survives_the_store);
    g_test_add_func("/decision/list-puts-the-deadline-first",
                    test_the_list_puts_the_deadline_first);
    g_test_add_func("/decision/answering-settles-it-once",
                    test_answering_settles_it_once);
    g_test_add_func("/decision/dismissing-and-missing-ids",
                    test_dismissing_and_missing_ids);

    return g_test_run();
}
