/*
 * daemon-step.c - The steps of a turn, on their way to the clients
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * How many steps of one room's running turn are kept.
 *
 * Enough that a client opening a room mid-turn sees what the agent has
 * been doing, and bounded because a turn's step count has no upper
 * limit that anything here controls.  The oldest go first: what an
 * agent did ten minutes ago matters less than what it is doing now, and
 * the alternative -- refusing new steps once full -- would freeze the
 * view at the least useful moment.
 */
#define ROOM_STEP_HISTORY 200

/*
 * One step, from the agent that produced it to the clients.
 *
 * Three things happen here and the order matters.  The text is redacted
 * first, because everything after this point either stores it or sends
 * it somewhere; then it is kept for the room, so a client arriving late
 * can be caught up; then it is published.
 *
 * What does *not* happen here is any part of message delivery.  A step
 * is not enqueued, not routed, and not written to the transcript --
 * there is no call into ClawtMailboxRouter in this file, and that is
 * the property to preserve.  Delivery is what starts a turn, and an
 * agent that started a turn for every step of a peer's turn would
 * answer its own answers for ever; libreclaw's "Still working..." notes
 * were removed for a milder version of the same thing.
 */
void
clawt_daemon_note_step(ClawtDaemon *self, ClawtTurnStep *step)
{
    ClawtEvent *event;
    GPtrArray *history;
    const gchar *room_id;
    const gchar *agent_id;
    g_autofree gchar *text = NULL;
    g_autofree gchar *detail = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));
    g_return_if_fail(step != NULL);

    room_id  = clawt_turn_step_get_room_id(step);
    agent_id = clawt_turn_step_get_agent_id(step);

    if (room_id == NULL || agent_id == NULL)
        return;

    /*
     * Redacted on the way *out*, which is the half that has been got
     * wrong here before: the log ring carried a comment saying it
     * redacted on the way in, directly above a signal emission of the
     * raw line.  A step's detail is a preview of a tool's arguments,
     * which is precisely where a token handed to a shell shows up --
     * and from here it reaches a transcript pane, an SSE stream and
     * whatever a plugin has subscribed to.
     */
    text   = clawt_redact_secrets(clawt_turn_step_get_text(step));
    detail = clawt_redact_secrets(clawt_turn_step_get_detail(step));

    /*
     * Kept for the room, so switching away and back does not throw the
     * running turn away.  A copy, because the caller owns the step for
     * the duration of its signal emission only -- and the copy carries
     * the redacted text, so nothing unredacted is retained anywhere.
     */
    if (self->room_steps != NULL) {
        g_autoptr(ClawtTurnStep) kept = clawt_turn_step_new(
            clawt_turn_step_get_kind(step), agent_id, room_id, text,
            clawt_turn_step_get_tool_name(step), detail,
            clawt_turn_step_get_failed(step));

        history = g_hash_table_lookup(self->room_steps, room_id);

        if (history == NULL) {
            history = g_ptr_array_new_with_free_func(
                (GDestroyNotify)clawt_turn_step_free);
            g_hash_table_insert(self->room_steps, g_strdup(room_id), history);
        }

        while (history->len >= ROOM_STEP_HISTORY)
            g_ptr_array_remove_index(history, 0);

        g_ptr_array_add(history, g_steal_pointer(&kept));
    }

    /*
     * A step is proof of life, which is what the turn watchdogs are
     * looking for.  Before this, activity meant "sent a message", so a
     * turn spending twenty minutes in tool calls looked identical to
     * one that had wedged -- the watchdog could not tell them apart
     * because nothing told it.
     */
    clawt_daemon_turn_activity(self, agent_id);

    event = clawt_event_new("turn.step", agent_id);

    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_KIND,
                           clawt_enum_to_nick(CLAWT_TYPE_STEP_KIND,
                                              clawt_turn_step_get_kind(step)));
    clawt_event_set_detail(event, CLAWT_STEP_MEMBER_ROOM, room_id);

    if (text != NULL && text[0] != '\0')
        clawt_event_set_detail(event, CLAWT_STEP_MEMBER_TEXT, text);

    if (clawt_turn_step_get_tool_name(step) != NULL)
        clawt_event_set_detail(event, CLAWT_STEP_MEMBER_TOOL,
                               clawt_turn_step_get_tool_name(step));

    if (detail != NULL && detail[0] != '\0')
        clawt_event_set_detail(event, CLAWT_STEP_MEMBER_DETAIL, detail);

    clawt_event_set_detail_int(event, CLAWT_STEP_MEMBER_FAILED,
                               clawt_turn_step_get_failed(step) ? 1 : 0);

    clawt_event_bus_publish(self->bus, event);
    clawt_event_free(event);
}

/*
 * What a room's running turn has done so far.
 *
 * Empty is the ordinary answer and does not mean anything is wrong: a
 * room with no turn running has no steps, and neither does one whose
 * agent has not reached a tool yet.
 *
 * Returns: (transfer container) (element-type ClawtTurnStep): the steps
 */
GPtrArray *
clawt_daemon_room_steps(ClawtDaemon *self, const gchar *room_id)
{
    GPtrArray *history;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    if (room_id == NULL || self->room_steps == NULL)
        return g_ptr_array_new();

    history = g_hash_table_lookup(self->room_steps, room_id);

    if (history == NULL)
        return g_ptr_array_new();

    return g_ptr_array_ref(history);
}
