/*
 * daemon-activity.c - Every turn running across the fleet, in one reply
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The Activity page in both clients is a fold over this: which agents are
 * mid-turn, where, for how long, on whose behalf and doing what. Nothing
 * here is new state. Which rooms are running is the agent's typing set,
 * which is already cleared wherever a turn can end -- the runtime exiting
 * included -- and the steps are the rooms' own bounded history. A second
 * table of "running turns" kept here would be one more thing to forget to
 * clear, and the one that drifted would be the one drawing a turn that
 * ended an hour ago.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * Whether a steer typed on the Activity page reaches this turn.
 *
 * msg.send holds a correction for a busy agent and folds it into the turn
 * running in the agent's direct room with the operator -- nowhere else.
 * Offering Steer on a peer exchange or a routine's room would send the
 * text somewhere the row does not describe, so the row says which it is.
 */
static gboolean
turn_is_steerable(ClawtDaemon *self, const gchar *agent_id, const gchar *room_id)
{
    ClawtRoom *direct = self->rooms != NULL
        ? clawt_room_manager_get_direct(self->rooms, "user", agent_id) : NULL;

    return direct != NULL && g_strcmp0(clawt_room_get_id(direct), room_id) == 0;
}

/* The last few steps @agent_id took in @room_id since @started. */
static void
add_recent_steps(ClawtDaemon *self, JsonBuilder *builder, const gchar *agent_id,
                 const gchar *room_id, gint64 started, guint *out_total)
{
    g_autoptr(GPtrArray) history = clawt_daemon_room_steps(self, room_id);
    g_autoptr(GPtrArray) mine = g_ptr_array_new();
    guint first;
    guint i;

    /* A room's history spans turns and members; keep this turn's own. */
    for (i = 0; i < history->len; i++) {
        ClawtTurnStep *step = g_ptr_array_index(history, i);

        if (g_strcmp0(clawt_turn_step_get_agent_id(step), agent_id) == 0 &&
            clawt_turn_step_get_timestamp(step) >= started)
            g_ptr_array_add(mine, step);
    }

    *out_total = mine->len;
    first = mine->len > CLAWT_ACTIVITY_STEPS ? mine->len - CLAWT_ACTIVITY_STEPS : 0;

    json_builder_set_member_name(builder, "steps");
    json_builder_begin_array(builder);
    for (i = first; i < mine->len; i++)
        clawt_daemon_add_step_object(builder, g_ptr_array_index(mine, i));
    json_builder_end_array(builder);
}

JsonNode *
clawt_daemon_handle_activity(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    GPtrArray *agents;     /* transfer none: the manager's own array */
    gint64 now = g_get_real_time();
    guint a;

    (void)payload;
    *handled = TRUE;

    if (g_strcmp0(kind, "activity.list") != 0) {
        *handled = FALSE;
        return NULL;
    }

    builder = json_builder_new();
    agents = clawt_agent_manager_list(self->agents);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "now");
    json_builder_add_int_value(builder, now);
    json_builder_set_member_name(builder, "turns");
    json_builder_begin_array(builder);

    for (a = 0; agents != NULL && a < agents->len; a++) {
        ClawtAgent *agent = g_ptr_array_index(agents, a);
        const gchar *agent_id = clawt_agent_get_id(agent);
        g_autoptr(GPtrArray) rooms = clawt_agent_dup_running_rooms(agent);
        guint r;

        for (r = 0; r < rooms->len; r++) {
            const gchar *key = g_ptr_array_index(rooms, r);
            /* The typing set's key for a frame that named no room. */
            const gchar *room_id = (key[0] == '\x01') ? NULL : key;
            ClawtRoom *room = (room_id != NULL && self->rooms != NULL)
                ? clawt_room_manager_get(self->rooms, room_id) : NULL;
            gint64 started = clawt_agent_get_turn_started_in(agent, room_id);
            const gchar *origin = clawt_agent_get_turn_origin_in(agent, room_id);
            const gchar *task = clawt_agent_get_turn_task_id_in(agent, room_id);
            guint total = 0;

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "agent", agent_id);
            if (clawt_agent_get_name(agent) != NULL)
                clawt_daemon_add_string_member(builder, "agent_name",
                                               clawt_agent_get_name(agent));
            clawt_daemon_add_string_member(builder, "room", room_id != NULL ? room_id : "");
            if (room != NULL && clawt_room_get_name(room) != NULL)
                clawt_daemon_add_string_member(builder, "room_name",
                                               clawt_room_get_name(room));
            json_builder_set_member_name(builder, "started");
            json_builder_add_int_value(builder, started);
            if (origin != NULL)
                clawt_daemon_add_string_member(builder, "origin", origin);
            if (task != NULL)
                clawt_daemon_add_string_member(builder, "task", task);
            json_builder_set_member_name(builder, "steerable");
            json_builder_add_boolean_value(builder,
                room_id != NULL && turn_is_steerable(self, agent_id, room_id));
            json_builder_set_member_name(builder, "running_turns");
            json_builder_add_int_value(builder, (gint64)rooms->len);
            add_recent_steps(self, builder, agent_id, room_id, started, &total);
            json_builder_set_member_name(builder, "step_count");
            json_builder_add_int_value(builder, (gint64)total);
            json_builder_end_object(builder);
        }
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}
