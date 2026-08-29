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

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_MESSAGE (clawt_message_get_type())

GType clawt_message_get_type(void) G_GNUC_CONST;

/**
 * clawt_message_new:
 * @room_id: where it is being said; an agent id is accepted and resolves
 *   to the direct room between sender and recipient
 * @sender_id: who is saying it, or "user" for a person
 * @body: what is said
 *
 * The id and timestamp are generated here, so two messages created in
 * the same microsecond still sort deterministically.
 *
 * Returns: (transfer full): a new #ClawtMessage
 */
ClawtMessage *clawt_message_new(const gchar *room_id,
                                const gchar *sender_id,
                                const gchar *body);

/**
 * clawt_message_copy:
 * @self: a #ClawtMessage
 *
 * Returns: (transfer full): a copy
 */
ClawtMessage *clawt_message_copy(ClawtMessage *self);
void          clawt_message_free(ClawtMessage *self);

/**
 * clawt_message_get_id:
 * @self: a #ClawtMessage
 *
 * The accessors below read what clawt_message_new() and the setters put
 * there.  Documented as a group rather than one line each; the fields
 * are described on #ClawtMessage.
 *
 * String getters are (transfer none).  Task id, parent id and sender
 * name are (nullable) -- a message from a person has no sender name, and
 * one that is not part of delegated work has no task.
 *
 * Returns: (transfer none): the message's identifier
 */
const gchar *clawt_message_get_id(ClawtMessage *self);
const gchar *clawt_message_get_room_id(ClawtMessage *self);
const gchar *clawt_message_get_sender_id(ClawtMessage *self);
const gchar *clawt_message_get_sender_name(ClawtMessage *self);
const gchar *clawt_message_get_body(ClawtMessage *self);
const gchar *clawt_message_get_task_id(ClawtMessage *self);
gint64       clawt_message_get_timestamp(ClawtMessage *self);
gint         clawt_message_get_depth(ClawtMessage *self);

/**
 * clawt_message_get_priority:
 * @self: a #ClawtMessage
 *
 * The delivery band this message asked for.
 *
 * A message is the only thing that travels from a sender to a mailbox,
 * so it is the only place a band can be carried.  Without this field
 * every #ClawtMailboxItem the router ever queued was built at the
 * constructor's %CLAWT_PRIORITY_NORMAL -- the mailbox leased by band,
 * `drop-oldest` shed by band, the docs described the bands and
 * `clawtilla_message_agent` promised that urgent jumps the queue, and
 * nothing outside a test had ever set one.
 *
 * Returns: the band, %CLAWT_PRIORITY_NORMAL unless a sender named one
 */
ClawtPriority clawt_message_get_priority(ClawtMessage *self);

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

/**
 * clawt_message_set_id:
 * @self: a #ClawtMessage
 * @id: the identifier
 *
 * The setters below exist for reading a message back off disk or off the
 * wire, where the values are already decided.  A message built in code
 * gets its id and timestamp from clawt_message_new().
 */
void clawt_message_set_id(ClawtMessage *self, const gchar *id);
void clawt_message_set_sender_name(ClawtMessage *self, const gchar *name);
void clawt_message_set_task_id(ClawtMessage *self, const gchar *task_id);
void clawt_message_set_parent_id(ClawtMessage *self, const gchar *parent_id);
void clawt_message_set_timestamp(ClawtMessage *self, gint64 timestamp);
void clawt_message_set_depth(ClawtMessage *self, gint depth);

/**
 * clawt_message_get_invites_reply:
 * @self: a #ClawtMessage
 *
 * Whether the recipient's ordinary turn output should be sent back.
 *
 * Returns: %TRUE unless the sender cleared it
 */
gboolean clawt_message_get_invites_reply(ClawtMessage *self);

/**
 * clawt_message_set_invites_reply:
 * @self: a #ClawtMessage
 * @invites: %TRUE if an answer is wanted
 *
 * Says whether the agent this reaches should answer it by ordinary
 * reply.  %TRUE by default; the daemon clears it on the message an
 * agent's own reply produces, which is what makes a peer exchange end.
 *
 * An AI CLI answers whatever it is handed -- the text it writes at the
 * end of a turn *is* the reply, and there is no way for it to write
 * nothing.  So two agents each replying politely could only be stopped
 * by `orchestration.max_hops`, eight turns later, and a one-line
 * greeting cost eight.  Clearing this on a reply means a deliberate
 * message earns exactly one answer and the exchange stops there;
 * anything further has to be another deliberate call, which is the
 * difference between a conversation and a loop.
 */
void clawt_message_set_invites_reply(ClawtMessage *self, gboolean invites);

/**
 * clawt_message_set_priority:
 * @self: a #ClawtMessage
 * @priority: the band to queue at
 *
 * Sets the delivery band, which the router copies onto every mailbox
 * item the message produces.
 *
 * Separate from the depth beside it on purpose.  The two are adjacent
 * small integers with nothing in the type system between them, and the
 * `0` a caller passes for depth has already once been read as a
 * priority -- which, since %CLAWT_PRIORITY_LOW is 0, would silently
 * post at the band `drop-oldest` sheds first.
 */
void clawt_message_set_priority(ClawtMessage *self, ClawtPriority priority);

/**
 * clawt_message_priority_from_nick:
 * @nick: (nullable): the band a sender named, or %NULL if they named none
 * @out_priority: (out): return location for the band
 * @out_refusal: (out) (optional) (nullable): return location for what to
 *   tell the sender when @nick is not a band
 *
 * Turns the band a sender wrote into a #ClawtPriority, or refuses.
 *
 * Every surface that lets somebody name a band goes through this -- the
 * `priority` argument of `clawtilla_message_agent` and the `priority`
 * parameter of a pod's `message_agent` -- so the vocabulary is one
 * answer rather than one per caller, and the two cannot come to differ
 * on what "urgent" means or on what happens to "P1".
 *
 * Absent is %CLAWT_PRIORITY_NORMAL: a sender who did not mention a band
 * did not ask for anything.
 *
 * An unrecognised nickname is a **refusal**, never a fallback, and that
 * is the whole decision here.  %CLAWT_PRIORITY_LOW is 0, so falling
 * through to a zeroed value turns a mistyped `urgent` into the band
 * `drop-oldest` sheds *first* -- the exact opposite of what was asked
 * for, invisible from both ends, and the same silence that once let an
 * agent asking for `block-sender` get `reject`.  A refusal naming the
 * bands costs the sender one turn and is the only answer it can act on.
 *
 * The bands come from %CLAWT_TYPE_PRIORITY rather than from a list of
 * strings here, so a band added to the enum needs no edit in this file
 * and the refusal cannot name a set that has stopped being true.
 *
 * Returns: %TRUE if @nick names a band, or names nothing
 */
gboolean clawt_message_priority_from_nick(const gchar    *nick,
                                          ClawtPriority  *out_priority,
                                          gchar         **out_refusal);

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

/**
 * clawt_unread_should_count:
 * @room_id: (nullable): the room the message arrived in
 * @viewing_room: (nullable): the room on screen, or %NULL
 * @from: (nullable): who sent it
 * @event_ts: when it happened, in microseconds
 * @connected_at: when this client connected, in microseconds
 *
 * Whether an arriving message counts as unread.
 *
 * Four conditions, and every one of them was a bug waiting to happen:
 *
 * - **Not your own.**  A message from `user` is one the operator sent.
 * - **Not the room on screen.**  A conversation being read never accrues
 *   a count whatever the scroll position -- that case belongs to the
 *   transcript's "New messages" rule, which deliberately carries no
 *   number.  The two must never fire for the same message.
 * - **Not a replay.**  A client subscribes from cursor 0 and the daemon
 *   replays its recent events, so the first thing a fresh window
 *   receives is everything that just happened -- possibly read in the
 *   previous session.  Counting those opens a window already showing a
 *   number for a conversation nobody has touched, and makes the count
 *   depend on whether the replay beat the first fleet listing.
 * - **A room at all.**  The caller resolves @room_id to an agent before
 *   asking; a room that is nobody's conversation with the operator is
 *   the fleet's own peer traffic.
 *
 * Here rather than in either client because both apply it, and two
 * implementations of one rule differ exactly once -- on the case nobody
 * looked at.  Pure, so the four conditions can be exercised without a
 * window, a browser or a daemon.
 *
 * Returns: %TRUE if the message should increment an unread count
 */
gboolean clawt_unread_should_count(const gchar *room_id,
                                   const gchar *viewing_room,
                                   const gchar *from,
                                   gint64       event_ts,
                                   gint64       connected_at);

/**
 * CLAWT_TRANSCRIPT_FOLLOW_TOLERANCE:
 *
 * How near the bottom still counts as being at it, in pixels.
 *
 * Not zero, because a scrolled window rarely lands on an exact value and
 * a reader who is one pixel off the end has not chosen to be.  Not large,
 * because every pixel of it is a pixel of message a new arrival can push
 * off the bottom without the client noticing it stopped following.
 */
#define CLAWT_TRANSCRIPT_FOLLOW_TOLERANCE 32.0

/**
 * clawt_transcript_is_at_bottom:
 * @value: the adjustment's value
 * @upper: its upper bound
 * @page_size: the visible height
 *
 * Whether the reader is at the live edge of a transcript.
 *
 * The whole follow behaviour turns on this one predicate: a client
 * refuses to move the view when it is false, which is right, and both
 * unread affordances are driven by the edge where it changes.  It is a
 * pure function so the tolerance can be exercised on both sides and at
 * the boundary without a window -- which is the one thing a test of the
 * follow machinery could not otherwise reach.
 *
 * A transcript shorter than its viewport is at the bottom by definition:
 * @upper less @page_size is zero or negative there, and a reader cannot
 * be anywhere else.
 *
 * Returns: %TRUE if the view is at, or within the tolerance of, the end
 */
gboolean clawt_transcript_is_at_bottom(gdouble value,
                                       gdouble upper,
                                       gdouble page_size);

G_END_DECLS
