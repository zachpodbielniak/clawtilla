/*
 * clawt-chat-run.h - Grouping consecutive messages into runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_CHAT_RUN_H
#define CLAWT_CHAT_RUN_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_chat_run_is_start:
 * @previous_sender: (nullable): who sent the message before this one
 * @previous_day: (nullable): its day, as `%Y-%m-%d`
 * @sender: who sent this one
 * @day: this one's day, as `%Y-%m-%d`
 * @out_new_day: (out) (optional): whether the date changed
 *
 * Whether a message begins a new run.
 *
 * A run is consecutive messages from one sender, and it gets one header
 * rather than one per message -- that grouping is what makes a stack of
 * labels read as a conversation.  A run ends at a different sender or at
 * a day boundary, and deliberately **not** at a time gap: a fifteen
 * minute rule would be a third constant with no evidence behind it, and
 * agent traffic arrives in bursts where it would fire constantly.
 *
 * A pure function so both clients ask the same question and get the same
 * answer -- two implementations of "is this a new run" would differ
 * exactly once, on the case nobody looked at.
 *
 * Returns: %TRUE when this message starts a run
 */
gboolean clawt_chat_run_is_start(const gchar *previous_sender,
                                 const gchar *previous_day,
                                 const gchar *sender,
                                 const gchar *day,
                                 gboolean    *out_new_day);

/**
 * clawt_chat_day_label:
 * @when: the message's time
 * @now: (nullable): what to call "now", or %NULL for the real clock
 *
 * "Today", "Yesterday", or "Wednesday 25 August".
 *
 * @now exists so the two relative answers can be tested without waiting
 * for midnight, which is the only time they would otherwise be wrong.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_chat_day_label(GDateTime *when, GDateTime *now);

G_END_DECLS

#endif /* CLAWT_CHAT_RUN_H */
