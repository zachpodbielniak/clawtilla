/*
 * clawt-steer-queue.c - Talking to an agent that is already mid-turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-steer-queue.h"

typedef struct {
    gchar     *thread_id;
    gchar     *agent_id;
    GPtrArray *texts;      /* gchar*, in the order they were typed */
} SteerEntry;

struct _ClawtSteerQueue {
    GObject parent_instance;

    /*
     * An array rather than a hash table, because the order matters and a
     * hash table has none.  A drain has to take the *oldest* thread, and
     * there are never more than a handful: one per conversation somebody
     * is typing into while an agent works.
     */
    GPtrArray *entries;    /* SteerEntry* */
};

G_DEFINE_FINAL_TYPE(ClawtSteerQueue, clawt_steer_queue, G_TYPE_OBJECT)

static void
steer_entry_free(gpointer data)
{
    SteerEntry *entry = data;

    g_free(entry->thread_id);
    g_free(entry->agent_id);
    g_ptr_array_unref(entry->texts);
    g_free(entry);
}

ClawtSteerQueue *
clawt_steer_queue_new(void)
{
    return g_object_new(CLAWT_TYPE_STEER_QUEUE, NULL);
}

static SteerEntry *
find_thread(ClawtSteerQueue *self, const gchar *thread_id)
{
    guint i;

    for (i = 0; i < self->entries->len; i++) {
        SteerEntry *entry = g_ptr_array_index(self->entries, i);

        if (g_strcmp0(entry->thread_id, thread_id) == 0)
            return entry;
    }

    return NULL;
}

guint
clawt_steer_queue_add(ClawtSteerQueue *self,
                      const gchar     *thread_id,
                      const gchar     *agent_id,
                      const gchar     *text)
{
    SteerEntry *entry;

    g_return_val_if_fail(CLAWT_IS_STEER_QUEUE(self), 0);
    g_return_val_if_fail(thread_id != NULL, 0);
    g_return_val_if_fail(agent_id != NULL, 0);
    g_return_val_if_fail(text != NULL, 0);

    entry = find_thread(self, thread_id);

    if (entry == NULL) {
        entry = g_new0(SteerEntry, 1);
        entry->thread_id = g_strdup(thread_id);
        entry->agent_id = g_strdup(agent_id);
        entry->texts = g_ptr_array_new_with_free_func(g_free);

        g_ptr_array_add(self->entries, entry);
    }

    g_ptr_array_add(entry->texts, g_strdup(text));

    return entry->texts->len;
}

gchar *
clawt_steer_queue_drain(ClawtSteerQueue  *self,
                        const gchar      *agent_id,
                        gchar           **thread_id_out)
{
    SteerEntry *entry = NULL;
    g_autoptr(GString) joined = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_STEER_QUEUE(self), NULL);

    if (thread_id_out != NULL)
        *thread_id_out = NULL;

    if (agent_id == NULL)
        return NULL;

    for (i = 0; i < self->entries->len; i++) {
        SteerEntry *candidate = g_ptr_array_index(self->entries, i);

        if (g_strcmp0(candidate->agent_id, agent_id) != 0)
            continue;

        /*
         * Stolen out of the array before a single byte of it is read.
         *
         * Two settles can arrive together -- the link lowering its typing
         * indicator and the daemon's own interrupt both free the same
         * agent -- and building the text first would let both of them
         * find the entry and deliver the correction twice.  There is no
         * lock here and there does not need to be one: every caller is on
         * the daemon's main context, and removing first is what makes
         * that enough.
         */
        entry = candidate;
        g_ptr_array_steal_index(self->entries, i);
        break;
    }

    if (entry == NULL)
        return NULL;

    joined = g_string_new(NULL);

    for (i = 0; i < entry->texts->len; i++) {
        if (i > 0)
            g_string_append_c(joined, '\n');

        g_string_append(joined, g_ptr_array_index(entry->texts, i));
    }

    if (thread_id_out != NULL)
        *thread_id_out = g_strdup(entry->thread_id);

    steer_entry_free(entry);

    return g_strdup(joined->str);
}

guint
clawt_steer_queue_pending(ClawtSteerQueue *self, const gchar *agent_id)
{
    guint held = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_STEER_QUEUE(self), 0);

    for (i = 0; i < self->entries->len; i++) {
        SteerEntry *entry = g_ptr_array_index(self->entries, i);

        if (agent_id != NULL && g_strcmp0(entry->agent_id, agent_id) != 0)
            continue;

        held += entry->texts->len;
    }

    return held;
}

guint
clawt_steer_queue_pending_in_thread(ClawtSteerQueue *self,
                                    const gchar     *thread_id)
{
    SteerEntry *entry;

    g_return_val_if_fail(CLAWT_IS_STEER_QUEUE(self), 0);

    if (thread_id == NULL)
        return 0;

    entry = find_thread(self, thread_id);

    return (entry != NULL) ? entry->texts->len : 0;
}

void
clawt_steer_queue_forget_agent(ClawtSteerQueue *self, const gchar *agent_id)
{
    guint i;

    g_return_if_fail(CLAWT_IS_STEER_QUEUE(self));

    if (agent_id == NULL)
        return;

    for (i = self->entries->len; i > 0; i--) {
        SteerEntry *entry = g_ptr_array_index(self->entries, i - 1);

        if (g_strcmp0(entry->agent_id, agent_id) == 0)
            g_ptr_array_remove_index(self->entries, i - 1);
    }
}

void
clawt_steer_queue_reset(ClawtSteerQueue *self)
{
    g_return_if_fail(CLAWT_IS_STEER_QUEUE(self));

    g_ptr_array_set_size(self->entries, 0);
}

static void
clawt_steer_queue_finalize(GObject *object)
{
    ClawtSteerQueue *self = CLAWT_STEER_QUEUE(object);

    g_clear_pointer(&self->entries, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_steer_queue_parent_class)->finalize(object);
}

static void
clawt_steer_queue_class_init(ClawtSteerQueueClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_steer_queue_finalize;
}

static void
clawt_steer_queue_init(ClawtSteerQueue *self)
{
    self->entries = g_ptr_array_new_with_free_func(steer_entry_free);
}
