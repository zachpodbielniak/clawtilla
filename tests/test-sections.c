/*
 * test-sections.c - The pages, and the groups both clients draw them in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The GTK client's switcher held eleven tabs and was clipped below about
 * 1500 logical pixels -- no ellipsis, no overflow menu, nothing logged --
 * so which pages a person could reach depended on the monitor.  Six
 * sections replaced them, and the grouping lives in the library because
 * both clients draw the row: a page filed under Work in the window and
 * under Library in a browser is a divergence with nothing to announce
 * it.
 *
 * These assert the properties the clients actually depend on rather than
 * the table's current contents, so that moving a page between sections
 * is an ordinary edit and dropping one on the floor is not.
 *
 * What they cannot see, stated rather than implied: every count here is
 * derived from the table, so a value added to #ClawtPage and *left out*
 * of the table is consistent with itself and invisible from in here.
 * The compiler catches that one -- build_page() in the GTK client and
 * clawt_web_view_body() in the web client are each a `switch` over
 * #ClawtPage with no `default:`, so a new value is two -Wswitch
 * warnings -- and clawt_gtk_set_page_badge() carries a bounds check as
 * the backstop, since what it would otherwise do is write past the end
 * of an array sized by clawt_page_count().
 */

#include "clawtilla.h"

#include <string.h>

/*
 * Every page is in exactly one section, and no page is stranded.
 *
 * The count over sections and the flat count are two different walks of
 * the same table.  A page added with a section that has no tab, or
 * counted twice, changes one and not the other -- and the symptom
 * without this is a page that simply cannot be reached, which nothing
 * else in the suite would notice.
 */
static void
test_every_page_is_in_exactly_one_section(void)
{
    guint reached = 0;
    guint i;

    for (i = 0; i < clawt_section_count(); i++) {
        ClawtSection section = clawt_section_nth(i);
        guint n = clawt_section_page_count(section);
        guint j;

        g_assert_cmpuint(n, >, 0);

        for (j = 0; j < n; j++) {
            ClawtPage page = clawt_section_page_nth(section, j);

            g_assert_cmpint(clawt_page_section(page), ==, section);
            reached++;
        }
    }

    g_assert_cmpuint(reached, ==, clawt_page_count());
}

/*
 * The GTK client sizes three arrays by clawt_page_count() and indexes
 * them by the #ClawtPage it is holding -- the badge on a section's tab
 * is the sum over its pages, which needs somewhere to keep each one's
 * number.  Same for the per-section arrays.
 *
 * That is only safe while every value is below its count, so it is
 * asserted here rather than assumed there: a page appended to the
 * enumeration but left out of the table would make the count too small
 * and the write out of bounds.
 */
static void
test_every_value_can_index_an_array_of_its_own_count(void)
{
    guint i;

    for (i = 0; i < clawt_section_count(); i++) {
        ClawtSection section = clawt_section_nth(i);
        guint n = clawt_section_page_count(section);
        guint j;

        g_assert_cmpuint((guint)section, <, clawt_section_count());

        for (j = 0; j < n; j++)
            g_assert_cmpuint((guint)clawt_section_page_nth(section, j), <,
                             clawt_page_count());
    }
}

/*
 * A nickname is a URL and a widget name at once, so two pages sharing
 * one is a link that lands on whichever the lookup meets first.  A page
 * sharing a nickname with a section is fine -- they are looked up in
 * different stacks and different halves of a path -- and Chat, Agent and
 * Computer deliberately do.
 */
static void
test_nicknames_are_unique_within_their_kind(void)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    guint i;

    for (i = 0; i < clawt_section_count(); i++) {
        const gchar *nick = clawt_section_nth_nick(i);

        g_assert_nonnull(nick);
        g_assert_cmpuint(strlen(nick), >, 0);
        g_assert_false(g_hash_table_contains(seen, nick));
        g_hash_table_add(seen, (gpointer)nick);
    }

    g_hash_table_remove_all(seen);

    for (i = 0; i < clawt_section_count(); i++) {
        ClawtSection section = clawt_section_nth(i);
        guint j;

        for (j = 0; j < clawt_section_page_count(section); j++) {
            const gchar *nick =
                clawt_page_nick(clawt_section_page_nth(section, j));

            g_assert_nonnull(nick);
            g_assert_cmpuint(strlen(nick), >, 0);
            g_assert_false(g_hash_table_contains(seen, nick));
            g_hash_table_add(seen, (gpointer)nick);
        }
    }
}

/*
 * And a nickname survives the round trip, in both directions.
 *
 * This is what a pasted link rests on: the web client puts the nickname
 * in the path and reads it back out, and the GTK client puts it on a
 * stack child and reads it back to answer "which page is up".
 */
static void
test_a_nickname_survives_the_round_trip(void)
{
    guint i;

    for (i = 0; i < clawt_section_count(); i++) {
        ClawtSection section = clawt_section_nth(i);
        guint j;

        g_assert_cmpint(clawt_section_from_nick(clawt_section_nick(section)),
                        ==, section);
        g_assert_cmpstr(clawt_section_nick(section), ==,
                        clawt_section_nth_nick(i));
        g_assert_cmpstr(clawt_section_label(section), ==,
                        clawt_section_nth_label(i));

        for (j = 0; j < clawt_section_page_count(section); j++) {
            ClawtPage page = clawt_section_page_nth(section, j);

            g_assert_cmpint(clawt_page_from_nick(clawt_page_nick(page)), ==,
                            page);
        }
    }
}

/*
 * Every tab has something written on it.
 *
 * An empty label draws a tab of pure padding: clickable, indistinguish-
 * able from its neighbours, and reported by nothing.
 */
static void
test_every_tab_has_a_label(void)
{
    guint i;

    for (i = 0; i < clawt_section_count(); i++) {
        ClawtSection section = clawt_section_nth(i);
        guint j;

        g_assert_cmpuint(strlen(clawt_section_label(section)), >, 0);

        for (j = 0; j < clawt_section_page_count(section); j++) {
            ClawtPage page = clawt_section_page_nth(section, j);

            g_assert_cmpuint(strlen(clawt_page_label(page)), >, 0);
        }
    }
}

/*
 * A section's own tab lands on its first page.
 *
 * Both clients rely on this: the web topbar links a section tab at it,
 * and the GTK client answers "which page is up" with it for a section
 * that holds one page and therefore has no inner stack.
 */
static void
test_a_section_lands_on_its_first_page(void)
{
    guint i;

    for (i = 0; i < clawt_section_count(); i++) {
        ClawtSection section = clawt_section_nth(i);

        g_assert_cmpint(clawt_section_default_page(section), ==,
                        clawt_section_page_nth(section, 0));
        g_assert_cmpint(clawt_page_section(clawt_section_default_page(section)),
                        ==, section);
    }
}

/*
 * The whole point of the change: the row is short enough to draw.
 *
 * Not a style preference -- eleven tabs did not fit and were silently
 * clipped.  A ceiling here is what stops the next page from being added
 * as a twelfth top-level tab and quietly restoring the bug, since
 * nothing else in the suite can see how wide a header bar is.
 */
static void
test_the_switcher_row_stays_short(void)
{
    g_assert_cmpuint(clawt_section_count(), <=, 6);
    g_assert_cmpuint(clawt_page_count(), >, clawt_section_count());
}

/*
 * An unknown name is the conversation, never an error.
 *
 * These arrive from a path somebody typed and from a stack child name,
 * and Chat is the page every agent has.  Asserted because both clients
 * hand this untrusted input directly.
 */
static void
test_an_unknown_name_falls_back(void)
{
    g_assert_cmpint(clawt_page_from_nick("nonsense"), ==, CLAWT_PAGE_CHAT);
    g_assert_cmpint(clawt_page_from_nick(""), ==, CLAWT_PAGE_CHAT);
    g_assert_cmpint(clawt_page_from_nick(NULL), ==, CLAWT_PAGE_CHAT);

    g_assert_cmpint(clawt_section_from_nick("nonsense"), ==,
                    CLAWT_SECTION_CHAT);
    g_assert_cmpint(clawt_section_from_nick(NULL), ==, CLAWT_SECTION_CHAT);

    /*
     * And an index past the end, which a client reaches by walking a
     * count it read before something changed underneath it.
     */
    g_assert_nonnull(clawt_section_nth_nick(clawt_section_count()));
    g_assert_nonnull(clawt_section_nth_label(clawt_section_count()));
    g_assert_cmpint(clawt_section_nth(clawt_section_count()), ==,
                    CLAWT_SECTION_CHAT);
    g_assert_cmpint(clawt_section_page_nth(CLAWT_SECTION_WORK, 99), ==,
                    CLAWT_PAGE_CHAT);
}

/*
 * The grouping the clients were built against.
 *
 * Named values rather than counts, because the properties above hold
 * just as well for a table somebody has shuffled -- and a page moving
 * from Work to Library is a decision, not a refactor.  This is the test
 * that makes it one.
 */
static void
test_the_grouping_is_the_one_intended(void)
{
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_CHAT), ==,
                    CLAWT_SECTION_CHAT);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_AGENT), ==,
                    CLAWT_SECTION_AGENT);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_MAILBOX), ==,
                    CLAWT_SECTION_AGENT);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_COMPUTER), ==,
                    CLAWT_SECTION_COMPUTER);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_ROUTINES), ==,
                    CLAWT_SECTION_AUTOMATION);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_TRIGGERS), ==,
                    CLAWT_SECTION_AUTOMATION);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_TASKS), ==,
                    CLAWT_SECTION_WORK);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_DECISIONS), ==,
                    CLAWT_SECTION_WORK);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_FLOW), ==,
                    CLAWT_SECTION_WORK);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_SKILLS), ==,
                    CLAWT_SECTION_LIBRARY);
    g_assert_cmpint(clawt_page_section(CLAWT_PAGE_MEMORY), ==,
                    CLAWT_SECTION_LIBRARY);

    /*
     * Chat first, because it is what the window opens on, and Chat is a
     * section of its own so that the unread total has a top-level tab to
     * sit on.
     */
    g_assert_cmpint(clawt_section_nth(0), ==, CLAWT_SECTION_CHAT);
    g_assert_cmpuint(clawt_section_page_count(CLAWT_SECTION_CHAT), ==, 1);
}

/*
 * The inspector is "Agent" as a group and "Overview" as one page of it.
 *
 * "Agent > Agent" reads as a mistake, and the sub-tab is the only place
 * the distinction shows -- so a label change that collapses the two
 * would look deliberate.
 */
static void
test_a_page_may_be_named_differently_from_its_section(void)
{
    g_assert_cmpstr(clawt_section_label(CLAWT_SECTION_AGENT), ==, "Agent");
    g_assert_cmpstr(clawt_page_label(CLAWT_PAGE_AGENT), ==, "Overview");
    g_assert_cmpstr(clawt_page_nick(CLAWT_PAGE_AGENT), ==, "agent");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sections/every-page-is-in-exactly-one-section",
                    test_every_page_is_in_exactly_one_section);
    g_test_add_func("/sections/every-value-can-index-its-own-count",
                    test_every_value_can_index_an_array_of_its_own_count);
    g_test_add_func("/sections/nicknames-are-unique-within-their-kind",
                    test_nicknames_are_unique_within_their_kind);
    g_test_add_func("/sections/a-nickname-survives-the-round-trip",
                    test_a_nickname_survives_the_round_trip);
    g_test_add_func("/sections/every-tab-has-a-label",
                    test_every_tab_has_a_label);
    g_test_add_func("/sections/a-section-lands-on-its-first-page",
                    test_a_section_lands_on_its_first_page);
    g_test_add_func("/sections/the-switcher-row-stays-short",
                    test_the_switcher_row_stays_short);
    g_test_add_func("/sections/an-unknown-name-falls-back",
                    test_an_unknown_name_falls_back);
    g_test_add_func("/sections/the-grouping-is-the-one-intended",
                    test_the_grouping_is_the_one_intended);
    g_test_add_func("/sections/a-page-may-be-named-apart-from-its-section",
                    test_a_page_may_be_named_differently_from_its_section);

    return g_test_run();
}
