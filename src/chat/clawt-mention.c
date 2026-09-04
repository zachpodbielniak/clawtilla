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
boundary_before(const gchar *body, const gchar *at)
{
    gchar previous;

    if (at == body)
        return TRUE;

    previous = at[-1];

    return !g_ascii_isalnum(previous) &&
           previous != '_' && previous != '-' && previous != '@';
}

/* And the character after it, by the same rule. */
static gboolean
boundary_after(const gchar *at, gsize length)
{
    gchar next = at[length];

    return next == '\0' ||
           (!g_ascii_isalnum(next) && next != '_' && next != '-');
}

/*
 * Whether @body contains @needle as an address.
 *
 * @at_form_only is what separates `@all` from the word "all": a
 * broadcast has to be written deliberately, and "that's all for now" is
 * a sentence rather than an instruction to wake everybody.
 */
static gboolean
mentions(const gchar *body, const gchar *needle, gboolean at_form_only)
{
    const gchar *p;
    gsize length;

    if (body == NULL || needle == NULL || *needle == '\0')
        return FALSE;

    length = strlen(needle);

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
            g_ascii_strncasecmp(p, needle, length) == 0 &&
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
