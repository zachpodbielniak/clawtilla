/*
 * test-mention.c - Who a message names
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * In a room that requires mentions this predicate *is* the delivery
 * list, so every case here is a turn that will or will not be paid for.
 * The four corrections it encodes were all live: `@bobby` woke `bob`,
 * `zach@bob.com` woke `bob`, `alpha-bob` woke `bob`, and `Bob` -- the
 * ordinary way anybody writes a name -- woke nobody and said nothing
 * about it.
 */

#include <clawtilla.h>

#include "clawt-test-util.h"

/* ── The four corrections ────────────────────────────────────────── */

/*
 * `@bobby` is not `bob`.
 *
 * The `@` branch was a plain strstr while the bare branch was word
 * bounded, so an agent called `bob` was woken by every message naming
 * `@bobby` -- and the asymmetry meant the bare form got this right.
 */
static void
test_an_at_form_is_word_bounded(void)
{
    g_assert_false(clawt_mention_names("ask @bobby about it", "bob", NULL));
    g_assert_false(clawt_mention_names("ask bobby about it", "bob", NULL));
    g_assert_true(clawt_mention_names("ask @bob about it", "bob", NULL));

    /* And the same one hop longer, which is the case that found it. */
    g_assert_false(clawt_mention_names("@oryx is on it", "ory", NULL));
    g_assert_true(clawt_mention_names("@oryx is on it", "oryx", NULL));
}

/*
 * The `@` itself has to be at a word boundary.
 *
 * Without this every address in a pasted log wakes somebody, and a log
 * is exactly the sort of thing an agent pastes into a room.
 */
static void
test_an_address_is_not_a_mention(void)
{
    g_assert_false(clawt_mention_names("mail zach@bob.com", "bob", NULL));
    g_assert_false(clawt_mention_names("from user@bob", "bob", NULL));

    g_assert_true(clawt_mention_names("hi @bob", "bob", NULL));
    g_assert_true(clawt_mention_names("(@bob)", "bob", NULL));
    g_assert_true(clawt_mention_names("@bob: go", "bob", NULL));
}

/*
 * The boundary is the same on both sides.
 *
 * `_` and `-` were excluded after the name and not before it, so
 * `alpha-bob` and `my_bob` addressed `bob` while `bob-2` did not.  Both
 * characters are legal in an id, so both directions matter.
 */
static void
test_the_boundary_is_symmetric(void)
{
    g_assert_false(clawt_mention_names("alpha-bob failed", "bob", NULL));
    g_assert_false(clawt_mention_names("my_bob failed", "bob", NULL));
    g_assert_false(clawt_mention_names("bob-2 failed", "bob", NULL));
    g_assert_false(clawt_mention_names("bob_smith failed", "bob", NULL));
    g_assert_false(clawt_mention_names("xbob failed", "bob", NULL));
}

/*
 * Case does not decide who gets a message.
 *
 * Ids are lowercase by construction and a model writes "Bob, can you
 * check" -- so a byte-exact match reached nobody, silently, which is
 * the worst way for this to be wrong.
 */
static void
test_matching_ignores_case(void)
{
    g_assert_true(clawt_mention_names("Bob, can you check", "bob", NULL));
    g_assert_true(clawt_mention_names("@Bob please", "bob", NULL));
    g_assert_true(clawt_mention_names("@BOB please", "bob", NULL));
}

/* ── Boundaries in detail ────────────────────────────────────────── */

static void
test_punctuation_is_a_boundary(void)
{
    g_assert_true(clawt_mention_names("bob, please", "bob", NULL));
    g_assert_true(clawt_mention_names("ask bob.", "bob", NULL));
    g_assert_true(clawt_mention_names("bob!", "bob", NULL));
    g_assert_true(clawt_mention_names("(bob)", "bob", NULL));
    g_assert_true(clawt_mention_names("\"bob\"", "bob", NULL));
    g_assert_true(clawt_mention_names("bob", "bob", NULL));
    g_assert_true(clawt_mention_names("cc bob", "bob", NULL));
}

/*
 * A multi-byte character ends a word too.
 *
 * g_ascii_isalnum() reports FALSE for a UTF-8 lead byte, which has to
 * read as "this is a boundary" rather than as a broken match -- an
 * em-dash or a curly quote after a name is ordinary prose.
 */
static void
test_a_multibyte_neighbour_is_a_boundary(void)
{
    /*
     * Split literals, because a C hex escape is greedy: "\x9cbob" is one
     * number and overflows, which the compiler reports as a warning
     * about a string that looks perfectly ordinary.
     */
    g_assert_true(clawt_mention_names("bob\xe2\x80\x94" " go", "bob", NULL));
    g_assert_true(clawt_mention_names("\xe2\x80\x9c" "bob" "\xe2\x80\x9d",
                                      "bob", NULL));
}

/*
 * A name in a code fence or a URL still matches, on purpose.
 *
 * Asserted rather than left as an absence, because it is a decision:
 * the alternative is a markdown parser on the routing path, and one
 * extra turn is cheaper than a mention that does not fire for a reason
 * nobody can see.  If this test ever needs changing, that is the
 * argument to have.
 */
static void
test_there_is_no_syntax_awareness(void)
{
    g_assert_true(clawt_mention_names("```\nrun bob\n```", "bob", NULL));
    g_assert_true(clawt_mention_names("see https://x.test/bob", "bob",
                                      NULL));
}

/* ── Display names ───────────────────────────────────────────────── */

/*
 * A name that is a word is an address; a name that is a phrase is not.
 *
 * "Oryx" addresses an agent.  "Oryx the Researcher" would match every
 * message containing the word "the", which is most of them.
 */
static void
test_a_display_name_addresses_only_when_it_is_one_word(void)
{
    g_assert_true(clawt_mention_names("@Oryx take this", "oryx-research",
                                      "Oryx"));
    g_assert_true(clawt_mention_names("Oryx take this", "oryx-research",
                                      "Oryx"));

    g_assert_false(clawt_mention_names("the plan is ready", "oryx-research",
                                       "Oryx the Researcher"));

    /* And the id still works whatever the name is. */
    g_assert_true(clawt_mention_names("@oryx-research", "oryx-research",
                                      "Oryx the Researcher"));
}

/* ── Broadcast ───────────────────────────────────────────────────── */

/*
 * `@all` addresses everybody and "all" does not.
 *
 * A bare "all" is an English word: "that's all for now" must not wake
 * five agents, and an agent that wrote it would have no idea why it
 * had.
 */
static void
test_broadcast_needs_the_at_form(void)
{
    g_assert_true(clawt_mention_is_broadcast("@all standup in five"));
    g_assert_true(clawt_mention_is_broadcast("@ALL standup"));
    g_assert_true(clawt_mention_is_broadcast("@All standup"));

    g_assert_false(clawt_mention_is_broadcast("that's all for now"));
    g_assert_false(clawt_mention_is_broadcast("all done"));
    g_assert_false(clawt_mention_is_broadcast("@allocate the budget"));
    g_assert_false(clawt_mention_is_broadcast("mail me@all.test"));

    /* Naming somebody as well is still a broadcast. */
    g_assert_true(clawt_mention_is_broadcast("@all and @bob especially"));
}

/* ── Listing ─────────────────────────────────────────────────────── */

static GPtrArray *
ids(const gchar *first, ...)
{
    GPtrArray *out = g_ptr_array_new();
    const gchar *each = first;
    va_list args;

    va_start(args, first);

    while (each != NULL) {
        g_ptr_array_add(out, (gpointer)each);
        each = va_arg(args, const gchar *);
    }

    va_end(args);

    return out;
}

static void
test_a_listing_names_exactly_who_was_named(void)
{
    g_autoptr(GPtrArray) members =
        ids("alice", "bob", "carol", "dave", "erin", NULL);
    g_autoptr(GPtrArray) named =
        clawt_mention_list("@alice and carol, plus @erin", members);

    g_assert_cmpuint(named->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(named, 0), ==, "alice");
    g_assert_cmpstr(g_ptr_array_index(named, 1), ==, "carol");
    g_assert_cmpstr(g_ptr_array_index(named, 2), ==, "erin");
}

/*
 * Twice is once.
 *
 * A caller counting this array is counting model turns, and naming
 * somebody in two sentences is still one delivery.
 */
static void
test_a_listing_counts_a_name_once(void)
{
    g_autoptr(GPtrArray) members = ids("alice", "bob", NULL);
    g_autoptr(GPtrArray) named =
        clawt_mention_list("@alice, and alice again", members);

    g_assert_cmpuint(named->len, ==, 1);
}

/*
 * Nothing named is an empty list, never everybody.
 *
 * The direction matters: an empty answer that meant "no filter" would
 * turn a remark nobody was addressed by into a turn for the whole room.
 */
static void
test_nothing_named_lists_nobody(void)
{
    g_autoptr(GPtrArray) members = ids("alice", "bob", NULL);
    g_autoptr(GPtrArray) none = clawt_mention_list("morning", members);
    g_autoptr(GPtrArray) empty = clawt_mention_list("", members);
    g_autoptr(GPtrArray) null_body = clawt_mention_list(NULL, members);

    g_assert_cmpuint(none->len, ==, 0);
    g_assert_cmpuint(empty->len, ==, 0);
    g_assert_cmpuint(null_body->len, ==, 0);

    g_assert_false(clawt_mention_names(NULL, "alice", NULL));
    g_assert_false(clawt_mention_names("", "alice", NULL));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mention/at-form-is-word-bounded",
                    test_an_at_form_is_word_bounded);
    g_test_add_func("/mention/an-address-is-not-a-mention",
                    test_an_address_is_not_a_mention);
    g_test_add_func("/mention/boundary-is-symmetric",
                    test_the_boundary_is_symmetric);
    g_test_add_func("/mention/ignores-case", test_matching_ignores_case);
    g_test_add_func("/mention/punctuation-is-a-boundary",
                    test_punctuation_is_a_boundary);
    g_test_add_func("/mention/multibyte-neighbour-is-a-boundary",
                    test_a_multibyte_neighbour_is_a_boundary);
    g_test_add_func("/mention/no-syntax-awareness",
                    test_there_is_no_syntax_awareness);
    g_test_add_func("/mention/display-name-must-be-one-word",
                    test_a_display_name_addresses_only_when_it_is_one_word);
    g_test_add_func("/mention/broadcast-needs-the-at-form",
                    test_broadcast_needs_the_at_form);
    g_test_add_func("/mention/listing-names-who-was-named",
                    test_a_listing_names_exactly_who_was_named);
    g_test_add_func("/mention/listing-counts-a-name-once",
                    test_a_listing_counts_a_name_once);
    g_test_add_func("/mention/nothing-named-lists-nobody",
                    test_nothing_named_lists_nobody);

    return g_test_run();
}
