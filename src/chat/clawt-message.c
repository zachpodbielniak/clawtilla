/*
 * clawt-message.c - One message in a room
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-message.h"

struct _ClawtMessage {
    gint ref_count;

    gchar *id;
    gchar *room_id;
    gchar *sender_id;
    gchar *sender_name;
    gchar *body;
    gchar *task_id;
    gchar *parent_id;

    gint64 timestamp;
    gint   depth;
};

static ClawtMessage *
clawt_message_ref(ClawtMessage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtMessage, clawt_message,
                    clawt_message_ref, clawt_message_free)

ClawtMessage *
clawt_message_new(const gchar *room_id,
                  const gchar *sender_id,
                  const gchar *body)
{
    ClawtMessage *self;

    g_return_val_if_fail(body != NULL, NULL);

    self = g_new0(ClawtMessage, 1);
    self->ref_count = 1;
    self->id = clawt_generate_id(NULL);
    self->room_id = g_strdup(room_id);
    self->sender_id = g_strdup(sender_id);
    self->body = g_strdup(body);
    self->timestamp = g_get_real_time() / G_USEC_PER_SEC;

    return self;
}

ClawtMessage *
clawt_message_copy(ClawtMessage *self)
{
    ClawtMessage *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtMessage, 1);
    copy->ref_count = 1;
    copy->id = g_strdup(self->id);
    copy->room_id = g_strdup(self->room_id);
    copy->sender_id = g_strdup(self->sender_id);
    copy->sender_name = g_strdup(self->sender_name);
    copy->body = g_strdup(self->body);
    copy->task_id = g_strdup(self->task_id);
    copy->parent_id = g_strdup(self->parent_id);
    copy->timestamp = self->timestamp;
    copy->depth = self->depth;

    return copy;
}

void
clawt_message_free(ClawtMessage *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self->room_id);
    g_free(self->sender_id);
    g_free(self->sender_name);
    g_free(self->body);
    g_free(self->task_id);
    g_free(self->parent_id);
    g_free(self);
}

#define GETTER(name, field)                                     \
    const gchar *                                               \
    clawt_message_get_##name(ClawtMessage *self)                \
    {                                                           \
        g_return_val_if_fail(self != NULL, NULL);               \
        return self->field;                                     \
    }

GETTER(id, id)
GETTER(room_id, room_id)
GETTER(sender_id, sender_id)
GETTER(sender_name, sender_name)
GETTER(body, body)
GETTER(task_id, task_id)
GETTER(parent_id, parent_id)

#undef GETTER

#define SETTER(name, field)                                     \
    void                                                        \
    clawt_message_set_##name(ClawtMessage *self,                \
                             const gchar  *value)               \
    {                                                           \
        g_return_if_fail(self != NULL);                         \
        g_free(self->field);                                    \
        self->field = g_strdup(value);                          \
    }

SETTER(id, id)
SETTER(sender_name, sender_name)
SETTER(task_id, task_id)
SETTER(parent_id, parent_id)

#undef SETTER

gint64
clawt_message_get_timestamp(ClawtMessage *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->timestamp;
}

gint
clawt_message_get_depth(ClawtMessage *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->depth;
}

void
clawt_message_set_timestamp(ClawtMessage *self, gint64 timestamp)
{
    g_return_if_fail(self != NULL);
    self->timestamp = timestamp;
}

void
clawt_message_set_depth(ClawtMessage *self, gint depth)
{
    g_return_if_fail(self != NULL);
    self->depth = depth;
}

gchar *
clawt_message_body_fingerprint(ClawtMessage *self)
{
    g_autofree gchar *combined = NULL;

    g_return_val_if_fail(self != NULL, NULL);

    /*
     * Sender and room are part of it, not just the body.  Two agents both
     * saying "on it" is normal; one agent saying "on it" in the same room
     * for the fourth time is a loop.
     */
    combined = g_strdup_printf("%s\x1f%s\x1f%s",
                               self->sender_id != NULL ? self->sender_id : "",
                               self->room_id != NULL ? self->room_id : "",
                               self->body != NULL ? self->body : "");

    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, combined, -1);
}

gboolean
clawt_unread_should_count(const gchar *room_id, const gchar *viewing_room,
                          const gchar *from, gint64 event_ts,
                          gint64 connected_at)
{
    if (room_id == NULL || from == NULL)
        return FALSE;

    /* The operator's own turn. */
    if (g_strcmp0(from, "user") == 0)
        return FALSE;

    /* The conversation on screen; the transcript's own rule has that. */
    if (viewing_room != NULL && g_strcmp0(room_id, viewing_room) == 0)
        return FALSE;

    /*
     * Replayed rather than new.  Replayed events keep their original
     * timestamps, which is the whole of the test.
     */
    if (event_ts > 0 && event_ts < connected_at)
        return FALSE;

    return TRUE;
}
