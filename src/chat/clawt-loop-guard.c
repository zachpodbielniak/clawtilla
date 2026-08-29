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

/*
 * One remembered message, and when it was seen.
 *
 * The timestamp is what makes "recently" in the refusal true.  Without
 * it the queue was trimmed by count alone, so how far back the check
 * looked was however long the room's last cycle_window messages had
 * taken -- ten messages in a quiet room is hours, and an agent
 * repeating one error string was silenced for all of them.  check_rate()
 * below has kept a timestamp per entry since it was written; this is the
 * same idea, arrived at from the other end.
 */
typedef struct {
    gchar  *fingerprint;
    gchar  *near;         /* the same, with every run of digits collapsed */
    gint64  at;           /* monotonic microseconds */
} CycleEntry;

static void
cycle_entry_free(gpointer data)
{
    CycleEntry *entry = data;

    g_free(entry->fingerprint);
    g_free(entry->near);
    g_free(entry);
}

/*
 * The same message with the numbers taken out.
 *
 * Two agents can loop while looking different every time: "finished run
 * 41", "finished run 42", "finished run 43".  The exact fingerprint sees
 * three distinct messages and the loop runs for ever, which is the case
 * an exact-match detector is blind to by construction.
 *
 * Every run of digits collapses to one `#`, so those three are one
 * message.  Numbers rather than words, because a number is the thing
 * that varies in a machine-generated line and a word is the thing that
 * varies in a person's.
 *
 * Two things keep this from silencing real work, which is the failure
 * that matters most here: a false positive stops an agent doing its job,
 * and unlike a loop it costs nothing visible and nobody notices.
 *
 *   - It takes %NEAR_REPEAT_LIMIT of them, not two.  "step 1 done",
 *     "step 2 done", "step 3 done" is progress through a short task and
 *     has to get through; counting to forty is not.
 *   - It never stalls a room.  A near match is a refusal the sender can
 *     wait out; only an exact repeat ends the exchange, because being
 *     wrong about a stall means somebody has to come and restart the
 *     conversation, and "deploy 3 servers" is a near match that is not a
 *     loop at all.
 */
/*
 * How many near-identical messages the window tolerates.
 *
 * Five rather than two, and it is the difference between catching a
 * counting loop and refusing an agent that is reporting progress through
 * a handful of steps.  A loop produces dozens; a task with four steps in
 * it produces four.
 */
#define NEAR_REPEAT_LIMIT (5)

static gchar *
near_fingerprint(ClawtMessage *message)
{
    const gchar *body = clawt_message_get_body(message);
    g_autoptr(GString) collapsed = NULL;
    g_autofree gchar *combined = NULL;
    const gchar *p;
    gboolean in_digits = FALSE;

    collapsed = g_string_new(NULL);

    for (p = (body != NULL) ? body : ""; *p != '\0'; p++) {
        if (g_ascii_isdigit(*p)) {
            if (!in_digits)
                g_string_append_c(collapsed, '#');

            in_digits = TRUE;
            continue;
        }

        in_digits = FALSE;
        g_string_append_c(collapsed, *p);
    }

    combined = g_strdup_printf(
        "%s\x1f%s\x1f%s",
        clawt_message_get_sender_id(message) != NULL
            ? clawt_message_get_sender_id(message) : "",
        clawt_message_get_room_id(message) != NULL
            ? clawt_message_get_room_id(message) : "",
        collapsed->str);

    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, combined, -1);
}

/*
 * An exchange this guard has ended.
 *
 * Held per room rather than per pair, because a room is what the mailbox
 * router resolves a message into and is therefore the one identifier
 * every send already has.
 */
typedef struct {
    ClawtStallReason reason;
    gchar           *detail;
} Stall;

static void
stall_free(gpointer data)
{
    Stall *stall = data;

    g_free(stall->detail);
    g_free(stall);
}

enum {
    SIGNAL_STALLED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtLoopGuard {
    GObject parent_instance;

    guint   max_hops;
    guint   rate_per_minute;
    guint   cycle_window;
    guint   cycle_seconds;
    gdouble task_budget_usd;

    GHashTable *rates;         /* agent_id -> RateWindow */
    GHashTable *recent;        /* room_id -> GQueue of CycleEntry, oldest first */
    GHashTable *task_spend;    /* task_id -> gdouble* */
    GHashTable *stalls;        /* room_id -> Stall* */

    ClawtLoopGuardPeerFunc peer_func;
    gpointer               peer_data;
    GDestroyNotify         peer_notify;
};

G_DEFINE_FINAL_TYPE(ClawtLoopGuard, clawt_loop_guard, G_TYPE_OBJECT)

static gboolean
sender_is_peer(ClawtLoopGuard *self, ClawtMessage *message)
{
    if (self->peer_func == NULL)
        return FALSE;

    return self->peer_func(clawt_message_get_sender_id(message),
                           self->peer_data);
}

void
clawt_loop_guard_set_peer_func(ClawtLoopGuard         *self,
                               ClawtLoopGuardPeerFunc  func,
                               gpointer                user_data,
                               GDestroyNotify          notify)
{
    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    if (self->peer_notify != NULL && self->peer_data != NULL)
        self->peer_notify(self->peer_data);

    self->peer_func = func;
    self->peer_data = user_data;
    self->peer_notify = notify;
}

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
    g_queue_free_full(data, cycle_entry_free);
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
clawt_loop_guard_set_cycle_seconds(ClawtLoopGuard *self, guint seconds)
{
    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    self->cycle_seconds = seconds;
}

void
clawt_loop_guard_set_task_budget(ClawtLoopGuard *self, gdouble budget_usd)
{
    g_return_if_fail(CLAWT_IS_LOOP_GUARD(self));

    self->task_budget_usd = budget_usd;
}

static gboolean
check_hops(ClawtLoopGuard *self, ClawtMessage *message, guint limit,
           GError **error)
{
    gint depth = clawt_message_get_depth(message);

    /*
     * The room's own limit if it declared one, the fleet's otherwise.
     * Zero means "not set" in both, and in the fleet's case also means
     * no limit -- which is why a room of 0 has to fall through to the
     * fleet value rather than being taken as unlimited.
     */
    if (limit == 0)
        limit = self->max_hops;

    if (limit == 0 || depth < (gint)limit)
        return TRUE;

    /*
     * The message names the limit it hit, so the agent can say something
     * useful rather than reporting an unexplained failure -- and the
     * advice has to match what the agent was actually doing.
     *
     * "Answer directly rather than passing it on again" is right for a
     * delegation chain and useless for a *conversation*: three agents
     * in a room each reply one hop deeper, so an ordinary standup
     * reaches the ceiling on its own, and every one of them was already
     * answering directly.  An agent told to do the thing it is doing
     * has nothing to act on, which is how a refusal gets retried in a
     * different shape.
     *
     * A room id is the discriminator, because that is the one thing
     * that distinguishes broadcasting from handing work along.
     */
    if (clawt_message_get_room_id(message) != NULL &&
        !g_str_has_prefix(clawt_message_get_room_id(message), "dm:")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "this message is %d hops from the original request, "
                    "and the limit is %u. This is a room, so every reply "
                    "counts as a hop and a long exchange reaches the "
                    "ceiling on its own -- it does not mean anybody did "
                    "anything wrong. Say what you have concluded to the "
                    "person who asked, or raise this room's "
                    "rooms.max_hops -- or orchestration.max_hops -- if "
                    "conversations of this length are wanted.",
                    depth, limit);

        return FALSE;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                "this message is %d hops from the original request, and the "
                "limit is %u. Delegation has gone deeper than intended; "
                "answer directly rather than passing it on again.",
                depth, limit);

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
    g_autofree gchar *near = NULL;
    GQueue *history;
    GList *l;
    gint64 now;
    gint64 cutoff;
    guint near_seen;

    if (self->cycle_window == 0 || self->cycle_seconds == 0 || room == NULL)
        return TRUE;

    now = g_get_monotonic_time();
    cutoff = now - (gint64)G_USEC_PER_SEC * (gint64)self->cycle_seconds;

    fingerprint = clawt_message_body_fingerprint(message);
    near = near_fingerprint(message);

    history = g_hash_table_lookup(self->recent, room);
    if (history == NULL) {
        history = g_queue_new();
        g_hash_table_insert(self->recent, g_strdup(room), history);
    }

    /*
     * Anything older than the window is not evidence of a loop, so it
     * goes before the search rather than being skipped inside it: a
     * queue nothing ever drops from is also a leak in a room that stays
     * below its count bound.  Oldest first, so this stops at the first
     * entry still inside.
     */
    while (!g_queue_is_empty(history)) {
        CycleEntry *oldest = g_queue_peek_head(history);

        if (oldest->at >= cutoff)
            break;

        cycle_entry_free(g_queue_pop_head(history));
    }

    /*
     * The near matches are counted on the way past, so the exact match
     * below still short-circuits: an exact repeat is the cheaper and the
     * more certain finding, and it is the one that ends the exchange.
     */
    near_seen = 0;

    for (l = history->head; l != NULL; l = l->next) {
        CycleEntry *entry = l->data;

        if (g_strcmp0(entry->fingerprint, fingerprint) != 0) {
            if (g_strcmp0(entry->near, near) != 0)
                continue;

            if (++near_seen < NEAR_REPEAT_LIMIT)
                continue;

            /*
             * The same message with different numbers in it, enough
             * times that counting is all that is happening.  Refused,
             * and the exchange is deliberately *not* ended -- see
             * near_fingerprint().
             */
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                        "this is the %d'th message to '%s' in the last %u "
                        "seconds that differs from the one before it only "
                        "in the numbers in it. Counting up is not "
                        "progress; say what has actually changed, or stop "
                        "and report.",
                        NEAR_REPEAT_LIMIT + 1, room, self->cycle_seconds);

            return FALSE;
        }

        /*
         * This is the case the hop limit misses entirely: two agents
         * alternating the same two replies, each message a fresh chain with
         * a depth of one, for ever.
         *
         * The duration is named because the previous wording -- "already
         * been sent recently" -- was not something the reader could act
         * on, and because it was not true: there was no clock in this
         * function at all, so "recently" meant "within the last
         * cycle_window messages", which in a quiet room was hours. A
         * refusal that cannot be waited out is a refusal that gets
         * retried.
         */
        /*
         * And this is where an exchange between agents *ends* rather
         * than being refused one more time.
         *
         * The refusal on its own was advice, and the agent it was given
         * to had just demonstrated that it produces this same text --
         * so the next turn produced it again, and was refused again, and
         * every one of those turns had already been paid for.  A pair
         * that will not stop has to be stopped.
         *
         * Only between agents.  A person repeating themselves is a
         * person, and clawtilla does not end their conversation.
         */
        if (sender_is_peer(self, message)) {
            clawt_loop_guard_stall_room(self, room,
                                        CLAWT_STALL_REPEATED_MESSAGE,
                                        clawt_message_get_body(message));

            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                        "this exact message has already gone to '%s' "
                        "within the last %u seconds, so the exchange has "
                        "been ended rather than refused again -- each "
                        "refusal was costing a turn. Report what you have "
                        "to whoever asked for it; a person restarting the "
                        "conversation in '%s' is what reopens it.",
                        room, self->cycle_seconds, room);

            return FALSE;
        }

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "this exact message has already been sent to '%s' "
                    "within the last %u seconds. The conversation is going "
                    "in circles; say something different, or wait for the "
                    "window to pass if the repetition is genuinely what you "
                    "mean.", room, self->cycle_seconds);
        return FALSE;
    }

    {
        CycleEntry *entry = g_new0(CycleEntry, 1);

        entry->fingerprint = g_steal_pointer(&fingerprint);
        entry->near = g_steal_pointer(&near);
        entry->at = now;
        g_queue_push_tail(history, entry);
    }

    /*
     * And the count bound stays, now purely as a memory bound: a busy
     * room must not accumulate a fingerprint per message for the whole
     * duration of the window.
     */
    while (g_queue_get_length(history) > self->cycle_window)
        cycle_entry_free(g_queue_pop_head(history));

    return TRUE;
}

gboolean
clawt_loop_guard_stall_room(ClawtLoopGuard   *self,
                            const gchar      *room_id,
                            ClawtStallReason  reason,
                            const gchar      *detail)
{
    Stall *stall;

    g_return_val_if_fail(CLAWT_IS_LOOP_GUARD(self), FALSE);

    if (room_id == NULL || reason == CLAWT_STALL_NONE)
        return FALSE;

    /*
     * The first reason is kept.  A room that stalled on a repeated
     * message and then hit the room budget while nobody was reading is
     * still a room that stalled on a repeated message, and overwriting
     * would leave the alert naming the consequence rather than the cause.
     */
    if (g_hash_table_lookup(self->stalls, room_id) != NULL)
        return FALSE;

    stall = g_new0(Stall, 1);
    stall->reason = reason;
    stall->detail = g_strdup(detail);

    g_hash_table_insert(self->stalls, g_strdup(room_id), stall);

    g_signal_emit(self, signals[SIGNAL_STALLED], 0, room_id, (guint)reason,
                  detail);

    return TRUE;
}

ClawtStallReason
clawt_loop_guard_get_stall_reason(ClawtLoopGuard *self, const gchar *room_id)
{
    Stall *stall;

    g_return_val_if_fail(CLAWT_IS_LOOP_GUARD(self), CLAWT_STALL_NONE);

    if (room_id == NULL)
        return CLAWT_STALL_NONE;

    stall = g_hash_table_lookup(self->stalls, room_id);

    return (stall != NULL) ? stall->reason : CLAWT_STALL_NONE;
}

const gchar *
clawt_loop_guard_get_stall_detail(ClawtLoopGuard *self, const gchar *room_id)
{
    Stall *stall;

    g_return_val_if_fail(CLAWT_IS_LOOP_GUARD(self), NULL);

    if (room_id == NULL)
        return NULL;

    stall = g_hash_table_lookup(self->stalls, room_id);

    return (stall != NULL) ? stall->detail : NULL;
}

gboolean
clawt_loop_guard_clear_stall(ClawtLoopGuard *self, const gchar *room_id)
{
    g_return_val_if_fail(CLAWT_IS_LOOP_GUARD(self), FALSE);

    if (room_id == NULL)
        return FALSE;

    /*
     * The fingerprints go too.  Leaving them would have the first
     * message after a person restarts the conversation matched against
     * whatever was going round before it, so restarting an exchange
     * would refuse the restart.
     */
    g_hash_table_remove(self->recent, room_id);

    return g_hash_table_remove(self->stalls, room_id);
}

/*
 * An exchange that has already been ended.
 *
 * First in the chain and by a long way the cheapest: it is the whole
 * point of stalling that the next message costs nothing.  A refusal from
 * one of the other four still arrives after the sender has taken a turn
 * to write the message, so a pair that will not stop is billed for every
 * refusal.
 *
 * A person writing into the room clears it.  Anything else would leave a
 * room permanently dead with no way back, and the person is precisely
 * who the stall was raised for.
 */
static gboolean
check_stall(ClawtLoopGuard *self, ClawtMessage *message, GError **error)
{
    const gchar *room = clawt_message_get_room_id(message);
    Stall *stall;

    if (room == NULL)
        return TRUE;

    stall = g_hash_table_lookup(self->stalls, room);

    if (stall == NULL)
        return TRUE;

    if (!sender_is_peer(self, message)) {
        clawt_loop_guard_clear_stall(self, room);
        return TRUE;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                "this exchange was ended because it stopped making "
                "progress, and it stays ended until a person says "
                "something in '%s'. Do not try again; report what you "
                "have to whoever asked for it.", room);

    return FALSE;
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
    return clawt_loop_guard_check_in_room(self, message, 0, error);
}

gboolean
clawt_loop_guard_check_in_room(ClawtLoopGuard  *self,
                               ClawtMessage    *message,
                               guint            room_max_hops,
                               GError         **error)
{
    g_return_val_if_fail(CLAWT_IS_LOOP_GUARD(self), FALSE);
    g_return_val_if_fail(message != NULL, FALSE);

    /*
     * Ordered cheapest first, and the ones with no side effects before the
     * ones that record.  A message refused on hops must not have consumed
     * part of its sender's rate allowance.
     *
     * The stall check is ahead of all four because it is the only one
     * that can be true of a room rather than of a message: once an
     * exchange has been ended there is nothing left to measure.
     */
    if (!check_stall(self, message, error))
        return FALSE;

    if (!check_hops(self, message, room_max_hops, error))
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
    g_hash_table_remove_all(self->stalls);
}

static void
clawt_loop_guard_finalize(GObject *object)
{
    ClawtLoopGuard *self = CLAWT_LOOP_GUARD(object);

    if (self->peer_notify != NULL && self->peer_data != NULL)
        self->peer_notify(self->peer_data);

    g_clear_pointer(&self->rates, g_hash_table_unref);
    g_clear_pointer(&self->recent, g_hash_table_unref);
    g_clear_pointer(&self->task_spend, g_hash_table_unref);
    g_clear_pointer(&self->stalls, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_loop_guard_parent_class)->finalize(object);
}

static void
clawt_loop_guard_class_init(ClawtLoopGuardClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_loop_guard_finalize;

    /**
     * ClawtLoopGuard::stalled:
     * @self: the guard
     * @room_id: the exchange that was ended
     * @reason: a #ClawtStallReason, as a guint
     * @detail: (nullable): what was repeating
     *
     * A signal rather than a callback: what to do about a stall -- move
     * the task, raise the alert, tell both clients -- belongs to the
     * daemon, and this object has to stay testable without one.
     */
    signals[SIGNAL_STALLED] =
        g_signal_new("stalled", CLAWT_TYPE_LOOP_GUARD, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                     G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING);
}

static void
clawt_loop_guard_init(ClawtLoopGuard *self)
{
    self->max_hops = 8;
    self->rate_per_minute = 30;
    self->cycle_window = 10;
    self->cycle_seconds = 300;
    self->task_budget_usd = 5.0;

    self->rates = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, rate_window_free);
    self->recent = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, fingerprint_queue_free);
    self->task_spend = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    self->stalls = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, stall_free);
}
