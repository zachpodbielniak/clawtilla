/*
 * gtk-follow.c - Keeping a growing transcript at its newest message
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two views draw a conversation that grows underneath the reader: the
 * chat, and the Flow tab's right-hand pane.  The chat had all of this
 * and Flow had none of it, so Flow opened at the oldest message of the
 * two hundred it had loaded and went back there every time anything in
 * the fleet spoke.  One implementation, held once per transcript, for
 * the same reason there is now one row builder rather than two.
 *
 * The rule itself -- whether an offset counts as the bottom -- is
 * clawt_transcript_is_at_bottom() in libclawt, so its tolerance is
 * exercised on both sides and at its boundary without a window.  What
 * is here is the GTK half: when to ask, and how to write the answer
 * without fighting the frame clock.
 */

#include "clawt-window-private.h"

/*
 * A scroll that cannot outlive the window.
 *
 * A plain g_idle_add() of the follower runs after the window has been
 * destroyed if it is closed in the same turn a message arrives, and the
 * callback then reads freed memory -- the follower is embedded in the
 * window's instance struct.  Holding a reference for the life of the
 * idle costs nothing and removes the race.
 */
typedef struct {
    ClawtWindow    *window;   /* held */
    ClawtGtkFollow *follow;   /* borrowed; lives inside `window` */
} FollowIdle;

static void
follow_idle_free(gpointer data)
{
    FollowIdle *idle = data;

    g_object_unref(idle->window);
    g_free(idle);
}

/*
 * Scrolls to the bottom, but only when the reader was already there.
 *
 * Yanking somebody down mid-read because a message arrived is the single
 * most annoying thing a chat window can do.
 */
static gboolean
follow_to_bottom(gpointer user_data)
{
    FollowIdle *idle = user_data;
    ClawtGtkFollow *follow = idle->follow;
    GtkAdjustment *adjustment;

    if (!follow->following || follow->scroll == NULL)
        return G_SOURCE_REMOVE;

    adjustment = gtk_scrolled_window_get_vadjustment(follow->scroll);
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) -
                             gtk_adjustment_get_page_size(adjustment));

    return G_SOURCE_REMOVE;
}

void
clawt_gtk_follow_queue(ClawtGtkFollow *follow)
{
    FollowIdle *idle;

    if (follow == NULL || follow->window == NULL)
        return;

    idle = g_new0(FollowIdle, 1);
    idle->window = g_object_ref(follow->window);
    idle->follow = follow;

    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, follow_to_bottom, idle,
                    follow_idle_free);
}

/*
 * Notices that the transcript has grown, and asks for a scroll.
 *
 * It asks rather than scrolls, and that distinction is the whole point.
 * These two notifies are emitted by GtkViewport from inside its own
 * size-allocate, at the moment it reconfigures the adjustment for a
 * layout it has already positioned its child for.  Writing `value` here
 * moves the number and does not move the picture: the viewport has
 * finished placing the child for this pass, and the allocation the
 * write asks for is folded into the pass that is already running rather
 * than starting another one.  Nothing queues a further one, so the
 * displayed offset stays where it was while the adjustment reports the
 * new bottom.
 *
 * That mismatch is stable, not transient -- measured at 68px, exactly
 * one message, and still there ten seconds later.  It is also invisible
 * to every correction here, because all of them test the adjustment and
 * the adjustment is already right.  follow_to_bottom() in particular
 * finds value == bottom and returns without doing anything, so the one
 * write that would have happened outside a layout pass is the one this
 * handler suppresses.
 *
 * Queueing instead puts the write in an idle, after the pass has
 * finished.  It is then a real value change, the viewport allocates
 * again, and the newest message is on screen.
 *
 * page-size as well as upper, because typing grows the composer and
 * shrinks the transcript above it, which moves the bottom without adding
 * anything.
 */
static void
on_grew(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtGtkFollow *follow = user_data;
    GtkAdjustment *adjustment = GTK_ADJUSTMENT(object);
    gdouble bottom;

    (void)pspec;

    if (!follow->following)
        return;

    bottom = gtk_adjustment_get_upper(adjustment) -
             gtk_adjustment_get_page_size(adjustment);

    /*
     * Only when it is not already there.  This runs on every layout
     * pass, and a queued idle that would find nothing to do is worth
     * not queueing.
     */
    if (gtk_adjustment_get_value(adjustment) < bottom)
        clawt_gtk_follow_queue(follow);
}

static void
on_scrolled(GtkAdjustment *adjustment, gpointer user_data)
{
    ClawtGtkFollow *follow = user_data;

    clawt_gtk_follow_set(follow, clawt_transcript_is_at_bottom(
                         gtk_adjustment_get_value(adjustment),
                         gtk_adjustment_get_upper(adjustment),
                         gtk_adjustment_get_page_size(adjustment)));
}

/*
 * The only place `following` changes, and the reason the chat's two
 * unread affordances cannot disagree.
 *
 * False means the reader is deliberately somewhere above the live edge,
 * and the view already refuses to move for them -- see
 * follow_to_bottom().  That refusal is right; saying nothing about it
 * was not, which is what `armed` is for: it runs on the way back to the
 * edge, so every path that re-arms following clears what the view drew
 * about not following, with no new cases.
 */
void
clawt_gtk_follow_set(ClawtGtkFollow *follow, gboolean following)
{
    if (follow == NULL)
        return;

    follow->following = following;

    if (following && follow->armed != NULL && follow->window != NULL)
        follow->armed(follow->window);
}

gboolean
clawt_gtk_follow_active(const ClawtGtkFollow *follow)
{
    return follow != NULL && follow->following;
}

void
clawt_gtk_follow_attach(ClawtGtkFollow *follow, ClawtWindow *window,
                        GtkScrolledWindow *scroll,
                        void (*armed)(ClawtWindow *self))
{
    GtkAdjustment *adjustment;

    g_return_if_fail(follow != NULL);
    g_return_if_fail(GTK_IS_SCROLLED_WINDOW(scroll));

    follow->window = window;
    follow->scroll = scroll;
    follow->armed = armed;

    /*
     * Following starts true, because a conversation opens at its newest
     * message.  Flow used to open at its oldest, which on a room with
     * any history at all reads as a page that failed to load the rest.
     */
    follow->following = TRUE;

    adjustment = gtk_scrolled_window_get_vadjustment(scroll);

    /*
     * Maintained from three places: the reader scrolling, and the
     * content growing (either notify), because "am I at the bottom"
     * changes for both reasons.
     */
    g_signal_connect(adjustment, "notify::upper", G_CALLBACK(on_grew),
                     follow);
    g_signal_connect(adjustment, "notify::page-size", G_CALLBACK(on_grew),
                     follow);
    g_signal_connect(adjustment, "value-changed", G_CALLBACK(on_scrolled),
                     follow);
}
