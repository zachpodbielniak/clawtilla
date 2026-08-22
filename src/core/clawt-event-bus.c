/*
 * clawt-event-bus.c - Where events go, and how a client catches up
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "core/clawt-event-bus.h"

struct _ClawtEventBus {
    GObject parent_instance;

    GPtrArray *history;      /* ClawtEvent*, oldest first */
    guint      capacity;
    guint64    cursor;
    guint64    oldest_held;  /* cursor of history[0], 0 when empty */
};

G_DEFINE_FINAL_TYPE(ClawtEventBus, clawt_event_bus, G_TYPE_OBJECT)

enum {
    SIGNAL_EVENT,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

ClawtEventBus *
clawt_event_bus_new(guint history)
{
    ClawtEventBus *self = g_object_new(CLAWT_TYPE_EVENT_BUS, NULL);

    /*
     * A bus with no history would answer every reconnect with "you have
     * missed something", which makes the resume path useless.
     */
    self->capacity = (history > 0) ? history : 1;

    return self;
}

guint64
clawt_event_bus_publish(ClawtEventBus *self, ClawtEvent *event)
{
    ClawtEvent *stored;

    g_return_val_if_fail(CLAWT_IS_EVENT_BUS(self), 0);
    g_return_val_if_fail(event != NULL, 0);

    self->cursor++;
    clawt_event_set_cursor(event, self->cursor);

    stored = clawt_event_copy(event);
    g_ptr_array_add(self->history, stored);

    if (self->oldest_held == 0)
        self->oldest_held = self->cursor;

    while (self->history->len > self->capacity) {
        ClawtEvent *dropped = g_ptr_array_index(self->history, 0);

        /*
         * Remembered before it is freed: a client resuming from a cursor
         * older than this must be told its replay is incomplete, and that
         * cannot be worked out from what is still held.
         */
        self->oldest_held = clawt_event_get_cursor(dropped) + 1;
        g_ptr_array_remove_index(self->history, 0);
    }

    g_signal_emit(self, signals[SIGNAL_EVENT], 0, event);

    return self->cursor;
}

guint64
clawt_event_bus_emit(ClawtEventBus *self, const gchar *kind,
                     const gchar *subject)
{
    g_autoptr(ClawtEvent) event = NULL;

    g_return_val_if_fail(CLAWT_IS_EVENT_BUS(self), 0);

    event = clawt_event_new(kind, subject);

    return clawt_event_bus_publish(self, event);
}

GPtrArray *
clawt_event_bus_replay(ClawtEventBus *self, guint64 cursor,
                       gboolean *out_complete)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_EVENT_BUS(self), NULL);

    out = g_ptr_array_new();

    if (out_complete != NULL) {
        /*
         * A cursor of 0 means "give me what you have", which is always
         * honoured in full.  Any other cursor is complete only if the next
         * event after it is still held.
         *
         * A cursor at or beyond the newest event has missed nothing by
         * definition -- including the absurd ones, since a client sending
         * a negative cursor arrives here as a very large unsigned number
         * and `cursor + 1` would otherwise wrap to zero.
         */
        *out_complete = (cursor == 0) ||
                        (self->oldest_held == 0) ||
                        (cursor >= self->cursor) ||
                        (cursor + 1 >= self->oldest_held);
    }

    for (i = 0; i < self->history->len; i++) {
        ClawtEvent *event = g_ptr_array_index(self->history, i);

        if (clawt_event_get_cursor(event) > cursor)
            g_ptr_array_add(out, event);
    }

    return out;
}

guint64
clawt_event_bus_get_cursor(ClawtEventBus *self)
{
    g_return_val_if_fail(CLAWT_IS_EVENT_BUS(self), 0);

    return self->cursor;
}

static void
clawt_event_bus_finalize(GObject *object)
{
    ClawtEventBus *self = CLAWT_EVENT_BUS(object);

    g_clear_pointer(&self->history, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_event_bus_parent_class)->finalize(object);
}

static void
clawt_event_bus_class_init(ClawtEventBusClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_event_bus_finalize;

    /**
     * ClawtEventBus::event:
     * @self: the bus
     * @event: the event
     *
     * Emitted for every published event.
     */
    signals[SIGNAL_EVENT] =
        g_signal_new("event", CLAWT_TYPE_EVENT_BUS, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 1, CLAWT_TYPE_EVENT);
}

static void
clawt_event_bus_init(ClawtEventBus *self)
{
    self->history = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_event_free);
    self->capacity = 1;
}
