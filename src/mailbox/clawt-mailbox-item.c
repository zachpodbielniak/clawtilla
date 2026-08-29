/*
 * clawt-mailbox-item.c - One queued message
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mailbox/clawt-mailbox-item.h"

struct _ClawtMailboxItem {
    gint ref_count;

    gchar *id;
    gchar *from;
    gchar *to;
    gchar *body;
    gchar *room;
    gchar *task_id;
    gchar *reply_to;
    gchar *subject;
    gchar *idempotency_key;
    gchar *last_error;

    ClawtPriority     priority;
    ClawtMailboxState state;

    gint     depth;
    gint     attempts;
    gboolean invites_reply;
    gint64   created_at;
    gint64   not_before;
    gint64   expires_at;
};

static ClawtMailboxItem *
clawt_mailbox_item_ref(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtMailboxItem, clawt_mailbox_item,
                    clawt_mailbox_item_ref, clawt_mailbox_item_free)

ClawtMailboxItem *
clawt_mailbox_item_new(const gchar *from, const gchar *to, const gchar *body)
{
    ClawtMailboxItem *self;

    g_return_val_if_fail(body != NULL, NULL);

    self = g_new0(ClawtMailboxItem, 1);
    self->ref_count = 1;

    /*
     * The id sorts by creation time, so "next item" is an ORDER BY rather
     * than a separate sequence column and an index to go with it.
     */
    self->id = clawt_generate_id(NULL);
    self->from = g_strdup(from);
    self->to = g_strdup(to);
    self->body = g_strdup(body);

    self->priority = CLAWT_PRIORITY_NORMAL;
    self->state = CLAWT_MAILBOX_PENDING;
    self->invites_reply = TRUE;
    self->created_at = g_get_real_time() / G_USEC_PER_SEC;

    return self;
}

ClawtMailboxItem *
clawt_mailbox_item_copy(ClawtMailboxItem *self)
{
    ClawtMailboxItem *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtMailboxItem, 1);
    copy->ref_count = 1;

    copy->id = g_strdup(self->id);
    copy->from = g_strdup(self->from);
    copy->to = g_strdup(self->to);
    copy->body = g_strdup(self->body);
    copy->room = g_strdup(self->room);
    copy->task_id = g_strdup(self->task_id);
    copy->reply_to = g_strdup(self->reply_to);
    copy->subject = g_strdup(self->subject);
    copy->idempotency_key = g_strdup(self->idempotency_key);
    copy->last_error = g_strdup(self->last_error);

    copy->priority = self->priority;
    copy->state = self->state;
    copy->depth = self->depth;
    copy->attempts = self->attempts;
    copy->invites_reply = self->invites_reply;
    copy->created_at = self->created_at;
    copy->not_before = self->not_before;
    copy->expires_at = self->expires_at;

    return copy;
}

void
clawt_mailbox_item_free(ClawtMailboxItem *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self->from);
    g_free(self->to);
    g_free(self->body);
    g_free(self->room);
    g_free(self->task_id);
    g_free(self->reply_to);
    g_free(self->subject);
    g_free(self->idempotency_key);
    g_free(self->last_error);
    g_free(self);
}

#define GETTER_STR(name, field)                                     \
    const gchar *                                                   \
    clawt_mailbox_item_get_##name(ClawtMailboxItem *self)           \
    {                                                               \
        g_return_val_if_fail(self != NULL, NULL);                   \
        return self->field;                                         \
    }

GETTER_STR(id, id)
GETTER_STR(from, from)
GETTER_STR(to, to)
GETTER_STR(body, body)
GETTER_STR(room, room)
GETTER_STR(task_id, task_id)
GETTER_STR(reply_to, reply_to)
GETTER_STR(subject, subject)
GETTER_STR(idempotency_key, idempotency_key)
GETTER_STR(last_error, last_error)

#undef GETTER_STR

#define SETTER_STR(name, field)                                     \
    void                                                            \
    clawt_mailbox_item_set_##name(ClawtMailboxItem *self,           \
                                  const gchar      *value)          \
    {                                                               \
        g_return_if_fail(self != NULL);                             \
        g_free(self->field);                                        \
        self->field = g_strdup(value);                              \
    }

SETTER_STR(id, id)
SETTER_STR(room, room)
SETTER_STR(task_id, task_id)
SETTER_STR(reply_to, reply_to)
SETTER_STR(subject, subject)
SETTER_STR(last_error, last_error)
SETTER_STR(idempotency_key, idempotency_key)

#undef SETTER_STR

ClawtPriority
clawt_mailbox_item_get_priority(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_PRIORITY_NORMAL);
    return self->priority;
}

ClawtMailboxState
clawt_mailbox_item_get_state(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_MAILBOX_PENDING);
    return self->state;
}

gint
clawt_mailbox_item_get_depth(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->depth;
}

gint
clawt_mailbox_item_get_attempts(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->attempts;
}

gboolean
clawt_mailbox_item_get_invites_reply(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, TRUE);
    return self->invites_reply;
}

void
clawt_mailbox_item_set_invites_reply(ClawtMailboxItem *self, gboolean invites)
{
    g_return_if_fail(self != NULL);
    self->invites_reply = invites;
}

gint64
clawt_mailbox_item_get_created_at(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->created_at;
}

gint64
clawt_mailbox_item_get_not_before(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->not_before;
}

gint64
clawt_mailbox_item_get_expires_at(ClawtMailboxItem *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->expires_at;
}

void
clawt_mailbox_item_set_priority(ClawtMailboxItem *self, ClawtPriority priority)
{
    g_return_if_fail(self != NULL);
    self->priority = priority;
}

void
clawt_mailbox_item_set_state(ClawtMailboxItem *self, ClawtMailboxState state)
{
    g_return_if_fail(self != NULL);
    self->state = state;
}

void
clawt_mailbox_item_set_depth(ClawtMailboxItem *self, gint depth)
{
    g_return_if_fail(self != NULL);
    self->depth = depth;
}

void
clawt_mailbox_item_set_attempts(ClawtMailboxItem *self, gint attempts)
{
    g_return_if_fail(self != NULL);
    self->attempts = attempts;
}

void
clawt_mailbox_item_set_created_at(ClawtMailboxItem *self, gint64 when)
{
    g_return_if_fail(self != NULL);
    self->created_at = when;
}

void
clawt_mailbox_item_set_not_before(ClawtMailboxItem *self, gint64 when)
{
    g_return_if_fail(self != NULL);
    self->not_before = when;
}

void
clawt_mailbox_item_set_expires_at(ClawtMailboxItem *self, gint64 when)
{
    g_return_if_fail(self != NULL);
    self->expires_at = when;
}
