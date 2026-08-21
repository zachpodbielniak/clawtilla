/*
 * clawt-mailbox-item.h - One queued message
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MAILBOX_ITEM (clawt_mailbox_item_get_type())

GType clawt_mailbox_item_get_type(void) G_GNUC_CONST;

/**
 * clawt_mailbox_item_new:
 * @from: sender's agent id, or a client identifier for a human
 * @to: recipient's agent id
 * @body: the message
 *
 * Creates an item with a fresh sortable id and the current time.
 *
 * Returns: (transfer full): a new #ClawtMailboxItem
 */
ClawtMailboxItem *clawt_mailbox_item_new(const gchar *from,
                                         const gchar *to,
                                         const gchar *body);

ClawtMailboxItem *clawt_mailbox_item_copy(ClawtMailboxItem *self);
void              clawt_mailbox_item_free(ClawtMailboxItem *self);

const gchar *clawt_mailbox_item_get_id(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_from(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_to(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_body(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_room(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_task_id(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_reply_to(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_subject(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_idempotency_key(ClawtMailboxItem *self);
const gchar *clawt_mailbox_item_get_last_error(ClawtMailboxItem *self);

ClawtPriority     clawt_mailbox_item_get_priority(ClawtMailboxItem *self);
ClawtMailboxState clawt_mailbox_item_get_state(ClawtMailboxItem *self);

gint   clawt_mailbox_item_get_depth(ClawtMailboxItem *self);
gint   clawt_mailbox_item_get_attempts(ClawtMailboxItem *self);
gint64 clawt_mailbox_item_get_created_at(ClawtMailboxItem *self);
gint64 clawt_mailbox_item_get_not_before(ClawtMailboxItem *self);
gint64 clawt_mailbox_item_get_expires_at(ClawtMailboxItem *self);

void clawt_mailbox_item_set_id(ClawtMailboxItem *self, const gchar *id);
void clawt_mailbox_item_set_room(ClawtMailboxItem *self, const gchar *room);
void clawt_mailbox_item_set_task_id(ClawtMailboxItem *self, const gchar *task_id);
void clawt_mailbox_item_set_reply_to(ClawtMailboxItem *self, const gchar *reply_to);
void clawt_mailbox_item_set_subject(ClawtMailboxItem *self, const gchar *subject);
void clawt_mailbox_item_set_last_error(ClawtMailboxItem *self, const gchar *error);
void clawt_mailbox_item_set_priority(ClawtMailboxItem *self, ClawtPriority priority);
void clawt_mailbox_item_set_state(ClawtMailboxItem *self, ClawtMailboxState state);
void clawt_mailbox_item_set_depth(ClawtMailboxItem *self, gint depth);
void clawt_mailbox_item_set_attempts(ClawtMailboxItem *self, gint attempts);
void clawt_mailbox_item_set_created_at(ClawtMailboxItem *self, gint64 when);
void clawt_mailbox_item_set_not_before(ClawtMailboxItem *self, gint64 when);
void clawt_mailbox_item_set_expires_at(ClawtMailboxItem *self, gint64 when);

/**
 * clawt_mailbox_item_set_idempotency_key:
 * @self: a #ClawtMailboxItem
 * @key: (nullable): a caller-chosen key
 *
 * Sets a key that makes posting this item at-most-once.
 *
 * A tool call that timed out on the agent's side but succeeded on ours
 * will be retried; without a key that retry enqueues a second copy and the
 * work happens twice.
 */
void clawt_mailbox_item_set_idempotency_key(ClawtMailboxItem *self,
                                            const gchar      *key);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMailboxItem, clawt_mailbox_item_free)

G_END_DECLS
