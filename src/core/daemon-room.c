/*
 * daemon-room.c - The client surface: msg.send and room.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

JsonNode *
clawt_daemon_handle_room(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /* ── messages and rooms ── */
    if (g_strcmp0(kind, "msg.send") == 0) {
        const gchar *target = clawt_ipc_payload_string(payload, "target");
        const gchar *body = clawt_ipc_payload_string(payload, "body");
        const gchar *from = clawt_ipc_payload_string(payload, "from");
        gint queued;

        if (target == NULL || body == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "target and body are both required");

        /*
         * A correction typed at an agent that is already working is held
         * rather than routed, and does not enter the transcript yet.
         *
         * Appending it now would make the queued line the active leaf, so
         * the rest of the turn already in flight would hang off a line
         * the model was never shown -- and the transcript would read as
         * though the agent had answered something nobody had said.
         *
         * The reply says so, because a message that vanishes from the
         * composer and does not appear in the conversation reads exactly
         * like a message that was lost.
         */
        if (clawt_daemon_turn_steer(self, from != NULL ? from : "user",
                                    target, body)) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "queued");
            json_builder_add_int_value(builder, 0);
            json_builder_set_member_name(builder, "steered");
            json_builder_add_boolean_value(builder, TRUE);
            json_builder_set_member_name(builder, "target_state");
            json_builder_add_string_value(builder, "running");
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        queued = clawt_mailbox_router_send_to(self->router,
                                              from != NULL ? from : "user",
                                              target, body, NULL, 0, &error);

        if (queued < 0)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "queued");
        json_builder_add_int_value(builder, queued);
        json_builder_set_member_name(builder, "steered");
        json_builder_add_boolean_value(builder, FALSE);

        /*
         * Whether anything is going to read it.  A mailbox accepts a
         * message for a stopped agent by design -- that is the point of
         * making it durable -- but a client that cannot tell "queued" from
         * "delivered" leaves the user watching a spinner for an agent that
         * is not running and never will answer.  Reported only for a
         * single agent; for a room the members each have their own state
         * and the client can ask for them.
         */
        {
            ClawtAgent *agent = clawt_agent_manager_get(self->agents, target);

            if (agent != NULL) {
                json_builder_set_member_name(builder, "target_state");
                json_builder_add_string_value(
                    builder, clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                                clawt_agent_get_state(agent)));
            }
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.list") == 0) {
        g_autoptr(GPtrArray) rooms = clawt_room_manager_list(self->rooms);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "rooms");
        json_builder_begin_array(builder);

        for (i = 0; i < rooms->len; i++) {
            ClawtRoom *room = g_ptr_array_index(rooms, i);
            GPtrArray *members = clawt_room_get_members(room);
            guint j;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, clawt_room_get_id(room));
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, clawt_room_get_name(room));
            json_builder_set_member_name(builder, "members");
            json_builder_begin_array(builder);

            for (j = 0; j < members->len; j++)
                json_builder_add_string_value(
                    builder, g_ptr_array_index(members, j));

            json_builder_end_array(builder);

            /*
             * Enough for a client to draw a conversation list without
             * fetching every transcript to find out which rooms have
             * anything in them.  A fleet accumulates a direct room per
             * pair, and most of them are empty.
             */
            json_builder_set_member_name(builder, "messages");
            json_builder_add_int_value(
                builder, clawt_room_get_message_count(room));

            {
                g_autoptr(GPtrArray) last = clawt_room_get_history(room, 1);

                if (last->len > 0) {
                    ClawtMessage *message = g_ptr_array_index(last, 0);

                    json_builder_set_member_name(builder, "last_sender");
                    json_builder_add_string_value(
                        builder, clawt_message_get_sender_id(message));
                    json_builder_set_member_name(builder, "last_body");
                    json_builder_add_string_value(
                        builder, clawt_message_get_body(message));
                    json_builder_set_member_name(builder, "last_ts");
                    json_builder_add_int_value(
                        builder, clawt_message_get_timestamp(message));
                }
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.create") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *members = clawt_ipc_payload_string(payload, "members");
        ClawtRoom *room;

        room = clawt_room_manager_create(self->rooms, room_id, name, &error);

        if (room == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (members != NULL) {
            g_auto(GStrv) parts = g_strsplit(members, ",", -1);
            gsize i;

            for (i = 0; parts[i] != NULL; i++)
                clawt_room_add_member(room, g_strstrip(parts[i]));
        }

        clawt_event_bus_emit(self->bus, "room.created", room_id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "room.add") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtRoom *room = clawt_room_manager_get(self->rooms, room_id);

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room");

        /*
         * Checked rather than passed straight through: without it a
         * request with no agent named added nobody and reported success,
         * which is the one answer a caller cannot act on.
         */
        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent should be added?");

        clawt_room_add_member(room, agent_id);
        clawt_event_bus_emit(self->bus, "room.changed", room_id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "room.history") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *viewer = clawt_ipc_payload_string(payload, "as");
        ClawtRoom *room = clawt_room_manager_get(self->rooms, room_id);
        g_autoptr(GPtrArray) history = NULL;
        guint i;

        /*
         * An agent id means the direct room with that agent, the same way
         * it does for msg.send.  Without this a client showing a
         * conversation had to know how a direct room is named -- and the
         * GTK client did not, so every chat opened empty with a "no such
         * room" error behind it.
         */
        if (room == NULL && room_id != NULL &&
            clawt_agent_manager_get(self->agents, room_id) != NULL)
            room = clawt_room_manager_get_direct(
                self->rooms, viewer != NULL ? viewer : "user", room_id);

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room or agent");

        history = clawt_room_get_history(
            room, (guint)clawt_ipc_payload_int(payload, "limit", 50));

        json_builder_begin_object(builder);

        /*
         * Which room this actually is, because the request may have
         * named an agent and let the daemon resolve the direct room. A
         * client that shows a conversation has to be able to tell
         * whether an incoming message belongs in it, and comparing
         * against the agent it asked for is not the same question --
         * that is how a reply from an agent to one of its peers ended up
         * drawn in the user's own chat with it.
         */
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder, clawt_room_get_id(room));

        json_builder_set_member_name(builder, "messages");
        json_builder_begin_array(builder);

        for (i = 0; i < history->len; i++) {
            ClawtMessage *message = g_ptr_array_index(history, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder,
                                          clawt_message_get_id(message));
            json_builder_set_member_name(builder, "sender");
            json_builder_add_string_value(
                builder, clawt_message_get_sender_id(message));
            json_builder_set_member_name(builder, "body");
            json_builder_add_string_value(builder,
                                          clawt_message_get_body(message));
            json_builder_set_member_name(builder, "ts");
            json_builder_add_int_value(
                builder, clawt_message_get_timestamp(message));

            /*
             * The task this message belongs to, when it belongs to one.
             * It is what turns a transcript into a chain you can follow:
             * without it a delegated reply is just another line from an
             * agent, with no sign of what asked for it.
             */
            if (clawt_message_get_task_id(message) != NULL) {
                json_builder_set_member_name(builder, "task");
                json_builder_add_string_value(
                    builder, clawt_message_get_task_id(message));
            }

            /*
             * How far this message had travelled agent-to-agent.  It is
             * what makes a runaway visible: a conversation whose hop
             * count climbs towards max_hops is a loop, and reading two
             * agents politely agreeing to do nothing gives no sign of
             * that at all.
             */
            json_builder_set_member_name(builder, "depth");
            json_builder_add_int_value(builder,
                                       clawt_message_get_depth(message));

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
