/*
 * clawt-loop-guard.c - What stops agents talking to each other for ever
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-loop-guard.h"

typedef struct {
    GQueue *timestamps;   /* gint64*, monotonic microseconds, oldest first */
} RateWindow;

struct _ClawtLoopGuard {
    GObject parent_instance;

    guint   max_hops;
    guint   rate_per_minute;
    guint   cycle_window;
    gdouble task_budget_usd;

    GHashTable *rates;         /* agent_id -> RateWindow */
    GHashTable *recent;        /* room_id -> GQueue of fingerprints */
    GHashTable *task_spend;    /* task_id -> gdouble* */
};

G_DEFINE_FINAL_TYPE(ClawtLoopGuard, clawt_loop_guard, G_TYPE_OBJECT)

static void
rate_window_free(gpointer data)
{
    RateWindow *window = data;

    g_queue_free_full(window->timestamps, g_free);
    g_free(window);
}

static void
fingerprint_queue_free(gpointer data)
{
    g_queue_free_full(data, g_free);
}

ClawtLoopGuard *
clawt_loop_guard_new(void)
{
    return g_object_new(CLAWT_TYPE_LOOP_GUARD, NULL);
}

void
clawt_loop_guard_set_limits(ClawtLoopGuard *self,
                            guint           max_hops,
                            guint           rate_per_minute,
                            guint           cycle_window)
{
    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    self->max_hops = max_hops;
    self->rate_per_minute = rate_per_minute;
    self->cycle_window = cycle_window;
}

void
clawt_loop_guard_set_task_budget(ClawtLoopGuard *self, gdouble budget_usd)
{
    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    self->task_budget_usd = budget_usd;
}

static gboolean
check_hops(ClawtLoopGuard *self, ClawtMessage *message, GError **error)
{
    gint depth = clawt_message_get_depth(message);

    if (self->max_hops == 0 || depth < (gint)self->max_hops)
        return TRUE;

    /*
     * The message names the limit it hit, so the agent can say something
     * useful rather than reporting an unexplained failure.
     */
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                "this message is %d hops from the original request, and the "
                "limit is %u. Delegation has gone deeper than intended; "
                "answer directly rather than passing it on again.",
                depth, self->max_hops);

    return FALSE;
}

static gboolean
check_rate(ClawtLoopGuard *self, ClawtMessage *message, GError **error)
{
    const gchar *sender = clawt_message_get_sender_id(message);
    RateWindow *window;
    gint64 now;
    gint64 cutoff;

    if (self->rate_per_minute == 0 || sender == NULL)
        return TRUE;

    now = g_get_monotonic_time();
    cutoff = now - (gint64)G_USEC_PER_SEC * 60;

    window = g_hash_table_lookup(self->rates, sender);
    if (window == NULL) {
        window = g_new0(RateWindow, 1);
        window->timestamps = g_queue_new();
        g_hash_table_insert(self->rates, g_strdup(sender), window);
    }

    /* A sliding window rather than a per-minute bucket: a bucket lets an
     * agent send its whole allowance twice across a boundary. */
    while (!g_queue_is_empty(window->timestamps)) {
        gint64 *oldest = g_queue_peek_head(window->timestamps);

        if (*oldest >= cutoff)
            break;

        g_free(g_queue_pop_head(window->timestamps));
    }

    if (g_queue_get_length(window->timestamps) >= self->rate_per_minute) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "'%s' has sent %u messages in the last minute, which is "
                    "its limit. Something is looping; wait before sending "
                    "more.", sender, self->rate_per_minute);
        return FALSE;
    }

    {
        gint64 *stamp = g_new(gint64, 1);

        *stamp = now;
        g_queue_push_tail(window->timestamps, stamp);
    }

    return TRUE;
}

static gboolean
check_cycle(ClawtLoopGuard *self, ClawtMessage *message, GError **error)
{
    const gchar *room = clawt_message_get_room_id(message);
    g_autofree gchar *fingerprint = NULL;
    GQueue *history;
    GList *l;

    if (self->cycle_window == 0 || room == NULL)
        return TRUE;

    fingerprint = clawt_message_body_fingerprint(message);

    history = g_hash_table_lookup(self->recent, room);
    if (history == NULL) {
        history = g_queue_new();
        g_hash_table_insert(self->recent, g_strdup(room), history);
    }

    for (l = history->head; l != NULL; l = l->next) {
        if (g_strcmp0(l->data, fingerprint) != 0)
            continue;

        /*
         * This is the case the hop limit misses entirely: two agents
         * alternating the same two replies, each message a fresh chain with
         * a depth of one, for ever.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "this exact message has already been sent to '%s' "
                    "recently. The conversation is going in circles; say "
                    "something different or stop.", room);
        return FALSE;
    }

    g_queue_push_tail(history, g_steal_pointer(&fingerprint));

    while (g_queue_get_length(history) > self->cycle_window)
        g_free(g_queue_pop_head(history));

    return TRUE;
}

static gboolean
check_budget(ClawtLoopGuard *self, ClawtMessage *message, GError **error)
{
    const gchar *task_id = clawt_message_get_task_id(message);
    gdouble *spent;

    if (self->task_budget_usd <= 0.0 || task_id == NULL)
        return TRUE;

    spent = g_hash_table_lookup(self->task_spend, task_id);
    if (spent == NULL || *spent < self->task_budget_usd)
        return TRUE;

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                "this task has spent $%.2f, which is its budget of $%.2f. "
                "Stop and report what you have.",
                *spent, self->task_budget_usd);

    return FALSE;
}

gboolean
clawt_loop_guard_check(ClawtLoopGuard  *self,
                       ClawtMessage    *message,
                       GError         **error)
{
    g_return_val_if_fail(CLAWT_IS_LOOP_GUARD(self), FALSE);
    g_return_val_if_fail(message != NULL, FALSE);

    /*
     * Ordered cheapest first, and the ones with no side effects before the
     * ones that record.  A message refused on hops must not have consumed
     * part of its sender's rate allowance.
     */
    if (!check_hops(self, message, error))
        return FALSE;

    if (!check_budget(self, message, error))
        return FALSE;

    /*
     * Rate before cycle, because the cycle check records as a side
     * effect.  A message refused on rate used to have its fingerprint
     * written into the cycle window anyway, so re-sending that same text
     * later -- legitimately, after the throttle cleared -- was flagged as
     * going in circles.
     */
    if (!check_rate(self, message, error))
        return FALSE;

    if (!check_cycle(self, message, error))
        return FALSE;

    return TRUE;
}

void
clawt_loop_guard_record_spend(ClawtLoopGuard *self,
                              const gchar    *task_id,
                              gdouble         amount_usd)
{
    gdouble *spent;

    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    if (task_id == NULL)
        return;

    spent = g_hash_table_lookup(self->task_spend, task_id);

    if (spent == NULL) {
        spent = g_new0(gdouble, 1);
        g_hash_table_insert(self->task_spend, g_strdup(task_id), spent);
    }

    *spent += amount_usd;
}

gdouble
clawt_loop_guard_get_task_spend(ClawtLoopGuard *self, const gchar *task_id)
{
    gdouble *spent;

    g_return_val_if_fail(CLAWT_IS_LOOP_GUARD(self), 0.0);

    if (task_id == NULL)
        return 0.0;

    spent = g_hash_table_lookup(self->task_spend, task_id);

    return (spent != NULL) ? *spent : 0.0;
}

void
clawt_loop_guard_forget_task(ClawtLoopGuard *self, const gchar *task_id)
{
    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    if (task_id != NULL)
        g_hash_table_remove(self->task_spend, task_id);
}

void
clawt_loop_guard_reset(ClawtLoopGuard *self)
{
    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    g_hash_table_remove_all(self->rates);
    g_hash_table_remove_all(self->recent);
    g_hash_table_remove_all(self->task_spend);
}

static void
clawt_loop_guard_finalize(GObject *object)
{
    ClawtLoopGuard *self = CLAWT_LOOP_GUARD(object);

    g_clear_pointer(&self->rates, g_hash_table_unref);
    g_clear_pointer(&self->recent, g_hash_table_unref);
    g_clear_pointer(&self->task_spend, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_loop_guard_parent_class)->finalize(object);
}

static void
clawt_loop_guard_class_init(ClawtLoopGuardClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_loop_guard_finalize;
}

static void
clawt_loop_guard_init(ClawtLoopGuard *self)
{
    self->max_hops = 8;
    self->rate_per_minute = 30;
    self->cycle_window = 10;
    self->task_budget_usd = 5.0;

    self->rates = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, rate_window_free);
    self->recent = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, fingerprint_queue_free);
    self->task_spend = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
}
