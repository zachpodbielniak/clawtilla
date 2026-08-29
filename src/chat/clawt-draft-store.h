/*
 * clawt-draft-store.h - Composer text somebody has not sent yet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A half-typed message belongs to the person, not to the fleet, so it
 * lives in the *client's* own config beside `connections.yaml` and never
 * in `clawtilla.yaml`.  The same reasoning as the connection profiles
 * themselves: a laptop reaching a workstation may have no fleet at all,
 * and the client switches daemons at runtime -- a draft written into the
 * fleet's config would follow the wrong machine and would be visible to
 * every other client of that daemon.
 *
 * One function per operation rather than a live object, because both
 * clients want the same two moments -- read one when a room opens, write
 * one when it closes -- and a cached object would have to be invalidated
 * by the other client editing the same file.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_draft_store_default_path:
 *
 * Returns: (transfer full): `$XDG_CONFIG_HOME/clawtilla/drafts.yaml`
 */
gchar *clawt_draft_store_default_path(void);

/**
 * clawt_draft_key:
 * @profile: (nullable): the connection profile the client is on, or %NULL
 *   for the local daemon
 * @room_id: the conversation
 *
 * The one spelling of a draft's key.
 *
 * The profile is in it because a client switches daemons at runtime and
 * two fleets can hold an agent with the same id -- keying on the agent
 * alone would show one machine's half-written message in the other
 * machine's composer, which is a small privacy failure and a large
 * confusion. Both clients call this rather than building the string, for
 * the reason every shared rule in this tree lives in the library.
 *
 * Returns: (transfer full): the key
 */
gchar *clawt_draft_key(const gchar *profile, const gchar *room_id);

/**
 * clawt_draft_store_load:
 * @path: (nullable): the file, or %NULL for clawt_draft_store_default_path()
 * @error: (out) (optional): return location for a #GError
 *
 * A missing file is an empty set of drafts, not an error: nobody has
 * typed anything yet is the ordinary case.
 *
 * Returns: (transfer full) (element-type utf8 utf8) (nullable): room id to
 *   draft text, or %NULL only when the file exists and could not be read
 */
GHashTable *clawt_draft_store_load(const gchar *path, GError **error);

/**
 * clawt_draft_store_save:
 * @path: (nullable): the file, or %NULL for the default
 * @drafts: (element-type utf8 utf8): room id to draft text
 * @error: (out) (optional): return location for a #GError
 *
 * Written at 0600 through the same atomic write everything else uses. A
 * draft is what somebody was about to say, which is no less private than
 * what they did.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_draft_store_save(const gchar *path,
                                GHashTable  *drafts,
                                GError     **error);

/**
 * clawt_draft_store_get:
 * @path: (nullable): the file, or %NULL for the default
 * @room_id: which conversation
 *
 * Returns: (transfer full) (nullable): the held text, or %NULL when there
 *   is none
 */
gchar *clawt_draft_store_get(const gchar *path, const gchar *room_id);

/**
 * clawt_draft_store_set:
 * @path: (nullable): the file, or %NULL for the default
 * @room_id: which conversation
 * @text: (nullable): what is in the composer, or %NULL/empty to forget it
 * @error: (out) (optional): return location for a #GError
 *
 * Empty text removes the entry rather than storing a blank one, so a
 * file left behind by somebody who cleared every composer is empty
 * rather than a list of rooms they once opened.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_draft_store_set(const gchar  *path,
                               const gchar  *room_id,
                               const gchar  *text,
                               GError      **error);

G_END_DECLS
