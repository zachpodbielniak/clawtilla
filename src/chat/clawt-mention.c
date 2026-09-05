/*
 * clawt-mention.c - Who a message names
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-mention.h"

#include <string.h>

/*
 * Whether the character before @at ends a word.
 *
 * Decoded as UTF-8, not as a byte.  g_ascii_isalnum() answers FALSE for
 * every byte of a multi-byte character, so a byte test read the second
 * byte of `ñ` as a word boundary and "hasta mañana" addressed an agent
 * called `ana`.  A letter is a letter whatever it is encoded as.
 *
 * `@` counts as part of a word here, which is what keeps the bare form
 * from rescuing an address the `@` form has already refused: in
 * `zach@bob.com` the `@` is not itself at a boundary, so the `@` form
 * declines -- and if the bare form then treated that same `@` as a
 * boundary it would match `bob` anyway and the check would be for
 * nothing.
 *
 * `_` and `-` count too, on this side as well as the other.  They were
 * excluded only after the name for a long time, so `alpha-bob` and
 * `my_bob` addressed an agent called `bob` while `bob-2` did not --
 * asymmetric, and both characters are legal in an id.
 */
static gboolean
character_joins_a_word(gunichar c)
{
    return g_unichar_isalnum(c) || c == '_' || c == '-' || c == '@';
}

static gboolean
boundary_before(const gchar *body, const gchar *at)
{
    const gchar *previous;

    if (at == body)
        return TRUE;

    previous = g_utf8_find_prev_char(body, at);

    if (previous == NULL)
        return TRUE;

    /*
     * Validated, because a body is text a model wrote and nothing
     * promises it is well-formed UTF-8.  g_utf8_get_char() trusts the
     * lead byte's declared length and would read past the terminator on
     * a truncated sequence; the validated form is bounded by it.  A byte
     * that decodes to nothing is not a letter, so it ends a word.
     */
    {
        gunichar c = g_utf8_get_char_validated(previous, at - previous);

        if (c == (gunichar)-1 || c == (gunichar)-2)
            return TRUE;

        return !character_joins_a_word(c);
    }
}

/*
 * And the character after it, by the same rule -- `@` included.
 *
 * It was left out on this side, so the bare form matched the `bob` in
 * `bob@example.com`: the `@` form correctly refused the address, and
 * then the bare form took it anyway from the other end.  The rule is
 * symmetric or it is not a rule.
 */
static gboolean
boundary_after(const gchar *at, gsize length)
{
    const gchar *next = at + length;
    gunichar c;

    if (*next == '\0')
        return TRUE;

    c = g_utf8_get_char_validated(next, -1);

    if (c == (gunichar)-1 || c == (gunichar)-2)
        return TRUE;

    return !character_joins_a_word(c);
}

/*
 * Whether @body contains @needle as an address.
 *
 * @at_form_only is what separates `@all` from the word "all": a
 * broadcast has to be written deliberately, and "that's all for now" is
 * a sentence rather than an instruction to wake everybody.
 *
 * The two forms differ on case, deliberately.
 *
 * `@name` is the deliberate address -- it is what the delivery preamble
 * tells every member to write -- so it ignores case: somebody typing
 * `@Bob` plainly meant `bob`, and a mention that fails on a capital
 * reaches nobody and says nothing about it.
 *
 * A *bare* name is not an address, it is a word that happens to be
 * spelled like an id, so it must match the id exactly.  Case-folding it
 * charged an agent called `writer` a whole model turn for the sentence
 * "Writer's block on section 2", and an agent called `research` for any
 * sentence opening with "Research".  A missed bare name costs a person
 * one `@`; a false one costs a model turn nobody asked for.
 */
static gboolean
mentions(const gchar *body, const gchar *needle, gboolean at_form_only)
{
    const gchar *p;
    gsize length;

    if (body == NULL || needle == NULL || *needle == '\0')
        return FALSE;

    length = strlen(needle);

    /*
     * By bytes, not by characters.  An id is ASCII by construction
     * (clawt_is_valid_id), so a match can only begin at an ASCII byte
     * and a continuation byte can never start one -- while stepping by
     * characters would trust a malformed lead byte to skip forward and
     * could step over the terminator.
     */
    for (p = body; *p != '\0'; p++) {
        if (*p == '@') {
            if (boundary_before(body, p) &&
                g_ascii_strncasecmp(p + 1, needle, length) == 0 &&
                boundary_after(p + 1, length))
                return TRUE;

            continue;
        }

        if (at_form_only)
            continue;

        if (boundary_before(body, p) &&
            strncmp(p, needle, length) == 0 &&
            boundary_after(p, length))
            return TRUE;
    }

    return FALSE;
}

/*
 * Whether a display name is usable as an address at all.
 *
 * One word, and made of what an id is made of.  "Oryx" addresses an
 * agent; "Oryx the Researcher" would match every message containing the
 * word "the".
 */
static gboolean
is_addressable_name(const gchar *name)
{
    const gchar *p;

    if (name == NULL || *name == '\0')
        return FALSE;

    for (p = name; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    }

    return TRUE;
}

gboolean
clawt_mention_names(const gchar *body,
                    const gchar *id,
                    const gchar *display_name)
{
    if (mentions(body, id, FALSE))
        return TRUE;

    /*
     * The name only when it says something the id does not.  Matching is
     * case-insensitive, so a display name that is the id with a capital
     * -- which is nearly all of them -- has already been answered above.
     */
    if (display_name != NULL &&
        g_ascii_strcasecmp(display_name, id) != 0 &&
        is_addressable_name(display_name))
        return mentions(body, display_name, FALSE);

    return FALSE;
}

gboolean
clawt_mention_is_broadcast(const gchar *body)
{
    return mentions(body, CLAWT_MENTION_ALL, TRUE);
}

GPtrArray *
clawt_mention_list(const gchar *body, GPtrArray *candidates)
{
    GPtrArray *named = g_ptr_array_new_with_free_func(g_free);
    guint i;

    if (body == NULL || candidates == NULL)
        return named;

    for (i = 0; i < candidates->len; i++) {
        const gchar *id = g_ptr_array_index(candidates, i);

        /*
         * Once each.  A body that writes a name twice addresses one
         * agent, and a caller counting this array is counting turns.
         */
        if (id != NULL && clawt_mention_names(body, id, NULL) &&
            !g_ptr_array_find_with_equal_func(named, id,
                                              (GEqualFunc)g_str_equal,
                                              NULL))
            g_ptr_array_add(named, g_strdup(id));
    }

    return named;
}

gchar *
clawt_mention_prefix_at(const gchar *body, gsize offset)
{
    const gchar *at;
    const gchar *p;

    if (body == NULL || offset > strlen(body))
        return NULL;

    /*
     * Back from the cursor to the `@` that opened the token.
     *
     * Anything an id cannot contain ends the search: a mention is one
     * word, so a space, a newline or punctuation between the `@` and
     * the cursor means whatever was being typed is no longer a name.
     */
    at = NULL;

    for (p = body + offset; p > body; p--) {
        gchar c = p[-1];

        if (c == '@') {
            at = p - 1;
            break;
        }

        if (!g_ascii_isalnum(c) && c != '_' && c != '-')
            return NULL;
    }

    if (at == NULL)
        return NULL;

    /*
     * And the same boundary rule delivery applies, so an address does
     * not open a member list halfway through typing it.
     */
    if (!boundary_before(body, at))
        return NULL;

    return g_strndup(at + 1, (gsize)((body + offset) - (at + 1)));
}

GPtrArray *
clawt_mention_candidates(const gchar *prefix, GPtrArray *members)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    gsize length;
    guint i;

    if (members == NULL)
        return out;

    length = (prefix != NULL) ? strlen(prefix) : 0;

    for (i = 0; i < members->len; i++) {
        const gchar *member = g_ptr_array_index(members, i);

        if (member == NULL)
            continue;

        /*
         * Case-insensitive, because the `@` form is.  A completion that
         * offered fewer names than the matcher accepts would send
         * somebody looking for a member that is right there.
         */
        if (length == 0 ||
            g_ascii_strncasecmp(member, prefix, length) == 0)
            g_ptr_array_add(out, g_strdup(member));
    }

    return out;
}
