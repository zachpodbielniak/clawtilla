/*
 * clawt-repeat-watch.c - Noticing that an agent keeps making the same call
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-repeat-watch.h"

#define DEFAULT_MAX_KEYS (256)

enum {
    SIGNAL_THRESHOLD,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/*
 * One distinct call, and where it sits in the recency order.
 *
 * The link is kept so that seeing a call again is O(1): without it,
 * moving an entry to the tail means walking the queue, which for a
 * pathological turn is the thing this table was bounded to avoid.
 */
typedef struct {
    gchar  *key;
    guint   count;
    GList  *link;      /* this slot's node in TurnTable.order */
} Slot;

typedef struct {
    GHashTable *slots;  /* key -> Slot*, owning */
    GQueue     *order;  /* Slot*, least recently seen at the head */
} TurnTable;

struct _ClawtRepeatWatch {
    GObject parent_instance;

    GArray     *thresholds;   /* guint, ascending */
    guint       max_keys;
    GHashTable *turns;        /* turn id -> TurnTable* */
};

G_DEFINE_FINAL_TYPE(ClawtRepeatWatch, clawt_repeat_watch, G_TYPE_OBJECT)

static void
slot_free(gpointer data)
{
    Slot *slot = data;

    g_free(slot->key);
    g_free(slot);
}

static void
turn_table_free(gpointer data)
{
    TurnTable *table = data;

    /*
     * The queue holds the same pointers the hash table owns, so it is
     * freed without a free func.  Freeing both would be a double free,
     * and freeing neither leaks every slot of every finished turn.
     */
    g_queue_free(table->order);
    g_hash_table_unref(table->slots);
    g_free(table);
}

static TurnTable *
turn_table_new(void)
{
    TurnTable *table = g_new0(TurnTable, 1);

    table->slots = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         NULL, slot_free);
    table->order = g_queue_new();

    return table;
}

ClawtRepeatWatch *
clawt_repeat_watch_new(void)
{
    return g_object_new(CLAWT_TYPE_REPEAT_WATCH, NULL);
}

static gint
compare_guint(gconstpointer a, gconstpointer b)
{
    guint left = *(const guint *)a;
    guint right = *(const guint *)b;

    if (left < right)
        return -1;

    return (left > right) ? 1 : 0;
}

void
clawt_repeat_watch_set_thresholds(ClawtRepeatWatch *self, const gchar *csv)
{
    g_auto(GStrv) parts = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_REPEAT_WATCH(self));

    g_array_set_size(self->thresholds, 0);

    if (csv == NULL || *csv == '\0')
        return;

    parts = g_strsplit(csv, ",", -1);

    for (i = 0; parts[i] != NULL; i++) {
        g_autofree gchar *trimmed = g_strdup(parts[i]);
        gchar *end = NULL;
        guint64 value;

        g_strstrip(trimmed);

        if (*trimmed == '\0')
            continue;

        value = g_ascii_strtoull(trimmed, &end, 10);

        /*
         * Dropped with a warning rather than taken as zero.  A threshold
         * of zero fires on the count before the first call, so a typo
         * would turn every tool call in the fleet into a report.
         */
        if (end == NULL || *end != '\0' || value == 0 || value > G_MAXUINT) {
            g_warning("orchestration.repeat_thresholds: '%s' is not a "
                      "positive whole number; ignoring it", trimmed);
            continue;
        }

        {
            guint threshold = (guint)value;

            g_array_append_val(self->thresholds, threshold);
        }
    }

    /*
     * Sorted, so "the highest" is the last one whatever order somebody
     * wrote them in -- the escalation point must not depend on the order
     * of a config value a person types by hand.
     */
    g_array_sort(self->thresholds, compare_guint);
}

void
clawt_repeat_watch_set_max_keys(ClawtRepeatWatch *self, guint max_keys)
{
    g_return_if_fail(CLAWT_IS_REPEAT_WATCH(self));

    self->max_keys = (max_keys > 0) ? max_keys : DEFAULT_MAX_KEYS;
}

guint
clawt_repeat_watch_get_highest_threshold(ClawtRepeatWatch *self)
{
    g_return_val_if_fail(CLAWT_IS_REPEAT_WATCH(self), 0);

    if (self->thresholds->len == 0)
        return 0;

    return g_array_index(self->thresholds, guint, self->thresholds->len - 1);
}

gchar *
clawt_repeat_key(const gchar *tool, const gchar *args)
{
    g_autoptr(GString) out = NULL;
    const gchar *p;
    gboolean in_space = FALSE;
    gboolean any = FALSE;

    if (tool == NULL || *tool == '\0')
        return NULL;

    /*
     * A bare tool name is not a call worth counting.
     *
     * "bash" five times may be five different commands, and a report
     * that says otherwise is a false positive -- which is worse than
     * missing the loop, because it is what teaches somebody to stop
     * reading these.
     */
    if (args == NULL)
        return NULL;

    out = g_string_new(NULL);

    for (p = args; *p != '\0'; p++) {
        if (g_ascii_isspace(*p)) {
            in_space = TRUE;
            continue;
        }

        if (in_space && any)
            g_string_append_c(out, ' ');

        in_space = FALSE;
        any = TRUE;
        g_string_append_c(out, *p);
    }

    if (!any)
        return NULL;

    return g_strdup_printf("%s:%s", tool, out->str);
}

static gboolean
lands_on_threshold(ClawtRepeatWatch *self, guint count)
{
    guint i;

    for (i = 0; i < self->thresholds->len; i++) {
        if (g_array_index(self->thresholds, guint, i) == count)
            return TRUE;
    }

    return FALSE;
}

guint
clawt_repeat_watch_note(ClawtRepeatWatch *self,
                        const gchar      *turn_id,
                        const gchar      *tool,
                        const gchar      *args)
{
    g_autofree gchar *key = NULL;
    g_autofree gchar *reported = NULL;
    TurnTable *table;
    Slot *slot;
    guint count;

    g_return_val_if_fail(CLAWT_IS_REPEAT_WATCH(self), 0);

    if (turn_id == NULL)
        return 0;

    key = clawt_repeat_key(tool, args);

    if (key == NULL)
        return 0;

    table = g_hash_table_lookup(self->turns, turn_id);

    if (table == NULL) {
        table = turn_table_new();
        g_hash_table_insert(self->turns, g_strdup(turn_id), table);
    }

    slot = g_hash_table_lookup(table->slots, key);

    if (slot != NULL) {
        slot->count++;
        g_queue_unlink(table->order, slot->link);
        g_queue_push_tail_link(table->order, slot->link);
    } else {
        slot = g_new0(Slot, 1);
        slot->key = g_steal_pointer(&key);
        slot->count = 1;

        g_hash_table_insert(table->slots, slot->key, slot);
        g_queue_push_tail(table->order, slot);
        slot->link = table->order->tail;
    }

    /*
     * Read out before anything is evicted.  The slot this call is about
     * cannot be the one dropped -- it has just been moved to the tail
     * and eviction takes from the head -- but reading it afterwards
     * would make that a fact about the eviction loop rather than about
     * this function, and the next edit to either would decide it again.
     */
    count = slot->count;
    reported = g_strdup(slot->key);

    /*
     * Evicted least-recently-seen first, and the eviction is the reason
     * a dropped call restarts at one: this table is a bound on memory,
     * not a record of the turn.
     */
    while (g_queue_get_length(table->order) > self->max_keys) {
        Slot *oldest = g_queue_pop_head(table->order);

        g_hash_table_remove(table->slots, oldest->key);
    }

    if (!lands_on_threshold(self, count))
        return 0;

    g_signal_emit(self, signals[SIGNAL_THRESHOLD], 0, turn_id, reported,
                  count);

    return count;
}

guint
clawt_repeat_watch_count(ClawtRepeatWatch *self,
                         const gchar      *turn_id,
                         const gchar      *tool,
                         const gchar      *args)
{
    g_autofree gchar *key = NULL;
    TurnTable *table;
    Slot *slot;

    g_return_val_if_fail(CLAWT_IS_REPEAT_WATCH(self), 0);

    if (turn_id == NULL)
        return 0;

    key = clawt_repeat_key(tool, args);

    if (key == NULL)
        return 0;

    table = g_hash_table_lookup(self->turns, turn_id);

    if (table == NULL)
        return 0;

    slot = g_hash_table_lookup(table->slots, key);

    return (slot != NULL) ? slot->count : 0;
}

void
clawt_repeat_watch_end_turn(ClawtRepeatWatch *self, const gchar *turn_id)
{
    g_return_if_fail(CLAWT_IS_REPEAT_WATCH(self));

    if (turn_id != NULL)
        g_hash_table_remove(self->turns, turn_id);
}

void
clawt_repeat_watch_reset(ClawtRepeatWatch *self)
{
    g_return_if_fail(CLAWT_IS_REPEAT_WATCH(self));

    g_hash_table_remove_all(self->turns);
}

static void
clawt_repeat_watch_finalize(GObject *object)
{
    ClawtRepeatWatch *self = CLAWT_REPEAT_WATCH(object);

    g_clear_pointer(&self->turns, g_hash_table_unref);
    g_clear_pointer(&self->thresholds, g_array_unref);

    G_OBJECT_CLASS(clawt_repeat_watch_parent_class)->finalize(object);
}

static void
clawt_repeat_watch_class_init(ClawtRepeatWatchClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_repeat_watch_finalize;

    /**
     * ClawtRepeatWatch::threshold:
     * @self: the watch
     * @turn_id: the turn making the calls
     * @key: the repeated call, as clawt_repeat_key() spells it
     * @count: how many times, always exactly a configured threshold
     *
     * A signal rather than a callback, because what to do about it is
     * the daemon's business and this object must stay testable without
     * one.
     */
    signals[SIGNAL_THRESHOLD] =
        g_signal_new("threshold", CLAWT_TYPE_REPEAT_WATCH, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
}

static void
clawt_repeat_watch_init(ClawtRepeatWatch *self)
{
    self->thresholds = g_array_new(FALSE, FALSE, sizeof(guint));
    self->max_keys = DEFAULT_MAX_KEYS;
    self->turns = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, turn_table_free);

    clawt_repeat_watch_set_thresholds(self, "5,10,20");
}
