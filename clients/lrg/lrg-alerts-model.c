/*
 * lrg-alerts-model.c - The decisions the notifications drawer makes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The list is the daemon's (`notice.list`): which events become rows,
 * their words, and what the bell counts are decided there, once, for
 * every client.  What a client still decides is which rows a narrowed
 * drawer shows and when the sidebar has to redraw for a mute -- both
 * here, without a display, so tests/test-lrg-models-chat.c can hold
 * them.
 */

#include "clawt-lrg-model.h"

gboolean
clawt_lrg_notice_in_scope(const gchar *agent, const gchar *scope)
{
    return scope == NULL || g_strcmp0(agent, scope) == 0;
}

gboolean
clawt_lrg_muted_take(GHashTable *muted, GHashTable *from)
{
    GHashTableIter iter;
    gpointer key;
    gboolean same = TRUE;

    g_return_val_if_fail(muted != NULL, FALSE);

    /* NULL is the empty set: a reply with no mutes in it */
    if (g_hash_table_size(muted) != (from != NULL ? g_hash_table_size(from)
                                                  : 0)) {
        same = FALSE;
    } else if (from != NULL) {
        g_hash_table_iter_init(&iter, from);

        while (g_hash_table_iter_next(&iter, &key, NULL)) {
            if (!g_hash_table_contains(muted, key)) {
                same = FALSE;
                break;
            }
        }
    }

    if (same)
        return FALSE;

    g_hash_table_remove_all(muted);

    if (from != NULL) {
        g_hash_table_iter_init(&iter, from);

        while (g_hash_table_iter_next(&iter, &key, NULL))
            g_hash_table_add(muted, g_strdup(key));
    }

    return TRUE;
}
