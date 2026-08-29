/*
 * clawt-turn-watch.c - A turn that stops making progress has to end
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-turn-watch.h"

/*
 * One turn's budget.
 *
 * While it is running the deadline is what matters and @remaining is
 * stale; while it is held the reverse.  Two fields rather than one,
 * because "when does this expire" and "how much is left" are different
 * questions and a single field would have to be rewritten on every tick
 * of a held turn to answer both.
 */
typedef struct {
    gint64   deadline;    /* monotonic usec, valid while holds == 0 */
    gint64   remaining;   /* usec, valid while holds > 0 */
    guint    holds;
    gboolean expired;     /* latched, so a spent budget cannot be un-spent */
} Watched;

struct _ClawtTurnWatch {
    GObject parent_instance;

    gboolean    resets_on_activity;
    guint       budget_seconds;
    GHashTable *watched;       /* key -> Watched* */

    ClawtTurnWatchClockFunc clock;
    gpointer                clock_data;
    GDestroyNotify          clock_notify;
};

G_DEFINE_FINAL_TYPE(ClawtTurnWatch, clawt_turn_watch, G_TYPE_OBJECT)

static gint64
now_usec(ClawtTurnWatch *self)
{
    if (self->clock != NULL)
        return self->clock(self->clock_data);

    return g_get_monotonic_time();
}

static gint64
budget_usec(ClawtTurnWatch *self)
{
    return (gint64)self->budget_seconds * G_USEC_PER_SEC;
}

/* How much is left, whether the turn is running or parked. */
static gint64
remaining_of(ClawtTurnWatch *self, Watched *entry)
{
    gint64 left;

    if (entry->expired)
        return 0;

    if (entry->holds > 0)
        return MAX(entry->remaining, 0);

    left = entry->deadline - now_usec(self);

    return MAX(left, 0);
}

static ClawtTurnWatch *
turn_watch_new(gboolean resets_on_activity)
{
    ClawtTurnWatch *self = g_object_new(CLAWT_TYPE_TURN_WATCH, NULL);

    self->resets_on_activity = resets_on_activity;

    return self;
}

ClawtTurnWatch *
clawt_turn_watch_new_activity(void)
{
    return turn_watch_new(TRUE);
}

ClawtTurnWatch *
clawt_turn_watch_new_work(void)
{
    return turn_watch_new(FALSE);
}

void
clawt_turn_watch_set_budget(ClawtTurnWatch *self, guint seconds)
{
    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    self->budget_seconds = seconds;
}

guint
clawt_turn_watch_get_budget(ClawtTurnWatch *self)
{
    g_return_val_if_fail(CLAWT_IS_TURN_WATCH(self), 0);

    return self->budget_seconds;
}

void
clawt_turn_watch_set_clock(ClawtTurnWatch          *self,
                           ClawtTurnWatchClockFunc  clock,
                           gpointer                 user_data,
                           GDestroyNotify           notify)
{
    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    if (self->clock_notify != NULL && self->clock_data != NULL)
        self->clock_notify(self->clock_data);

    self->clock = clock;
    self->clock_data = user_data;
    self->clock_notify = notify;
}

void
clawt_turn_watch_begin(ClawtTurnWatch *self, const gchar *key)
{
    Watched *entry;

    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    if (key == NULL)
        return;

    /*
     * A budget of zero is the option turned off, and an entry with no
     * deadline would be reported as expired on the first sweep.
     */
    if (self->budget_seconds == 0) {
        g_hash_table_remove(self->watched, key);
        return;
    }

    entry = g_new0(Watched, 1);
    entry->deadline = now_usec(self) + budget_usec(self);

    g_hash_table_insert(self->watched, g_strdup(key), entry);
}

void
clawt_turn_watch_note_activity(ClawtTurnWatch *self, const gchar *key)
{
    Watched *entry;

    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    if (key == NULL || !self->resets_on_activity)
        return;

    entry = g_hash_table_lookup(self->watched, key);

    if (entry == NULL || entry->expired)
        return;

    /*
     * A held turn is credited the same fresh budget, so that answering
     * the question and going quiet again gets the whole allowance rather
     * than whatever was left when the question was asked.
     */
    if (entry->holds > 0)
        entry->remaining = budget_usec(self);
    else
        entry->deadline = now_usec(self) + budget_usec(self);
}

void
clawt_turn_watch_end(ClawtTurnWatch *self, const gchar *key)
{
    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    if (key != NULL)
        g_hash_table_remove(self->watched, key);
}

gboolean
clawt_turn_watch_is_watching(ClawtTurnWatch *self, const gchar *key)
{
    g_return_val_if_fail(CLAWT_IS_TURN_WATCH(self), FALSE);

    if (key == NULL)
        return FALSE;

    return g_hash_table_lookup(self->watched, key) != NULL;
}

void
clawt_turn_watch_hold(ClawtTurnWatch *self, const gchar *key)
{
    Watched *entry;

    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    if (key == NULL)
        return;

    entry = g_hash_table_lookup(self->watched, key);

    if (entry == NULL)
        return;

    if (entry->holds > 0) {
        entry->holds++;
        return;
    }

    entry->remaining = remaining_of(self, entry);

    /*
     * A hold that finds the budget already gone expires the turn instead
     * of parking it.
     *
     * The main loop can be late: the decision event is delivered from an
     * idle, so the card can arrive after the deadline has passed.  Taking
     * the hold then would hand the turn an unbounded extension for a
     * budget that was spent before anybody asked anything, and the
     * expiry would never be reported at all.
     */
    if (entry->remaining <= 0) {
        entry->expired = TRUE;
        return;
    }

    entry->holds = 1;
}

void
clawt_turn_watch_release(ClawtTurnWatch *self, const gchar *key)
{
    Watched *entry;

    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    if (key == NULL)
        return;

    entry = g_hash_table_lookup(self->watched, key);

    /*
     * Clamped at zero rather than allowed to go negative.
     *
     * A resolve can arrive for a card this turn never opened -- the
     * stale cleanup after an interrupt does exactly that -- and a
     * counter below zero needs as many spurious holds to climb back,
     * during which the budget is not running at all.
     */
    if (entry == NULL || entry->holds == 0)
        return;

    entry->holds--;

    if (entry->holds == 0)
        entry->deadline = now_usec(self) + MAX(entry->remaining, 0);
}

guint
clawt_turn_watch_get_holds(ClawtTurnWatch *self, const gchar *key)
{
    Watched *entry;

    g_return_val_if_fail(CLAWT_IS_TURN_WATCH(self), 0);

    if (key == NULL)
        return 0;

    entry = g_hash_table_lookup(self->watched, key);

    return (entry != NULL) ? entry->holds : 0;
}

gint64
clawt_turn_watch_remaining(ClawtTurnWatch *self, const gchar *key)
{
    Watched *entry;

    g_return_val_if_fail(CLAWT_IS_TURN_WATCH(self), -1);

    if (key == NULL)
        return -1;

    entry = g_hash_table_lookup(self->watched, key);

    if (entry == NULL)
        return -1;

    return remaining_of(self, entry);
}

GPtrArray *
clawt_turn_watch_collect_expired(ClawtTurnWatch *self)
{
    GPtrArray *expired;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_return_val_if_fail(CLAWT_IS_TURN_WATCH(self), NULL);

    expired = g_ptr_array_new_with_free_func(g_free);

    g_hash_table_iter_init(&iter, self->watched);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        Watched *entry = value;

        /*
         * A held turn is not a stalled one -- unless the hold was taken
         * on a budget that had already run out, which latches @expired
         * precisely so that case is still reported.
         */
        if (entry->holds > 0 && !entry->expired)
            continue;

        if (remaining_of(self, entry) > 0)
            continue;

        g_ptr_array_add(expired, g_strdup(key));
    }

    /*
     * Removed after the walk rather than inside it: g_hash_table_iter_remove()
     * is safe, but the keys handed back are copies precisely so the
     * caller owns something the table cannot free underneath it.
     */
    {
        guint i;

        for (i = 0; i < expired->len; i++)
            g_hash_table_remove(self->watched, g_ptr_array_index(expired, i));
    }

    return expired;
}

void
clawt_turn_watch_reset(ClawtTurnWatch *self)
{
    g_return_if_fail(CLAWT_IS_TURN_WATCH(self));

    g_hash_table_remove_all(self->watched);
}

static void
clawt_turn_watch_finalize(GObject *object)
{
    ClawtTurnWatch *self = CLAWT_TURN_WATCH(object);

    if (self->clock_notify != NULL && self->clock_data != NULL)
        self->clock_notify(self->clock_data);

    g_clear_pointer(&self->watched, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_turn_watch_parent_class)->finalize(object);
}

static void
clawt_turn_watch_class_init(ClawtTurnWatchClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_turn_watch_finalize;
}

static void
clawt_turn_watch_init(ClawtTurnWatch *self)
{
    self->watched = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_free);
}
