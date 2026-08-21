/*
 * clawt-message.h - One message in a room
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

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MESSAGE (clawt_message_get_type())

GType clawt_message_get_type(void) G_GNUC_CONST;

ClawtMessage *clawt_message_new(const gchar *room_id,
                                const gchar *sender_id,
                                const gchar *body);

ClawtMessage *clawt_message_copy(ClawtMessage *self);
void          clawt_message_free(ClawtMessage *self);

const gchar *clawt_message_get_id(ClawtMessage *self);
const gchar *clawt_message_get_room_id(ClawtMessage *self);
const gchar *clawt_message_get_sender_id(ClawtMessage *self);
const gchar *clawt_message_get_sender_name(ClawtMessage *self);
const gchar *clawt_message_get_body(ClawtMessage *self);
const gchar *clawt_message_get_task_id(ClawtMessage *self);
gint64       clawt_message_get_timestamp(ClawtMessage *self);
gint         clawt_message_get_depth(ClawtMessage *self);

/**
 * clawt_message_get_parent_id:
 * @self: a #ClawtMessage
 *
 * The message this one replaces or replies to.
 *
 * Editing forks rather than overwrites: two messages sharing a parent are
 * two versions of the same thing, and the earlier one is still there.  An
 * edit that destroyed the original would also destroy whatever an agent
 * had already reasoned from.
 *
 * Returns: (transfer none) (nullable): the parent's id
 */
const gchar *clawt_message_get_parent_id(ClawtMessage *self);

void clawt_message_set_id(ClawtMessage *self, const gchar *id);
void clawt_message_set_sender_name(ClawtMessage *self, const gchar *name);
void clawt_message_set_task_id(ClawtMessage *self, const gchar *task_id);
void clawt_message_set_parent_id(ClawtMessage *self, const gchar *parent_id);
void clawt_message_set_timestamp(ClawtMessage *self, gint64 timestamp);
void clawt_message_set_depth(ClawtMessage *self, gint depth);

/**
 * clawt_message_body_fingerprint:
 * @self: a #ClawtMessage
 *
 * A hash of sender, room and body, used to notice a message repeating.
 *
 * This is what catches the loop the hop limit does not: two agents
 * alternating the same two replies, each one a fresh chain with a depth of
 * one.
 *
 * Returns: (transfer full): the fingerprint
 */
gchar *clawt_message_body_fingerprint(ClawtMessage *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMessage, clawt_message_free)

G_END_DECLS
