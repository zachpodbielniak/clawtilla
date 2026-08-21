/*
 * clawt-event.c - Something that happened in the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "core/clawt-event.h"

struct _ClawtEvent {
    gchar      *kind;
    gchar      *subject;
    gint64      timestamp;
    guint64     cursor;
    GHashTable *details;   /* gchar* -> gchar*, insertion order not kept */
    GPtrArray  *order;     /* keys in the order they were set */
};

G_DEFINE_BOXED_TYPE(ClawtEvent, clawt_event, clawt_event_copy,
                    clawt_event_free)

ClawtEvent *
clawt_event_new(const gchar *kind, const gchar *subject)
{
    ClawtEvent *self;

    g_return_val_if_fail(kind != NULL, NULL);

    self = g_new0(ClawtEvent, 1);
    self->kind = g_strdup(kind);
    self->subject = g_strdup(subject);
    self->timestamp = g_get_real_time();
    self->details = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_free);

    /*
     * Field order is kept alongside the table so a rendered event is
     * byte-stable.  An event log whose field order wanders is painful to
     * diff and impossible to golden-test.
     */
    self->order = g_ptr_array_new_with_free_func(g_free);

    return self;
}

ClawtEvent *
clawt_event_copy(ClawtEvent *self)
{
    ClawtEvent *copy;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_event_new(self->kind, self->subject);
    copy->timestamp = self->timestamp;
    copy->cursor = self->cursor;

    for (i = 0; i < self->order->len; i++) {
        const gchar *key = g_ptr_array_index(self->order, i);

        clawt_event_set_detail(copy, key,
                               g_hash_table_lookup(self->details, key));
    }

    return copy;
}

void
clawt_event_free(ClawtEvent *self)
{
    if (self == NULL)
        return;

    g_free(self->kind);
    g_free(self->subject);
    g_clear_pointer(&self->details, g_hash_table_unref);
    g_clear_pointer(&self->order, g_ptr_array_unref);
    g_free(self);
}

const gchar *
clawt_event_get_kind(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->kind;
}

const gchar *
clawt_event_get_subject(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->subject;
}

gint64
clawt_event_get_timestamp(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->timestamp;
}

void
clawt_event_set_timestamp(ClawtEvent *self, gint64 timestamp)
{
    g_return_if_fail(self != NULL);

    self->timestamp = timestamp;
}

guint64
clawt_event_get_cursor(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->cursor;
}

void
clawt_event_set_cursor(ClawtEvent *self, guint64 cursor)
{
    g_return_if_fail(self != NULL);

    self->cursor = cursor;
}

void
clawt_event_set_detail(ClawtEvent *self, const gchar *key, const gchar *value)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(key != NULL);

    if (value == NULL)
        return;

    if (!g_hash_table_contains(self->details, key))
        g_ptr_array_add(self->order, g_strdup(key));

    /*
     * Redacted on the way in, not on the way out.
     *
     * Events are teed to the event log and replayed into transcripts, so
     * a secret scrubbed only at display time is already on disk by then --
     * and stays there.
     */
    g_hash_table_replace(self->details, g_strdup(key),
                         clawt_redact_secrets(value));
}

void
clawt_event_set_detail_int(ClawtEvent *self, const gchar *key, gint64 value)
{
    g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, value);

    clawt_event_set_detail(self, key, text);
}

const gchar *
clawt_event_get_detail(ClawtEvent *self, const gchar *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    return g_hash_table_lookup(self->details, key);
}

JsonNode *
clawt_event_to_json(ClawtEvent *self)
{
    g_autoptr(JsonBuilder) builder = NULL;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "kind");
    json_builder_add_string_value(builder, self->kind);

    if (self->subject != NULL) {
        json_builder_set_member_name(builder, "subject");
        json_builder_add_string_value(builder, self->subject);
    }

    json_builder_set_member_name(builder, "ts");
    json_builder_add_int_value(builder, self->timestamp);

    if (self->cursor != 0) {
        json_builder_set_member_name(builder, "cursor");
        json_builder_add_int_value(builder, (gint64)self->cursor);
    }

    if (self->order->len > 0) {
        json_builder_set_member_name(builder, "detail");
        json_builder_begin_object(builder);

        for (i = 0; i < self->order->len; i++) {
            const gchar *key = g_ptr_array_index(self->order, i);

            json_builder_set_member_name(builder, key);
            json_builder_add_string_value(
                builder, g_hash_table_lookup(self->details, key));
        }

        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}
