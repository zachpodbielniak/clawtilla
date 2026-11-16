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
    g_assert_false(clawt_mention_names("mail bob@example.com", "bob",
                                       NULL));

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
 * The `@` form ignores case; a bare word does not.
 *
 * `@name` is the deliberate address -- it is what the delivery preamble
 * tells every member to write -- so somebody typing `@Bob` plainly
 * meant `bob`, and failing on a capital would reach nobody and say
 * nothing about it.
 *
 * A bare name is not an address, it is a word that happens to be spelled
 * like an id.  Folding its case charged an agent called `writer` a whole
 * model turn for "Writer's block on section 2", and one called
 * `research` for any sentence opening with "Research".  A missed bare
 * name costs a person one `@`; a false one costs a turn nobody asked
 * for.
 */
static void
test_the_at_form_ignores_case_and_a_bare_word_does_not(void)
{
    g_assert_true(clawt_mention_names("@Bob please", "bob", NULL));
    g_assert_true(clawt_mention_names("@BOB please", "bob", NULL));
    g_assert_true(clawt_mention_names("bob, can you check", "bob", NULL));

    g_assert_false(clawt_mention_names("Bob, can you check", "bob", NULL));
    g_assert_false(clawt_mention_names("Writer's block on section 2",
                                       "writer", NULL));
    g_assert_false(clawt_mention_names("Research says otherwise",
                                       "research", NULL));
}

/*
 * A letter is a letter whatever it is encoded as.
 *
 * g_ascii_isalnum() answers FALSE for every byte of a multi-byte
 * character, so a byte test read the second byte of `ñ` as a word
 * boundary and "hasta mañana" addressed an agent called `ana`.
 */
static void
test_a_multibyte_letter_is_not_a_boundary(void)
{
    g_assert_false(clawt_mention_names("hasta ma\xc3\xb1" "ana", "ana",
                                       NULL));
    g_assert_false(clawt_mention_names("\xc3\x9c" "bob", "bob", NULL));
    g_assert_false(clawt_mention_names("caf\xc3\xa9" "bob", "bob", NULL));

    /* And a real boundary next to one still matches. */
    g_assert_true(clawt_mention_names("ma\xc3\xb1" "ana, ana here", "ana",
                                      NULL));
}

/*
 * The `@` rule is symmetric.
 *
 * `@` was excluded before a name and not after it, so the `@` form
 * correctly refused `bob@example.com` and then the bare form took the
 * same `bob` from the other end.
 */
static void
test_an_address_is_refused_from_both_ends(void)
{
    g_assert_false(clawt_mention_names("mail bob@example.com", "bob",
                                       NULL));
    g_assert_false(clawt_mention_names("mail zach@bob.com", "bob", NULL));
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

/* ── Completing one as it is typed ───────────────────────────────── */

/*
 * The `@` alone is a real answer, and it is the moment a completion is
 * most worth offering: nothing has been narrowed yet, so every member
 * is still a candidate.
 *
 * It must not be confused with "not typing a mention".  Returning ""
 * for both would pop a member list open on every keystroke.
 */
static void
test_a_bare_at_is_an_empty_prefix_not_nothing(void)
{
    g_autofree gchar *just_at = clawt_mention_prefix_at("hello @", 7);
    g_autofree gchar *partial = clawt_mention_prefix_at("hello @al", 9);
    g_autofree gchar *nothing = clawt_mention_prefix_at("hello", 5);

    g_assert_nonnull(just_at);
    g_assert_cmpstr(just_at, ==, "");

    g_assert_cmpstr(partial, ==, "al");

    g_assert_null(nothing);
}

/*
 * A space between the `@` and the cursor ends it: a mention is one
 * word, so what is being typed is no longer a name.
 */
static void
test_a_word_break_ends_the_prefix(void)
{
    g_autofree gchar *after_space = clawt_mention_prefix_at("@al ready", 9);
    g_autofree gchar *after_newline = clawt_mention_prefix_at("@al\nnext", 8);
    g_autofree gchar *after_comma = clawt_mention_prefix_at("@al, and", 8);

    g_assert_null(after_space);
    g_assert_null(after_newline);
    g_assert_null(after_comma);
}

/*
 * And the same boundary rule delivery applies, so typing an address
 * does not open a member list halfway through.
 */
static void
test_an_address_does_not_open_a_completion(void)
{
    g_autofree gchar *in_address = clawt_mention_prefix_at("zach@bo", 7);
    g_autofree gchar *real = clawt_mention_prefix_at("cc @bo", 6);

    g_assert_null(in_address);
    g_assert_cmpstr(real, ==, "bo");
}

/*
 * The cursor is where the completion is, not the end of the line.
 * Somebody who went back to fix a name mid-sentence is still typing it.
 */
static void
test_the_prefix_is_taken_at_the_cursor(void)
{
    g_autofree gchar *mid = clawt_mention_prefix_at("@ali and more", 4);

    g_assert_cmpstr(mid, ==, "ali");
}

static void
test_candidates_narrow_as_you_type(void)
{
    g_autoptr(GPtrArray) members =
        ids("alice", "alfred", "bob", "carol", NULL);
    g_autoptr(GPtrArray) all = clawt_mention_candidates("", members);
    g_autoptr(GPtrArray) al = clawt_mention_candidates("al", members);
    g_autoptr(GPtrArray) ali = clawt_mention_candidates("ali", members);
    g_autoptr(GPtrArray) none = clawt_mention_candidates("zz", members);

    g_assert_cmpuint(all->len, ==, 4);
    g_assert_cmpuint(al->len, ==, 2);
    g_assert_cmpuint(ali->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(ali, 0), ==, "alice");
    g_assert_cmpuint(none->len, ==, 0);
}

/*
 * Offered case-insensitively, because the `@` form is.
 *
 * A completion that showed fewer names than the matcher accepts would
 * send somebody looking for a member that is right in front of them.
 */
static void
test_candidates_ignore_case_because_the_at_form_does(void)
{
    g_autoptr(GPtrArray) members = ids("alice", "bob", NULL);
    g_autoptr(GPtrArray) upper = clawt_mention_candidates("AL", members);

    g_assert_cmpuint(upper->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(upper, 0), ==, "alice");

    /* And what it offers is what delivery would then accept. */
    g_assert_true(clawt_mention_names("@ALice hello", "alice", NULL));
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
    g_test_add_func("/mention/at-form-folds-case-bare-does-not",
                    test_the_at_form_ignores_case_and_a_bare_word_does_not);
    g_test_add_func("/mention/multibyte-letter-is-not-a-boundary",
                    test_a_multibyte_letter_is_not_a_boundary);
    g_test_add_func("/mention/an-address-is-refused-from-both-ends",
                    test_an_address_is_refused_from_both_ends);
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
    g_test_add_func("/mention/a-bare-at-is-an-empty-prefix",
                    test_a_bare_at_is_an_empty_prefix_not_nothing);
    g_test_add_func("/mention/a-word-break-ends-the-prefix",
                    test_a_word_break_ends_the_prefix);
    g_test_add_func("/mention/an-address-opens-no-completion",
                    test_an_address_does_not_open_a_completion);
    g_test_add_func("/mention/prefix-is-taken-at-the-cursor",
                    test_the_prefix_is_taken_at_the_cursor);
    g_test_add_func("/mention/candidates-narrow-as-you-type",
                    test_candidates_narrow_as_you_type);
    g_test_add_func("/mention/candidates-ignore-case",
                    test_candidates_ignore_case_because_the_at_form_does);

    return g_test_run();
}
