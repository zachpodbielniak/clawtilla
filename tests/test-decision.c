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
#include <sqlite3.h>
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
        /*
         * And says what did happen, because whoever pressed is looking
         * at a view that was out of date and needs to know which way it
         * went -- not merely that it went.
         */
        g_assert_nonnull(strstr(again->message, "Already settled"));
        g_assert_nonnull(strstr(again->message, "neither, do X"));
        g_assert_nonnull(strstr(again->message, "changed nothing"));
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

/*
 * Deleting forgets the row, in any state, and the open count follows.
 * Two rows, so a DELETE without its WHERE would show as the survivor
 * going missing.
 */
static void
test_deleting_removes_only_that_one(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDecisionStore) store = store_new(&dir);
    g_autofree gchar *gone = NULL;
    g_autofree gchar *kept = NULL;
    g_autofree gchar *answered = NULL;

    {
        g_autoptr(ClawtDecision) a = clawt_decision_new(NULL, "chief", "One?");
        g_autoptr(ClawtDecision) b = clawt_decision_new(NULL, "chief", "Two?");
        g_autoptr(ClawtDecision) c = clawt_decision_new(NULL, "chief", "Three?");

        gone = clawt_decision_store_post(store, a, NULL);
        kept = clawt_decision_store_post(store, b, NULL);
        answered = clawt_decision_store_post(store, c, NULL);
    }
    g_assert_cmpuint(clawt_decision_store_count_open(store), ==, 3);

    g_assert_true(clawt_decision_store_delete(store, gone, NULL));
    g_assert_cmpuint(clawt_decision_store_count_open(store), ==, 2);

    {
        g_autoptr(ClawtDecision) missing = clawt_decision_store_get(store, gone);
        g_autoptr(ClawtDecision) survivor = clawt_decision_store_get(store, kept);

        g_assert_null(missing);
        g_assert_nonnull(survivor);
    }

    /* a settled one goes too: deleting is not limited to the open */
    {
        g_autoptr(ClawtDecision) settled =
            clawt_decision_store_answer(store, answered, "yes", NULL);

        g_assert_nonnull(settled);
    }
    g_assert_true(clawt_decision_store_delete(store, answered, NULL));

    {
        g_autoptr(GPtrArray) all = clawt_decision_store_list(store, FALSE);

        g_assert_cmpuint(all->len, ==, 1);
    }

    /* a missing id is NOT_FOUND, not a quiet success */
    {
        g_autoptr(GError) missing = NULL;

        g_assert_false(clawt_decision_store_delete(store, gone, &missing));
        g_assert_error(missing, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
    }

    clawt_test_remove_tree(dir);
}

/* ── The inline view: the same record, drawn in its room ─────────── */

/*
 * What decision.list puts on the wire comes back as the record, room and
 * all -- one reader for three clients, so none of them can take the
 * answer of a defaulted decision for somebody's choice.
 */
static void
test_a_decision_reads_back_off_the_wire(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(ClawtDecision) back = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(json_parser_load_from_data(parser,
        "{\"id\": \"d1\", \"agent\": \"chief\", \"question\": \"Ship?\","
        " \"options\": [\"now\", \"\", 7, \"later\"],"
        " \"default\": \"later\", \"default_reason\": \"quiet hours\","
        " \"task\": null, \"answer\": \"later\", \"reversible_until\": 10,"
        " \"created_at\": 5, \"state\": 2, \"room\": \"dm:chief:user\","
        " \"settled_via\": null, \"answered_at\": 11}", -1, &error));
    g_assert_no_error(error);

    back = clawt_decision_new_from_object(
        json_node_get_object(json_parser_get_root(parser)));
    g_assert_nonnull(back);
    g_assert_cmpstr(clawt_decision_get_room(back), ==, "dm:chief:user");
    g_assert_cmpint(clawt_decision_get_state(back), ==,
                    CLAWT_DECISION_DEFAULTED);
    g_assert_cmpstr(clawt_decision_get_options(back)[0], ==, "now");
    g_assert_cmpstr(clawt_decision_get_options(back)[1], ==, "later");
    g_assert_null(clawt_decision_get_options(back)[2]);
    g_assert_cmpint(clawt_decision_get_created_at(back), ==, 5);
    g_assert_null(clawt_decision_get_settled_via(back));

    /* no id, no decision: there would be nothing to answer */
    {
        g_autoptr(JsonParser) bare = json_parser_new();

        g_assert_true(json_parser_load_from_data(bare,
            "{\"question\": \"orphan\"}", -1, NULL));
        g_assert_null(clawt_decision_new_from_object(
            json_node_get_object(json_parser_get_root(bare))));
    }
}

/*
 * Settled in the inbox, the inline view says so.
 *
 * The chat holds its own copy of the record; a `decision.settled` from
 * any surface is folded into it, and the copy then reads exactly as the
 * inbox's does.  A replay of the same event changes nothing -- the
 * daemon never settles one twice, so a second event is not news -- and
 * an event about a different decision changes nothing either.
 */
static void
test_settling_in_the_inbox_updates_the_inline_view(void)
{
    g_autoptr(ClawtDecision) held =
        clawt_decision_new("d1", "chief", "Take the outage now?");
    g_autoptr(ClawtEvent) other = clawt_event_new("decision.settled", "chief");
    g_autoptr(ClawtEvent) event = clawt_event_new("decision.settled", "chief");
    g_autofree gchar *outcome = NULL;
    const gchar *options[] = { "now", "after the release", NULL };

    clawt_decision_set_options(held, options);
    clawt_decision_set_default(held, "after the release", NULL);
    clawt_decision_set_room(held, "dm:chief:user");

    clawt_event_set_detail(other, "decision", "d2");
    clawt_event_set_detail(other, "how", "answered");
    g_assert_false(clawt_decision_apply_event(held, other));
    g_assert_cmpint(clawt_decision_get_state(held), ==, CLAWT_DECISION_OPEN);

    clawt_event_set_detail(event, "decision", "d1");
    clawt_event_set_detail(event, "how", "answered");
    clawt_event_set_detail(event, "answer", "now");
    clawt_event_set_detail(event, "via", CLAWT_DECISION_VIA_INBOX);
    clawt_event_set_detail(event, "room", "dm:chief:user");

    g_assert_cmpstr(clawt_decision_event_room(event), ==, "dm:chief:user");
    g_assert_true(clawt_decision_apply_event(held, event));
    g_assert_cmpint(clawt_decision_get_state(held), ==,
                    CLAWT_DECISION_ANSWERED);
    g_assert_cmpstr(clawt_decision_get_answer(held), ==, "now");

    outcome = clawt_decision_outcome_text(held);
    g_assert_nonnull(strstr(outcome, "You answered"));
    g_assert_nonnull(strstr(outcome, "now"));
    g_assert_nonnull(strstr(outcome, "in the inbox"));

    g_assert_false(clawt_decision_apply_event(held, event));

    /* deleted is not a state: the card goes, the record is not rewritten */
    {
        g_autoptr(ClawtDecision) open =
            clawt_decision_new("d3", "chief", "Which?");

        g_assert_false(clawt_decision_apply_settled(open, "deleted", NULL,
                                                    NULL, NOW));
        g_assert_cmpint(clawt_decision_get_state(open), ==,
                        CLAWT_DECISION_OPEN);
    }
}

/*
 * Every way of settling has a sentence that says who and how, and the
 * clock is never "you".
 */
static void
test_each_outcome_says_who_and_how(void)
{
    g_autoptr(ClawtDecision) chat = clawt_decision_new("a", "chief", "Q?");
    g_autoptr(ClawtDecision) clock = clawt_decision_new("b", "chief", "Q?");
    g_autoptr(ClawtDecision) passed = clawt_decision_new("c", "chief", "Q?");
    g_autoptr(ClawtDecision) unknown = clawt_decision_new("d", "chief", "Q?");
    g_autofree gchar *said_chat = NULL;
    g_autofree gchar *said_clock = NULL;
    g_autofree gchar *said_passed = NULL;
    g_autofree gchar *said_unknown = NULL;

    g_assert_null(clawt_decision_outcome_text(chat));

    clawt_decision_set_default(clock, "wait", NULL);
    clawt_decision_set_default(passed, "wait", NULL);

    g_assert_true(clawt_decision_apply_settled(chat, "answered", "go",
                                               CLAWT_DECISION_VIA_CHAT, NOW));
    g_assert_true(clawt_decision_apply_settled(clock, "defaulted", "wait",
                                               NULL, NOW));
    g_assert_true(clawt_decision_apply_settled(passed, "dismissed", NULL,
                                               CLAWT_DECISION_VIA_INBOX, NOW));
    g_assert_true(clawt_decision_apply_settled(unknown, "answered", "go",
                                               NULL, NOW));

    said_chat = clawt_decision_outcome_text(chat);
    said_clock = clawt_decision_outcome_text(clock);
    said_passed = clawt_decision_outcome_text(passed);
    said_unknown = clawt_decision_outcome_text(unknown);

    g_assert_nonnull(strstr(said_chat, "in the chat"));
    g_assert_nonnull(strstr(said_clock, "Nobody answered in time"));
    g_assert_null(strstr(said_clock, "You"));
    g_assert_nonnull(strstr(said_passed, "Dismissed in the inbox"));
    g_assert_nonnull(strstr(said_passed, "wait"));
    /* an answer from an unnamed surface is not claimed for the reader */
    g_assert_null(strstr(said_unknown, "You"));
}

/*
 * A press on a decision the clock already settled -- the stale press --
 * is told that, in words, and told the press did nothing.
 */
static void
test_a_stale_press_is_told_what_happened(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDecisionStore) store = store_new(&dir);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    {
        g_autoptr(ClawtDecision) decision =
            clawt_decision_new(NULL, "chief", "Restart the database?");
        g_autoptr(ClawtDecision) settled = NULL;

        clawt_decision_set_default(decision, "wait for the window", NULL);
        id = clawt_decision_store_post(store, decision, NULL);
        settled = clawt_decision_store_settle_default(store, id, NULL);
        g_assert_nonnull(settled);
    }

    {
        g_autoptr(ClawtDecision) late = clawt_decision_store_answer_via(
            store, id, "restart now", CLAWT_DECISION_VIA_CHAT, &error);

        g_assert_null(late);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
        g_assert_nonnull(strstr(error->message, "Already settled"));
        g_assert_nonnull(strstr(error->message, "Nobody answered in time"));
        g_assert_nonnull(strstr(error->message, "wait for the window"));
        g_assert_nonnull(strstr(error->message, "changed nothing"));
        g_assert_nonnull(strstr(error->message, "chief"));
    }

    clawt_test_remove_tree(dir);
}

/*
 * The room and the surface survive the store, and a room's list is that
 * room's in the order asked, settled ones included -- a settled question
 * stays in the transcript where it was asked.
 */
static void
test_a_rooms_decisions_come_back_in_order(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDecisionStore) store = store_new(&dir);
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;
    g_autofree gchar *elsewhere = NULL;
    g_autofree gchar *nowhere = NULL;

    {
        g_autoptr(ClawtDecision) a = clawt_decision_new(NULL, "chief", "A?");
        g_autoptr(ClawtDecision) b = clawt_decision_new(NULL, "chief", "B?");
        g_autoptr(ClawtDecision) c = clawt_decision_new(NULL, "chief", "C?");
        g_autoptr(ClawtDecision) d = clawt_decision_new(NULL, "chief", "D?");

        clawt_decision_set_room(a, "room-1");
        clawt_decision_set_created_at(a, NOW);
        clawt_decision_set_room(b, "room-1");
        clawt_decision_set_created_at(b, NOW + 1);
        clawt_decision_set_room(c, "room-2");
        clawt_decision_set_room(d, "");

        second = clawt_decision_store_post(store, b, NULL);
        first = clawt_decision_store_post(store, a, NULL);
        elsewhere = clawt_decision_store_post(store, c, NULL);
        nowhere = clawt_decision_store_post(store, d, NULL);
    }

    {
        g_autoptr(ClawtDecision) answered = clawt_decision_store_answer_via(
            store, first, "yes", CLAWT_DECISION_VIA_CHAT, NULL);

        g_assert_nonnull(answered);
        g_assert_cmpstr(clawt_decision_get_settled_via(answered), ==,
                        CLAWT_DECISION_VIA_CHAT);
    }

    {
        g_autoptr(GPtrArray) room = clawt_decision_store_list_room(store,
                                                                   "room-1");
        g_autoptr(GPtrArray) none = clawt_decision_store_list_room(store, "");
        g_autoptr(ClawtDecision) back = clawt_decision_store_get(store, first);

        g_assert_cmpuint(room->len, ==, 2);
        g_assert_cmpstr(clawt_decision_get_id(g_ptr_array_index(room, 0)), ==,
                        first);
        g_assert_cmpstr(clawt_decision_get_id(g_ptr_array_index(room, 1)), ==,
                        second);
        g_assert_cmpint(clawt_decision_get_state(g_ptr_array_index(room, 0)),
                        ==, CLAWT_DECISION_ANSWERED);

        /* an unplaced decision belongs to no room, not to every room */
        g_assert_cmpuint(none->len, ==, 0);

        g_assert_cmpstr(clawt_decision_get_room(back), ==, "room-1");
        g_assert_cmpstr(clawt_decision_get_settled_via(back), ==, "chat");
    }

    clawt_test_remove_tree(dir);
}

/*
 * A store written before the room existed is upgraded where it opens,
 * not left to answer every SELECT with an empty list.
 */
static void
test_an_old_store_gains_the_room(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-decision-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "decisions.db", NULL);
    g_autoptr(GError) error = NULL;
    sqlite3 *db = NULL;

    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db,
        "CREATE TABLE decisions (id TEXT PRIMARY KEY, agent TEXT NOT NULL,"
        " question TEXT NOT NULL, options TEXT, default_option TEXT,"
        " default_reason TEXT, task TEXT, answer TEXT,"
        " reversible_until INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL,"
        " answered_at INTEGER NOT NULL DEFAULT 0,"
        " state INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO decisions (id, agent, question, created_at)"
        " VALUES ('old', 'chief', 'From before?', 1);",
        NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);

    {
        g_autoptr(ClawtDecisionStore) store =
            clawt_decision_store_new(path, &error);
        g_autoptr(ClawtDecision) old = NULL;
        g_autoptr(GPtrArray) open = NULL;

        g_assert_no_error(error);
        g_assert_nonnull(store);

        old = clawt_decision_store_get(store, "old");
        g_assert_nonnull(old);
        g_assert_null(clawt_decision_get_room(old));

        open = clawt_decision_store_list(store, TRUE);
        g_assert_cmpuint(open->len, ==, 1);
    }

    /* and opening it again does not try to add the columns twice */
    {
        g_autoptr(ClawtDecisionStore) again =
            clawt_decision_store_new(path, &error);

        g_assert_no_error(error);
        g_assert_nonnull(again);
    }

    clawt_test_remove_tree(dir);
}

/*
 * The default line says what happens and until when, as a clock time --
 * a relative "in 3h" in a transcript is wrong from the moment it is
 * drawn.  Placing goes strictly before the message.
 */
static void
test_the_default_line_and_placement(void)
{
    g_autoptr(ClawtDecision) decision =
        clawt_decision_new("d", "chief", "Ship?");
    g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
    g_autofree gchar *line = NULL;

    g_assert_null(clawt_decision_default_text(decision, utc));

    clawt_decision_set_default(decision, "after the release", "quiet hours");
    clawt_decision_set_reversible_until(decision, NOW);
    line = clawt_decision_default_text(decision, utc);

    g_assert_nonnull(strstr(line, "If nobody answers"));
    g_assert_nonnull(strstr(line, "after the release"));
    g_assert_nonnull(strstr(line, "quiet hours"));
    g_assert_nonnull(strstr(line, "Reversible until"));
    g_assert_nonnull(strstr(line, "12:00"));

    clawt_decision_set_created_at(decision, 100);
    g_assert_false(clawt_decision_precedes(decision, 100));
    g_assert_true(clawt_decision_precedes(decision, 101));

    g_assert_true(clawt_decision_via_is_known(NULL));
    g_assert_true(clawt_decision_via_is_known(CLAWT_DECISION_VIA_CHAT));
    g_assert_false(clawt_decision_via_is_known("telepathy"));
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
    g_test_add_func("/decision/deleting-removes-only-that-one",
                    test_deleting_removes_only_that_one);
    g_test_add_func("/decision/dismissing-and-missing-ids",
                    test_dismissing_and_missing_ids);

    g_test_add_func("/decision/inline/reads-back-off-the-wire",
                    test_a_decision_reads_back_off_the_wire);
    g_test_add_func("/decision/inline/settled-in-the-inbox",
                    test_settling_in_the_inbox_updates_the_inline_view);
    g_test_add_func("/decision/inline/each-outcome-says-who",
                    test_each_outcome_says_who_and_how);
    g_test_add_func("/decision/inline/stale-press-is-told",
                    test_a_stale_press_is_told_what_happened);
    g_test_add_func("/decision/inline/a-rooms-decisions-in-order",
                    test_a_rooms_decisions_come_back_in_order);
    g_test_add_func("/decision/inline/old-store-gains-the-room",
                    test_an_old_store_gains_the_room);
    g_test_add_func("/decision/inline/default-line-and-placement",
                    test_the_default_line_and_placement);

    return g_test_run();
}
