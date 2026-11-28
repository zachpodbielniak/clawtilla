/*
 * daemon-step.c - The steps of a turn, on their way to the clients
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * How many of a room's steps are kept, across turns rather than for the
 * running one.
 *
 * They are part of the conversation: the tools an agent reached for are
 * how its answer came about, so a reader scrolling back wants them
 * where they happened, not deleted the moment the answer landed.
 *
 * Bounded because a room's step count has no upper limit that anything
 * here controls, and the oldest go first -- refusing new ones once full
 * would freeze the view at the least useful moment.  A room busy enough
 * to roll steps off therefore shows them for its recent turns and not
 * its older ones, which is worth knowing when reading far back: an
 * older turn drawn with no tool calls may have made some.
 *
 * In memory only.  Steps do not survive the daemon stopping, and they
 * are deliberately not written to the transcript -- the last thing in a
 * thread is what a finished turn's task result is inferred from, and
 * "Ran 6 commands" must never become somebody's delegated outcome.
 */
#define ROOM_STEP_HISTORY 500

/*
 * The tools a turn *stops* with, for the progress hold: a note, a
 * result, a wait or a schedule set, work handed on, a message sent.  A
 * turn that ends on these after a progress note is the turn the hold is
 * for.  Named, not "every clawtilla tool": clawtilla_computer_exec is a
 * confined agent's whole shell, and the browser, display and web tools
 * are its work as much as a host agent's own shell is.  ToolSearch is a
 * CLI loading one of these before it calls it.
 */
static const gchar *const turn_stop_tools[] = {
    "clawtilla_task_progress",
    "clawtilla_task_complete",
    "clawtilla_wait_until",
    "clawtilla_routine_add",
    "clawtilla_routine_update",
    "clawtilla_delegate",
    "clawtilla_ephemeral_spawn",
    "clawtilla_handoff",
    "clawtilla_message_agent",
    "clawtilla_message_user",
    "clawtilla_post_room",
    "clawtilla_mailbox_reply",
    "clawtilla_ask_decision",
    "clawtilla_grok_dispatch",
    "ToolSearch",
    NULL
};

/*
 * Whether a tool step is the turn going on with its work, for the
 * progress hold.  Everything is, except the tools above.  Matched on the
 * name after the last "__", so a CLI's MCP prefix (mcp__clawtilla__) does
 * not matter.
 */
static gboolean
step_carries_the_turn_on(const gchar *tool)
{
    const gchar *name;
    const gchar *sep;
    guint i;

    if (tool == NULL)
        return TRUE;

    name = tool;

    while ((sep = strstr(name, "__")) != NULL && sep[2] != '\0')
        name = sep + 2;

    for (i = 0; turn_stop_tools[i] != NULL; i++) {
        if (g_strcmp0(name, turn_stop_tools[i]) == 0)
            return FALSE;
    }

    return TRUE;
}

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
/*
 * A turn boundary the runtime reported inside one run (VENTURE #58).
 *
 * Here and not in the turn code because it arrives as a step -- the only
 * thing the runtime sends while a turn is still going -- and because it
 * has to be acted on *then*: the reply that ends the run comes after it,
 * and by the time that reply is judged the hold an earlier turn armed
 * must already be spent, or the task must already have failed.
 *
 * Only a status step can carry a marker, and only the runtime writes a
 * status step's detail.
 */
static void
note_turn_boundary(ClawtDaemon   *self,
                   ClawtTurnStep *step,
                   const gchar   *agent_id,
                   const gchar   *room_id,
                   const gchar   *text)
{
    const gchar *marker;
    const gchar *task_id;
    ClawtAgent *agent;
    gboolean continued;

    if (clawt_turn_step_get_kind(step) != CLAWT_STEP_STATUS)
        return;

    marker = clawt_turn_step_get_detail(step);
    continued = g_strcmp0(marker, CLAWT_STEP_DETAIL_TURN_CONTINUED) == 0;

    if (!continued &&
        g_strcmp0(marker, CLAWT_STEP_DETAIL_BACKGROUND_ABANDONED) != 0)
        return;

    if (self->agents == NULL || self->tasks == NULL)
        return;

    agent = clawt_agent_manager_get(self->agents, agent_id);
    task_id = agent != NULL
        ? clawt_agent_get_turn_task_id_in(agent, room_id) : NULL;

    if (task_id == NULL)
        return;

    if (continued) {
        if (clawt_task_manager_note_turn_continued(self->tasks, task_id,
                                                   agent_id, text))
            g_info("daemon: %s's CLI started another turn on %s, so the "
                   "progress hold of the turn before it was spent",
                   agent_id, task_id);
    } else if (clawt_task_manager_abandon_background(self->tasks, task_id,
                                                     agent_id, text)) {
        g_message("daemon: %s ended a turn on %s with background work its "
                  "runtime gave up waiting for; the task has failed: %s",
                  agent_id, task_id, text != NULL ? text : "");
    }
}

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
     * Kept for the room.  A copy, because the caller owns the step for
     * the duration of its signal emission only -- and the copy carries
     * the redacted text, so nothing unredacted is retained anywhere.
     */
    if (self->room_steps != NULL) {
        g_autoptr(ClawtTurnStep) kept = clawt_turn_step_new(
            clawt_turn_step_get_kind(step), agent_id, room_id, text,
            clawt_turn_step_get_tool_name(step), detail,
            clawt_turn_step_get_failed(step));

        clawt_turn_step_set_app(kept, clawt_turn_step_get_app_id(step),
                                clawt_turn_step_get_app_url(step),
                                clawt_turn_step_get_app_title(step));

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

    /* And it is not silent any more: a rescue offer is withdrawn. */
    clawt_daemon_rescue_life(self, agent_id, room_id);

    /* A turn that moved on, or gave up waiting, inside one run. */
    note_turn_boundary(self, step, agent_id, room_id, text);

	/*
	 * And evidence of what the turn did.  A tool step is a command run,
	 * a file read or written, a message sent -- anything that makes the
	 * turn's closing text more than a reply -- so the acknowledgement
	 * check reads this count before it considers skipping a wake.
	 */
	if (clawt_turn_step_get_kind(step) == CLAWT_STEP_TOOL &&
		self->agents != NULL) {
		ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);

		if (agent != NULL) {
			const gchar *tool = clawt_turn_step_get_tool_name(step);
			gboolean failed = clawt_turn_step_get_failed(step);

			clawt_agent_note_turn_tool(agent, room_id);

			/*
			 * And for the progress hold: the turn carrying on after a
			 * progress note says the note was not where it stopped.
			 * Failed or not -- a command that failed is still the turn
			 * working.  The step's own time, not now, because it can
			 * reach here after the note that came later.
			 */
			if (self->tasks != NULL && step_carries_the_turn_on(tool))
				clawt_task_manager_note_turn_work(self->tasks,
					clawt_agent_get_turn_task_id_in(agent, room_id),
					agent_id, clawt_turn_step_get_timestamp(step));

			/*
			 * And for the task progress guard, which wants work rather
			 * than activity: a progress note is the assignee saying it
			 * is still going, not evidence that it is, and a failure is
			 * kept by its text so three identical ones can be seen.
			 */
			if (failed) {
				g_autofree gchar *error = g_strdup_printf("%s: %s",
					tool != NULL ? tool : "a tool",
					(detail != NULL && *detail != '\0') ? detail
						: (text != NULL ? text : "failed"));
				g_autofree gchar *clipped = clawt_utf8_truncate(error, 200,
					FALSE);

				clawt_agent_note_turn_work(agent, room_id, FALSE, clipped);
			} else {
				/* Observed either way; only work that is not a note counts. */
				clawt_agent_note_turn_work(agent, room_id,
					tool == NULL ||
					strstr(tool, "clawtilla_task_progress") == NULL, NULL);
			}
		}
	}

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

    /*
     * When, so a client rebuilding a conversation can put the step back
     * where it happened.  Messages and steps are fetched separately and
     * merged on time; without this every step in a room's history would
     * pile up at the bottom, under answers it came before.
     */
    clawt_event_set_detail_int(event, CLAWT_STEP_MEMBER_TS,
                               clawt_turn_step_get_timestamp(step));

    if (clawt_turn_step_get_app_id(step) != NULL)
        clawt_event_set_detail(event, CLAWT_STEP_MEMBER_APP,
                               clawt_turn_step_get_app_id(step));

    if (clawt_turn_step_get_app_url(step) != NULL)
        clawt_event_set_detail(event, CLAWT_STEP_MEMBER_APP_URL,
                               clawt_turn_step_get_app_url(step));

    if (clawt_turn_step_get_app_title(step) != NULL)
        clawt_event_set_detail(event, CLAWT_STEP_MEMBER_APP_TITLE,
                               clawt_turn_step_get_app_title(step));

    clawt_event_bus_publish(self->bus, event);
    clawt_event_free(event);
}

/*
 * The steps this room has seen, oldest first, across turns.
 *
 * Empty is the ordinary answer and does not mean anything is wrong: a
 * room nothing has run in yet has no steps, and neither does one whose
 * agent has not reached a tool.
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

/*
 * One step as room.steps and activity.list both send it: the member
 * names clawt_turn_step_new_from_object() reads, from the same
 * constants, so a client parses either reply with one function.
 */
void
clawt_daemon_add_step_object(JsonBuilder *builder, ClawtTurnStep *step)
{
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_KIND);
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_STEP_KIND,
                                    clawt_turn_step_get_kind(step)));

    json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_ROOM);
    json_builder_add_string_value(builder,
                                  clawt_turn_step_get_room_id(step));

    if (clawt_turn_step_get_text(step) != NULL) {
        json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_TEXT);
        json_builder_add_string_value(builder,
                                      clawt_turn_step_get_text(step));
    }

    if (clawt_turn_step_get_tool_name(step) != NULL) {
        json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_TOOL);
        json_builder_add_string_value(
            builder, clawt_turn_step_get_tool_name(step));
    }

    if (clawt_turn_step_get_detail(step) != NULL) {
        json_builder_set_member_name(builder,
                                     CLAWT_STEP_MEMBER_DETAIL);
        json_builder_add_string_value(
            builder, clawt_turn_step_get_detail(step));
    }

    json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_FAILED);
    json_builder_add_boolean_value(builder,
                                   clawt_turn_step_get_failed(step));

    json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_TS);
    json_builder_add_int_value(builder,
                               clawt_turn_step_get_timestamp(step));

    if (clawt_turn_step_get_app_id(step) != NULL) {
        json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_APP);
        json_builder_add_string_value(builder,
                                      clawt_turn_step_get_app_id(step));
    }

    if (clawt_turn_step_get_app_url(step) != NULL) {
        json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_APP_URL);
        json_builder_add_string_value(builder,
                                      clawt_turn_step_get_app_url(step));
    }

    if (clawt_turn_step_get_app_title(step) != NULL) {
        json_builder_set_member_name(builder, CLAWT_STEP_MEMBER_APP_TITLE);
        json_builder_add_string_value(builder,
                                      clawt_turn_step_get_app_title(step));
    }

    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder,
                                  clawt_turn_step_get_agent_id(step));

    json_builder_end_object(builder);
}
