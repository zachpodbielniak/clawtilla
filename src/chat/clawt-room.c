/*
 * clawt-room.c - A conversation with members
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-room.h"

#include <json-glib/json-glib.h>
#include <string.h>

enum {
    SIGNAL_MESSAGE_ADDED,
    SIGNAL_MEMBERS_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtRoom {
    GObject parent_instance;

    gchar     *room_id;
    gchar     *name;
    gchar     *transcript_path;
    GPtrArray *members;      /* gchar* */
    GPtrArray *messages;     /* ClawtMessage*, oldest first */

    gboolean   require_mention;
    guint      max_hops;
    guint      turn_timeout_seconds;
};

G_DEFINE_FINAL_TYPE(ClawtRoom, clawt_room, G_TYPE_OBJECT)

ClawtRoom *
clawt_room_new(const gchar *room_id, const gchar *transcript_path)
{
    ClawtRoom *self;

    g_return_val_if_fail(room_id != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_ROOM, NULL);
    self->room_id = g_strdup(room_id);
    self->transcript_path = clawt_expand_path(transcript_path);

    return self;
}

const gchar *
clawt_room_get_id(ClawtRoom *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), NULL);
    return self->room_id;
}

const gchar *
clawt_room_get_name(ClawtRoom *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), NULL);

    return (self->name != NULL) ? self->name : self->room_id;
}

void
clawt_room_set_name(ClawtRoom *self, const gchar *name)
{
    g_return_if_fail(CLAWT_IS_ROOM(self));

    g_free(self->name);
    self->name = g_strdup(name);
}

void
clawt_room_add_member(ClawtRoom *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_ROOM(self));
    g_return_if_fail(agent_id != NULL);

    if (clawt_room_has_member(self, agent_id))
        return;

    g_ptr_array_add(self->members, g_strdup(agent_id));
    g_signal_emit(self, signals[SIGNAL_MEMBERS_CHANGED], 0);
}

gboolean
clawt_room_remove_member(ClawtRoom *self, const gchar *agent_id)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    for (i = 0; i < self->members->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->members, i), agent_id) != 0)
            continue;

        g_ptr_array_remove_index(self->members, i);
        g_signal_emit(self, signals[SIGNAL_MEMBERS_CHANGED], 0);
        return TRUE;
    }

    return FALSE;
}

gboolean
clawt_room_has_member(ClawtRoom *self, const gchar *agent_id)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM(self), FALSE);

    for (i = 0; i < self->members->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->members, i), agent_id) == 0)
            return TRUE;
    }

    return FALSE;
}

GPtrArray *
clawt_room_get_members(ClawtRoom *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), NULL);
    return self->members;
}

void
clawt_room_set_require_mention(ClawtRoom *self, gboolean require)
{
    g_return_if_fail(CLAWT_IS_ROOM(self));
    self->require_mention = require;
}

gboolean
clawt_room_get_require_mention(ClawtRoom *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), FALSE);
    return self->require_mention;
}

guint
clawt_room_get_max_hops(ClawtRoom *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), 0);
    return self->max_hops;
}

void
clawt_room_set_max_hops(ClawtRoom *self, guint max_hops)
{
    g_return_if_fail(CLAWT_IS_ROOM(self));
    self->max_hops = max_hops;
}

guint
clawt_room_get_turn_timeout(ClawtRoom *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), 0);

    return self->turn_timeout_seconds;
}

void
clawt_room_set_turn_timeout(ClawtRoom *self, guint seconds)
{
    g_return_if_fail(CLAWT_IS_ROOM(self));

    self->turn_timeout_seconds = seconds;
}

gboolean
clawt_room_message_is_for(ClawtRoom    *self,
                          ClawtMessage *message,
                          const gchar  *agent_id,
                          const gchar  *display_name)
{
    const gchar *body;

    g_return_val_if_fail(CLAWT_IS_ROOM(self), FALSE);
    g_return_val_if_fail(message != NULL, FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    /*
     * Never back to its sender.  That alone is an infinite loop, and no
     * mention rule should be able to switch it back on.
     */
    if (g_strcmp0(clawt_message_get_sender_id(message), agent_id) == 0)
        return FALSE;

    if (!clawt_room_has_member(self, agent_id))
        return FALSE;

    /*
     * An addressed message reaches its addressee and nobody else --
     * stronger than a mention, which only ever widens.  A settle notice
     * is the case that needs it: the room holds the delegator and the
     * assignee, the transcript should show the notice once, and only
     * the delegator's mailbox should take it.
     */
    {
        const gchar *only_for = clawt_message_get_only_for(message);

        if (only_for != NULL)
            return g_strcmp0(only_for, agent_id) == 0;
    }

    if (!self->require_mention)
        return TRUE;

    body = clawt_message_get_body(message);

    if (body == NULL)
        return FALSE;

    /*
     * Everybody, but only from a sender that is not an agent.
     *
     * An agent that could broadcast would turn one reply into a turn
     * for every other member, each of which could broadcast again --
     * the runaway the mention rule exists to prevent, rebuilt out of
     * one word.  So an agent's `@all` falls through and names nobody
     * unless it also named somebody individually, and the tool that
     * posts refuses it out loud rather than leaving the agent to wonder
     * where its message went.
     *
     * The test is clawt_agent_id_is_reserved(), which is already the
     * list of senders that are not agents -- the operator, the daemon
     * itself, and the routine and trigger runners.  None of those is a
     * model that can decide to broadcast again, and all of them are
     * saying something the operator arranged.  Asking it this way also
     * means a room needs no view of the fleet to answer.
     */
    if (clawt_mention_is_broadcast(body) &&
        clawt_agent_id_is_reserved(clawt_message_get_sender_id(message)))
        return TRUE;

    /*
     * The matching itself is clawt_mention_names(), because both
     * clients need the same answer -- one to offer a completion, the
     * other to warn before a message is sent to nobody -- and three
     * copies of a boundary rule is three chances to disagree about
     * whether `@bobby` addresses `bob`.
     */
    return clawt_mention_names(body, agent_id, display_name);
}

/*
 * Transcripts are JSON lines: one message per line, appended.
 *
 * Append-only because a transcript is replayed into every context rebuild,
 * so rewriting the file to change a message would change history an agent
 * has already reasoned from.  One line per message means a truncated write
 * costs the last message rather than the file.
 */
static gboolean
append_to_transcript(ClawtRoom     *self,
                     ClawtMessage  *message,
                     GError       **error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *redacted = NULL;
    g_autoptr(GFile) file = NULL;
    g_autoptr(GFileOutputStream) stream = NULL;
    g_autofree gchar *dir = NULL;

    if (self->transcript_path == NULL)
        return TRUE;

    dir = g_path_get_dirname(self->transcript_path);
    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    /*
     * Redacted on the way in.  A transcript gets replayed into every
     * context rebuild, so a key that reached the file would be handed back
     * to the model for ever; redacting at display time would leave it on
     * disk.
     */
    redacted = clawt_redact_secrets(clawt_message_get_body(message));

    builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, clawt_message_get_id(message));
    json_builder_set_member_name(builder, "room");
    json_builder_add_string_value(builder, clawt_message_get_room_id(message));
    json_builder_set_member_name(builder, "sender");
    json_builder_add_string_value(builder,
                                  clawt_message_get_sender_id(message));
    json_builder_set_member_name(builder, "body");
    json_builder_add_string_value(builder, redacted);
    /*
     * `ts`, not `timestamp`.  This writer spelled it the second way and
     * the room manager's spelled it the first, and nothing anywhere read
     * `timestamp` -- so unifying on the spelling every transcript on disk
     * already uses makes this an append to those files rather than a
     * format they would need migrating out of.
     */
    json_builder_set_member_name(builder, "ts");
    json_builder_add_int_value(builder, clawt_message_get_timestamp(message));
    json_builder_set_member_name(builder, "depth");
    json_builder_add_int_value(builder, clawt_message_get_depth(message));

    if (clawt_message_get_sender_name(message) != NULL) {
        json_builder_set_member_name(builder, "sender_name");
        json_builder_add_string_value(builder,
                                      clawt_message_get_sender_name(message));
    }

    if (clawt_message_get_task_id(message) != NULL) {
        json_builder_set_member_name(builder, "task_id");
        json_builder_add_string_value(builder,
                                      clawt_message_get_task_id(message));
    }

    if (clawt_message_get_parent_id(message) != NULL) {
        json_builder_set_member_name(builder, "parent_id");
        json_builder_add_string_value(builder,
                                      clawt_message_get_parent_id(message));
    }

    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    line = json_generator_to_data(generator, NULL);

    file = g_file_new_for_path(self->transcript_path);
    stream = g_file_append_to(file, G_FILE_CREATE_PRIVATE, NULL, error);

    if (stream == NULL)
        return FALSE;

    if (!g_output_stream_write_all(G_OUTPUT_STREAM(stream), line,
                                   strlen(line), NULL, NULL, error))
        return FALSE;

    return g_output_stream_write_all(G_OUTPUT_STREAM(stream), "\n", 1, NULL,
                                     NULL, error);
}

/**
 * clawt_room_set_transcript_path:
 * @self: a #ClawtRoom
 * @path: (nullable): where to append this room's transcript, or %NULL
 *   for a room that is not persisted
 *
 * Where clawt_room_append() writes.  #ClawtRoomManager sets this from the
 * transcript directory it was given, in one place, so a room's file is
 * named for its id and nothing else -- the manager used to hand
 * clawt_room_new() the room's *display name* for this argument, which
 * aimed a transcript at whatever somebody had called the room.
 */
void
clawt_room_set_transcript_path(ClawtRoom *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_ROOM(self));

    g_free(self->transcript_path);
    self->transcript_path = clawt_expand_path(path);
}

/**
 * clawt_room_restore:
 * @self: a #ClawtRoom
 * @message: (transfer none): a message read back from the transcript
 *
 * Puts a message into the room's history without writing it anywhere and
 * without emitting #ClawtRoom::message-added.
 *
 * This exists because the transcript is append-only.  Loading a room
 * through clawt_room_append() would append every line it had just read
 * straight back into the file, so a transcript would double in length on
 * every daemon start -- which is the one way an append-only file can
 * still lose a conversation, by burying it.
 *
 * No signal, either: a restore is not something happening in the room,
 * it is the room being rebuilt, and a subscriber told about a hundred
 * old messages at start has no way to tell them from a hundred new ones.
 */
void
clawt_room_restore(ClawtRoom *self, ClawtMessage *message)
{
    g_return_if_fail(CLAWT_IS_ROOM(self));
    g_return_if_fail(message != NULL);

    g_ptr_array_add(self->messages, clawt_message_copy(message));
}

gboolean
clawt_room_append(ClawtRoom *self, ClawtMessage *message, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), FALSE);
    g_return_val_if_fail(message != NULL, FALSE);

    g_ptr_array_add(self->messages, clawt_message_copy(message));

    if (!append_to_transcript(self, message, error)) {
        /*
         * The message stays in memory even when the file could not be
         * written.  A full disk should not make the conversation vanish
         * from the running daemon as well.
         */
        g_signal_emit(self, signals[SIGNAL_MESSAGE_ADDED], 0, message);
        return FALSE;
    }

    g_signal_emit(self, signals[SIGNAL_MESSAGE_ADDED], 0, message);

    return TRUE;
}

guint
clawt_room_get_message_count(ClawtRoom *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM(self), 0);

    return self->messages->len;
}

GPtrArray *
clawt_room_get_history(ClawtRoom *self, guint limit)
{
    GPtrArray *out;
    guint start;
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_message_free);

    start = (limit > 0 && self->messages->len > limit)
            ? self->messages->len - limit : 0;

    for (i = start; i < self->messages->len; i++)
        g_ptr_array_add(out,
                        clawt_message_copy(g_ptr_array_index(self->messages,
                                                             i)));

    return out;
}

static void
clawt_room_finalize(GObject *object)
{
    ClawtRoom *self = CLAWT_ROOM(object);

    g_clear_pointer(&self->room_id, g_free);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->transcript_path, g_free);
    g_clear_pointer(&self->members, g_ptr_array_unref);
    g_clear_pointer(&self->messages, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_room_parent_class)->finalize(object);
}

static void
clawt_room_class_init(ClawtRoomClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_room_finalize;

    /**
     * ClawtRoom::message-added:
     * @self: the room
     * @message: the message
     */
    signals[SIGNAL_MESSAGE_ADDED] =
        g_signal_new("message-added", CLAWT_TYPE_ROOM, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 1, CLAWT_TYPE_MESSAGE);

    /**
     * ClawtRoom::members-changed:
     * @self: the room
     */
    signals[SIGNAL_MEMBERS_CHANGED] =
        g_signal_new("members-changed", CLAWT_TYPE_ROOM, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
clawt_room_init(ClawtRoom *self)
{
    self->members = g_ptr_array_new_with_free_func(g_free);
    self->messages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_message_free);
}
