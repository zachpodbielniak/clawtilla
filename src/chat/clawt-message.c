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
    gchar *only_for;
	gchar *delivered_body;
	gchar *answer_from;
	gchar *waiting_on;
	gchar *reply_to;
	gchar *request_room;
	gchar *request_origin;
	gchar *request_task;
    gchar *model;

    gint64        timestamp;
    gint          depth;
    ClawtPriority priority;
    gboolean      invites_reply;
	gboolean      by_hand;
	gboolean      composed;
	gboolean      one_way;
	gboolean      acknowledged;
	guint         merged;
	ClawtConsultRole consult;
	gboolean      interrupted;

	/* A group room's round, and the thread of replies within it. */
	guint         round;
	gchar        *chain;
	gchar        *skipped;
	gchar        *skip_reason;

	/* Steered into a running turn: libreclaw's outcome, or NULL. */
	gchar        *steered;

	/*
	 * A cross-talk summary's jump targets: the Flow rooms and the first
	 * message of each exchange, space-separated and paired by position.
	 */
	gchar        *crosstalk_rooms;
	gchar        *crosstalk_messages;
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

    /*
     * Named rather than left to g_new0().  Zero is CLAWT_PRIORITY_LOW,
     * which is the band drop-oldest sheds first -- the opposite of a
     * neutral default, and not something to arrive at by accident.
     */
    self->priority = CLAWT_PRIORITY_NORMAL;

    /*
     * A message invites a reply unless whoever made it says otherwise.
     *
     * Deliberate messages -- an agent calling clawtilla_message_agent, an
     * operator typing -- are the ones that arrive here, and every one of
     * them is somebody choosing to write.  The exception is the reply
     * itself, built in the daemon's link handler, which clears this: see
     * clawt_message_set_invites_reply().
     */
    self->invites_reply = TRUE;

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
    copy->model = g_strdup(self->model);
    copy->parent_id = g_strdup(self->parent_id);
    copy->only_for = g_strdup(self->only_for);
	copy->delivered_body = g_strdup(self->delivered_body);
	copy->answer_from = g_strdup(self->answer_from);
	copy->waiting_on = g_strdup(self->waiting_on);
	copy->reply_to = g_strdup(self->reply_to);
	copy->request_room = g_strdup(self->request_room);
	copy->request_origin = g_strdup(self->request_origin);
	copy->request_task = g_strdup(self->request_task);
    copy->timestamp = self->timestamp;
    copy->depth = self->depth;
    copy->priority = self->priority;
    copy->invites_reply = self->invites_reply;
	copy->by_hand = self->by_hand;
	copy->composed = self->composed;
	copy->one_way = self->one_way;
	copy->acknowledged = self->acknowledged;
	copy->merged = self->merged;
	copy->consult = self->consult;
	copy->interrupted = self->interrupted;
	copy->round = self->round;
	copy->chain = g_strdup(self->chain);
	copy->steered = g_strdup(self->steered);
	copy->crosstalk_rooms = g_strdup(self->crosstalk_rooms);
	copy->crosstalk_messages = g_strdup(self->crosstalk_messages);
	copy->skipped = g_strdup(self->skipped);
	copy->skip_reason = g_strdup(self->skip_reason);

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
    g_free(self->model);
    g_free(self->parent_id);
    g_free(self->only_for);
	g_free(self->delivered_body);
	g_free(self->answer_from);
	g_free(self->waiting_on);
	g_free(self->reply_to);
	g_free(self->request_room);
	g_free(self->request_origin);
	g_free(self->request_task);
	g_free(self->chain);
	g_free(self->steered);
	g_free(self->crosstalk_rooms);
	g_free(self->crosstalk_messages);
	g_free(self->skipped);
	g_free(self->skip_reason);
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
GETTER(reply_to, reply_to)
GETTER(request_room, request_room)
GETTER(request_origin, request_origin)
GETTER(request_task, request_task)
GETTER(model, model)

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
SETTER(room_id, room_id)
SETTER(sender_name, sender_name)
SETTER(task_id, task_id)
SETTER(parent_id, parent_id)
SETTER(only_for, only_for)
SETTER(delivered_body, delivered_body)
SETTER(answer_from, answer_from)
SETTER(waiting_on, waiting_on)
SETTER(reply_to, reply_to)
SETTER(model, model)

#undef SETTER

/* Keep the originating turn together when a request or answer is copied. */
void
clawt_message_set_request_context(ClawtMessage *self, const gchar *room,
	const gchar *origin, const gchar *task_id)
{
	g_return_if_fail(self != NULL);
	g_free(self->request_room);
	g_free(self->request_origin);
	g_free(self->request_task);
	self->request_room = g_strdup(room);
	self->request_origin = g_strdup(origin);
	self->request_task = g_strdup(task_id);
}

const gchar *
clawt_message_get_only_for(ClawtMessage *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->only_for;
}

gboolean
clawt_message_is_from_system(ClawtMessage *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return g_strcmp0(self->sender_id, CLAWT_SYSTEM_SENDER) == 0;
}

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

gboolean
clawt_message_get_invites_reply(ClawtMessage *self)
{
    g_return_val_if_fail(self != NULL, TRUE);
    return self->invites_reply;
}

void
clawt_message_set_invites_reply(ClawtMessage *self, gboolean invites)
{
    g_return_if_fail(self != NULL);
    self->invites_reply = invites;
}

gboolean
clawt_message_get_by_hand(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, FALSE);
	return self->by_hand;
}

void
clawt_message_set_by_hand(
	ClawtMessage *self,
	gboolean      by_hand
){
	g_return_if_fail(self != NULL);
	self->by_hand = by_hand;
}

gboolean
clawt_message_get_composed(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, FALSE);
	return self->composed;
}

void
clawt_message_set_composed(
	ClawtMessage *self,
	gboolean      composed
){
	g_return_if_fail(self != NULL);
	self->composed = composed;
}

ClawtPriority
clawt_message_get_priority(ClawtMessage *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_PRIORITY_NORMAL);
    return self->priority;
}

void
clawt_message_set_priority(ClawtMessage *self, ClawtPriority priority)
{
    g_return_if_fail(self != NULL);
    self->priority = priority;
}

gboolean
clawt_message_priority_from_nick(const gchar    *nick,
                                 ClawtPriority  *out_priority,
                                 gchar         **out_refusal)
{
    gint band = CLAWT_PRIORITY_NORMAL;

    g_return_val_if_fail(out_priority != NULL, FALSE);

    *out_priority = CLAWT_PRIORITY_NORMAL;

    if (nick == NULL || *nick == '\0')
        return TRUE;

    /*
     * clawt_enum_from_nick() rather than g_enum_get_value_by_nick(),
     * because the latter's case-insensitivity arrived in a particular
     * GLib and whether "Urgent" is accepted must not depend on which one
     * this was built against.
     */
    if (!clawt_enum_from_nick(CLAWT_TYPE_PRIORITY, nick, &band)) {
        if (out_refusal != NULL) {
            g_autofree gchar *bands =
                clawt_enum_nick_list(CLAWT_TYPE_PRIORITY);

            *out_refusal = g_strdup_printf(
                "'%s' is not a priority. It is one of %s -- leave it out "
                "for normal. Nothing was sent, so send it again with a "
                "band from that list.", nick, bands);
        }

        return FALSE;
    }

    *out_priority = (ClawtPriority)band;

    return TRUE;
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
                          gint64 connected_at, GHashTable *rows)
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

    /*
     * And a room somebody can actually open to clear it.
     *
     * Peer traffic between two agents, and every routine's and
     * trigger's room, has no row in either sidebar -- so a count
     * against it can only ever go up.  It used to be excluded by each
     * client resolving the room to an agent first, which is exactly why
     * counting group rooms dropped it: a group resolves to no agent.
     */
    if (rows == NULL || !g_hash_table_contains(rows, room_id))
        return FALSE;

    return TRUE;
}

gboolean
clawt_transcript_is_at_bottom(gdouble value, gdouble upper, gdouble page_size)
{
    gdouble bottom = upper - page_size;

    /*
     * Shorter than the viewport: there is nowhere else to be.
     *
     * The comparison below already answers this correctly -- a negative
     * bottom is less than the tolerance -- so this is not a fix.  It says
     * the intent out loud, because a case that is right by accident is
     * one a later edit breaks without noticing.
     */
    if (bottom <= 0.0)
        return TRUE;

    return (bottom - value) < CLAWT_TRANSCRIPT_FOLLOW_TOLERANCE;
}

/*
 * reply: none.  Kept apart from invites_reply because the two say
 * different things to the recipient: a cleared invite on its own means
 * "this is somebody answering you", which is a lie about an FYI, and the
 * preamble has to be able to tell a notice from an answer.
 */
gboolean
clawt_message_get_one_way(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, FALSE);

	return self->one_way;
}

void
clawt_message_set_one_way(
	ClawtMessage *self,
	gboolean      one_way
){
	g_return_if_fail(self != NULL);

	self->one_way = one_way;

	/*
	 * A one-way message never invites an answer: the two are set
	 * together so no caller can produce the contradiction.
	 */
	if (one_way)
		self->invites_reply = FALSE;
}

gboolean
clawt_message_get_acknowledged(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, FALSE);

	return self->acknowledged;
}

void
clawt_message_set_acknowledged(
	ClawtMessage *self,
	gboolean      acknowledged
){
	g_return_if_fail(self != NULL);

	self->acknowledged = acknowledged;
}

guint
clawt_message_get_merged(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, 0);

	return self->merged;
}

void
clawt_message_set_merged(
	ClawtMessage *self,
	guint         parts
){
	g_return_if_fail(self != NULL);

	self->merged = parts;
}

gboolean
clawt_message_get_interrupted(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, FALSE);

	return self->interrupted;
}

void
clawt_message_set_interrupted(
	ClawtMessage *self,
	gboolean      interrupted
){
	g_return_if_fail(self != NULL);

	self->interrupted = interrupted;
}

const gchar *
clawt_message_get_steered(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->steered;
}

void
clawt_message_set_steered(
	ClawtMessage *self,
	const gchar  *outcome
){
	g_return_if_fail(self != NULL);

	g_free(self->steered);
	self->steered = (outcome != NULL && *outcome != '\0')
		? g_strdup(outcome) : NULL;
}

const gchar *
clawt_message_get_crosstalk_rooms(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->crosstalk_rooms;
}

const gchar *
clawt_message_get_crosstalk_messages(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->crosstalk_messages;
}

void
clawt_message_set_crosstalk(
	ClawtMessage *self,
	const gchar  *rooms,
	const gchar  *messages
){
	g_return_if_fail(self != NULL);

	g_clear_pointer(&self->crosstalk_rooms, g_free);
	g_clear_pointer(&self->crosstalk_messages, g_free);

	/*
	 * Both or neither: a message id with no room has nowhere to open,
	 * and a client pairing the two lists by position must never find
	 * one longer than the other because a half was dropped here.
	 */
	if (rooms == NULL || *rooms == '\0')
		return;

	self->crosstalk_rooms = g_strdup(rooms);
	self->crosstalk_messages = g_strdup(messages != NULL ? messages : "");
}

guint
clawt_message_get_round(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, 0);

	return self->round;
}

void
clawt_message_set_round(
	ClawtMessage *self,
	guint         round
){
	g_return_if_fail(self != NULL);

	self->round = round;
}

const gchar *
clawt_message_get_chain(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->chain;
}

void
clawt_message_set_chain(
	ClawtMessage *self,
	const gchar  *chain
){
	g_return_if_fail(self != NULL);

	g_free(self->chain);
	self->chain = (chain != NULL && *chain != '\0') ? g_strdup(chain) : NULL;
}

const gchar *
clawt_message_get_skipped(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->skipped;
}

const gchar *
clawt_message_get_skip_reason(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->skip_reason;
}

void
clawt_message_set_skipped(
	ClawtMessage *self,
	const gchar  *skipped,
	const gchar  *reason
){
	g_return_if_fail(self != NULL);

	g_free(self->skipped);
	g_free(self->skip_reason);

	/* The two travel together: a reason about nobody says nothing. */
	if (skipped == NULL || *skipped == '\0') {
		self->skipped = NULL;
		self->skip_reason = NULL;
		return;
	}

	self->skipped = g_strdup(skipped);
	self->skip_reason = g_strdup(reason);
}

const gchar *
clawt_message_get_delivered_body(ClawtMessage *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->delivered_body;
}

const gchar *
clawt_message_get_answer_from(ClawtMessage *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->answer_from;
}

const gchar *
clawt_message_get_waiting_on(ClawtMessage *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->waiting_on;
}

gchar *
clawt_message_answer_label(const gchar *answer_from)
{
	return g_strdup_printf("Answer from %s",
		(answer_from != NULL && *answer_from != '\0') ? answer_from
		                                               : "a teammate");
}

gchar *
clawt_message_waiting_note(const gchar *waiting_on)
{
	return g_strdup_printf("Waiting on %s \xe2\x80\x94 the answer will "
	                       "arrive here",
		(waiting_on != NULL && *waiting_on != '\0') ? waiting_on
		                                             : "a teammate");
}

const gchar *
clawt_message_waiting_tooltip(void)
{
	return "This turn asked a teammate something and ended. The answer "
	       "comes back to this conversation and starts the agent's next "
	       "turn here.";
}

const gchar *
clawt_message_ack_label(void)
{
	return "acknowledged (no wake)";
}

const gchar *
clawt_message_interrupted_label(void)
{
	return "interrupted \xe2\x80\x94 not resumed";
}

const gchar *
clawt_message_interrupted_tooltip(void)
{
	return "A restart or a stop cut this turn short. It is not rerun, so "
	       "nothing it already did outside the fleet is done twice. Check "
	       "what was done and ask again if it still needs doing.";
}

const gchar *
clawt_message_ack_tooltip(void)
{
	return "This answer only acknowledged a message, so clawtilla recorded "
	       "it here instead of waking the agent that asked. That agent "
	       "is told about it on its next turn.";
}

ClawtConsultRole
clawt_message_get_consult(
	ClawtMessage *self
){
	g_return_val_if_fail(self != NULL, CLAWT_CONSULT_NONE);

	return self->consult;
}

void
clawt_message_set_consult(
	ClawtMessage     *self,
	ClawtConsultRole  role
){
	g_return_if_fail(self != NULL);

	self->consult = role;
}

/*
 * An int rather than the enum, because the clients read the role back
 * off the wire as a plain integer and a value from a newer daemon must
 * fall through to "no label" rather than be cast into a role it is not.
 */
const gchar *
clawt_message_consult_label(
	gint role
){
	switch (role) {
	case CLAWT_CONSULT_ASKED:
		return "consultation";
	case CLAWT_CONSULT_ANSWERED:
		return "consultation answer";
	default:
		return NULL;
	}
}

const gchar *
clawt_message_consult_tooltip(void)
{
	return "A consultation: the agent asked was offered no tool that "
	       "acts -- no delegation, no messaging, no connector writes, no "
	       "commands -- so it could only answer from what it knows or can "
	       "read.";
}
