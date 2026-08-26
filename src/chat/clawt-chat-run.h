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

/**
 * clawt_chat_time_label:
 * @when: (nullable): the message's time
 *
 * What a transcript stamps a message with: `HH:MM`.
 *
 * One function because both clients draw this in two places each -- the
 * run header and the continuation gutter -- and they had four answers
 * between them.  The web transcript rendered a *relative* time ("2m
 * ago") while GTK rendered the clock, so the same conversation carried
 * two conventions depending on which client you opened it in.
 *
 * The clock rather than the relative form, for two reasons.  A relative
 * time rendered on the server is wrong the moment it is sent and gets
 * wronger: nothing re-renders a message that has not changed, so a page
 * left open shows "2m ago" for an hour.  And a transcript is a record --
 * an absolute time is what lets a line here be matched against the event
 * log, a task, or a journal entry.  Recency belongs to the lists that
 * are about recent activity, which keep their own relative formatting.
 *
 * The date is not in it: a day divider carries that, and repeating it on
 * every message would spend the gutter's whole width saying what the row
 * above already said.
 *
 * 24-hour, deliberately, rather than the locale's format.  This string
 * has to fit the slot the avatar reserves -- 32px in GTK, 28px in the
 * web -- and a 12-hour locale renders "4:23 PM", which does not.  A
 * stamp that wraps to two lines in a gutter is worse than one in an
 * unfamiliar format.
 *
 * Returns: (transfer full) (nullable): the label, or %NULL for a %NULL
 *   @when
 */
gchar *clawt_chat_time_label(GDateTime *when);

G_END_DECLS

#endif /* CLAWT_CHAT_RUN_H */
