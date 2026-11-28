/*
 * daemon-task.c - The client surface: task.*
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

/*
 * Whether the operator's Retry can do anything for @incident right now.
 *
 * Decided here, once, so three clients do not each guess from the state
 * names which rows get the button: a resolved incident, a run still
 * going, and an agent that never started (its work is still queued) all
 * say no.
 */
static gboolean
incident_retryable(
	ClawtDaemon   *self,
	ClawtIncident *incident
){
	ClawtTask *task;

	if (clawt_incident_is_resolved(incident) ||
	    clawt_incident_get_kind(incident) == CLAWT_INCIDENT_UNSTARTABLE)
		return FALSE;

	task = clawt_task_manager_get(self->tasks,
	                              clawt_incident_get_task_id(incident));

	if (task == NULL)
		return clawt_incident_get_routine_id(incident) != NULL;

	return clawt_task_state_is_retryable(clawt_task_get_state(task));
}

static void
add_incident_object(
	ClawtDaemon   *self,
	JsonBuilder   *builder,
	ClawtIncident *incident
){
	const gchar *const *lines = clawt_incident_get_log_lines(incident);
	gsize i;

	json_builder_begin_object(builder);

	clawt_daemon_add_string_member(builder, "id",
	                               clawt_incident_get_id(incident));
	clawt_daemon_add_string_member(
		builder, "kind",
		clawt_enum_to_nick(CLAWT_TYPE_INCIDENT_KIND,
		                   clawt_incident_get_kind(incident)));
	clawt_daemon_add_string_member(
		builder, "kind_label",
		clawt_incident_kind_label(clawt_incident_get_kind(incident)));
	clawt_daemon_add_string_member(builder, "agent",
	                               clawt_incident_get_agent(incident));
	clawt_daemon_add_string_member(builder, "team",
	                               clawt_incident_get_team(incident));
	clawt_daemon_add_string_member(builder, "room",
	                               clawt_incident_get_room(incident));
	clawt_daemon_add_string_member(builder, "run_room",
	                               clawt_incident_get_run_room(incident));
	clawt_daemon_add_string_member(builder, "task",
	                               clawt_incident_get_task_id(incident));
	clawt_daemon_add_string_member(builder, "routine",
	                               clawt_incident_get_routine_id(incident));
	clawt_daemon_add_string_member(builder, "reason",
	                               clawt_incident_get_reason(incident));
	clawt_daemon_add_string_member(builder, "recipient",
	                               clawt_incident_get_recipient(incident));
	clawt_daemon_add_string_member(builder, "note",
	                               clawt_incident_get_note(incident));
	clawt_daemon_add_string_member(builder, "retried_by",
	                               clawt_incident_get_retried_by(incident));

	json_builder_set_member_name(builder, "occurrences");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_occurrences(incident));
	json_builder_set_member_name(builder, "retries");
	json_builder_add_int_value(builder, clawt_incident_get_retries(incident));

	/* Microseconds, like every timestamp but a task's. */
	json_builder_set_member_name(builder, "opened_at");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_opened_at(incident));
	json_builder_set_member_name(builder, "updated_at");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_updated_at(incident));
	json_builder_set_member_name(builder, "delivered_at");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_delivered_at(incident));

	json_builder_set_member_name(builder, "queued");
	json_builder_add_boolean_value(builder,
	                               clawt_incident_is_queued(incident));
	json_builder_set_member_name(builder, "resolved");
	json_builder_add_boolean_value(builder,
	                               clawt_incident_is_resolved(incident));
	json_builder_set_member_name(builder, "retryable");
	json_builder_add_boolean_value(builder,
	                               incident_retryable(self, incident));

	json_builder_set_member_name(builder, "log_lines");
	json_builder_begin_array(builder);

	for (i = 0; lines != NULL && lines[i] != NULL; i++)
		json_builder_add_string_value(builder, lines[i]);

	json_builder_end_array(builder);
	json_builder_end_object(builder);
}

/*
 * Hands an operator's task to its assignee: the work and the task
 * contract, from `user`, into the conversation the two already share.
 * A send that is refused fails the task, so it is not left `pending`
 * with nobody told -- which reads as work in progress nobody is doing.
 */
gboolean
clawt_daemon_deliver_operator_task(
	ClawtDaemon  *self,
	ClawtTask    *task,
	GError      **error
){
	g_autofree gchar *guidance = NULL;
	g_autofree gchar *body = NULL;

	guidance = clawt_task_assignment_guidance_for(task);
	body = g_strconcat(clawt_task_get_prompt(task), guidance, NULL);

	if (clawt_mailbox_router_send_to(self->router, "user",
	                                 clawt_task_get_assignee(task), body,
	                                 clawt_task_get_id(task), 0, error) >= 0)
		return TRUE;

	clawt_task_manager_fail(self->tasks, clawt_task_get_id(task),
	                        "it could not be handed to its assignee");
	return FALSE;
}

static void publish_note(ClawtDaemon *self, ClawtTask *task,
                         const gchar *author);

/*
 * A comment on a task: kept as a note by @author and, while the task is
 * with its assignee, said to it in the conversation the work is in.  The
 * router does what it does with any message for a busy agent -- steers
 * it into the running turn when steering is on, or queues it for the
 * next -- so a comment never starts a turn of its own on a busy agent.
 * A backlog, blocked or finished task only keeps it.
 *
 * @said is how the assignee hears it, when that is not the operator's
 * plain words (a VENTURE comment is fenced); %NULL for the operator's.
 */
gint64
clawt_daemon_task_comment(
	ClawtDaemon  *self,
	const gchar  *task_id,
	const gchar  *author,
	const gchar  *said,
	const gchar  *text,
	gboolean     *told,
	GError      **error
){
	ClawtTask *task;
	gint64 note_id;

	if (told != NULL)
		*told = FALSE;

	note_id = clawt_task_manager_add_note(self->tasks, task_id, author,
	                                      CLAWT_TASK_NOTE_COMMENT, text, error);

	if (note_id == 0)
		return 0;

	task = clawt_task_manager_get(self->tasks, task_id);

	if (task != NULL &&
	    clawt_task_state_is_active(clawt_task_get_state(task)) &&
	    clawt_task_get_assignee(task) != NULL) {
		g_autofree gchar *body = (said != NULL) ? g_strdup(said)
			: g_strdup_printf("The operator commented on task %s (%s):\n\n%s",
			                  task_id, clawt_task_get_title(task), text);
		const gchar *target = clawt_task_get_thread_room(task) != NULL
			? clawt_task_get_thread_room(task)
			: clawt_task_get_assignee(task);
		g_autoptr(GError) send_error = NULL;
		gboolean sent;

		sent = clawt_mailbox_router_send_to(self->router, "user", target,
		                                    body, task_id, 0,
		                                    &send_error) >= 0;

		if (!sent)
			g_warning("task %s: the comment was kept but not delivered: "
			          "%s", task_id, send_error->message);

		if (told != NULL)
			*told = sent;
	}

	if (task != NULL)
		publish_note(self, task, author);

	return note_id;
}

/*
 * Tells the clients a task's thread grew.  Its own event rather than
 * ::task-changed, which settle notices and incidents hang off: a note on
 * a finished task must not send its delegator the result a second time.
 */
static void
publish_note(
	ClawtDaemon *self,
	ClawtTask   *task,
	const gchar *author
){
	g_autoptr(ClawtEvent) event = NULL;

	if (self->bus == NULL)
		return;

	event = clawt_event_new("task.noted", clawt_task_get_id(task));

	if (author != NULL)
		clawt_event_set_detail(event, "author", author);
	if (clawt_task_get_assignee(task) != NULL)
		clawt_event_set_detail(event, "agent", clawt_task_get_assignee(task));

	clawt_event_bus_publish(self->bus, event);
}

/*
 * Links @task to every id in @after (comma- or space-separated) and, if
 * any of them has not completed, holds it.  Nothing to do for no ids.
 */
static gboolean
link_after(
	ClawtDaemon  *self,
	ClawtTask    *task,
	const gchar  *after,
	GError      **error
){
	g_auto(GStrv) ids = NULL;
	g_autoptr(GPtrArray) open = NULL;
	guint i;

	if (after == NULL || *after == '\0')
		return TRUE;

	ids = g_strsplit_set(after, ", \t\n", -1);

	for (i = 0; ids[i] != NULL; i++) {
		if (*ids[i] == '\0')
			continue;

		if (!clawt_task_manager_link(self->tasks, clawt_task_get_id(task),
		                             ids[i], error))
			return FALSE;
	}

	open = clawt_task_manager_waiting_on(self->tasks, clawt_task_get_id(task));

	if (open->len > 0)
		clawt_task_manager_block(self->tasks, clawt_task_get_id(task));

	return TRUE;
}

static JsonNode *handle_checkout_request(ClawtDaemon *self,
                                         const gchar *kind,
                                         JsonNode    *request,
                                         JsonObject  *payload);

/* ── Waiting on somebody (clawtilla_task_progress waiting_on) ─────── */

/*
 * A task's assignee now waits on another agent: that agent is asked,
 * once, in its direct room with the assignee, so its answer goes back
 * to the assignee as an ordinary reply -- and that reply is what ends
 * the wait (on_answer_queued() below).  `operator` and `external:` are
 * told by the task.changed event and the mirrored ticket, not here --
 * the operator has their own notifiers and a service cannot read.
 *
 * Returns why the agent could not be asked, or NULL.  The manager then
 * undoes the wait, so a task is never blocked on a question that never
 * went out, and the tool that asked says so instead of "sent".
 */
static gchar *
on_task_waiting(ClawtTaskManager *tasks, const gchar *task_id,
                gint depth, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    ClawtTask *task = clawt_task_manager_get(tasks, task_id);
    const gchar *party;
    const gchar *assignee;
    const gchar *what;
    g_autoptr(ClawtMessage) message = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *clip = NULL;
    ClawtRoom *room;

    if (task == NULL)
        return NULL;

    party = clawt_task_get_waiting_on(task);
    assignee = clawt_task_get_assignee(task);

    /* Nobody in this fleet to ask: nothing to send, nothing to fail. */
    if (party == NULL || assignee == NULL ||
        g_strcmp0(party, "operator") == 0 ||
        g_str_has_prefix(party, "external:"))
        return NULL;

    if (clawt_agent_manager_get(self->agents, party) == NULL)
        return g_strdup_printf("there is no agent %s in this fleet", party);

    if (self->router == NULL || self->rooms == NULL)
        return g_strdup("the daemon is not routing messages yet");

    room = clawt_room_manager_get_direct(self->rooms, assignee, party);
    if (room == NULL)
        return g_strdup_printf("there is no direct room between %s and %s",
                               assignee, party);

    what = clawt_task_get_title(task);
    if (what == NULL) {
        clip = clawt_utf8_truncate(clawt_task_get_prompt(task), 120, FALSE);
        what = clip;
    }

    body = g_strdup_printf(
        "%s cannot go on with task %s (\"%s\") until you answer:\n\n%s\n\n"
        "[clawtilla] Reply here and %s gets your answer. The task is "
        "blocked on you until then.",
        assignee, task_id, what != NULL ? what : "",
        clawt_task_get_question(task) != NULL
            ? clawt_task_get_question(task) : "", assignee);

    message = clawt_message_new(clawt_room_get_id(room), assignee, body);
    clawt_message_set_only_for(message, party);

    /*
     * The caller's turn plus one, the same figure message_agent stamps.
     * Left at 0, check_hops() read a fresh chain, and rewording the
     * question kept the exchange going past max_hops.
     */
    clawt_message_set_depth(message, depth);

    /*
     * Logged, not warned: the refusal goes back to the assignee as the
     * tool's answer and onto the task as a note, so it is handled.
     */
    if (clawt_mailbox_router_send(self->router, message, &error) < 0) {
        g_message("daemon: task %s would wait on %s, who could not be "
                  "asked: %s", task_id, party, error->message);
        return g_strdup(error->message);
    }

    return NULL;
}

/*
 * An answer from the agent a task waits on, queued for the task's
 * assignee in their direct room.
 *
 * The question went out as an ordinary room message, so the answer comes
 * back as one, in a turn that is not the task's.  Nothing else ended the
 * wait -- a blocked task is not "active", so no turn end completes it and
 * no turn timeout is about it -- and an assignee that read the answer and
 * ended its turn without a task tool left the task blocked for good.  So
 * the answer itself resumes every task of the assignee's waiting on that
 * agent, and ::task-resumed wakes the assignee in the task's room, in a
 * turn that is the task's.
 *
 * Matched on the router's mailbox.queued, so only an answer that was
 * really queued for the assignee counts, and on nothing kept in memory:
 * a task restored blocked after a restart is resumed by the answer the
 * same way.  Run from an idle, not inside the router's send that
 * published the event, so the wake is not routed from inside that send.
 */
typedef struct {
    ClawtDaemon *daemon;    /* a reference */
    gchar       *assignee;
    gchar       *party;
} Answered;

static void
answered_free(gpointer data)
{
    Answered *answered = data;

    g_object_unref(answered->daemon);
    g_free(answered->assignee);
    g_free(answered->party);
    g_free(answered);
}

/* The assignee's tasks blocked on @party; borrowed tasks, in a container. */
static GPtrArray *
tasks_waiting_for(ClawtDaemon *self, const gchar *assignee,
                  const gchar *party)
{
    g_autoptr(GPtrArray) mine = NULL;
    GPtrArray *out = g_ptr_array_new();
    guint i;

    if (self->tasks == NULL)
        return out;

    mine = clawt_task_manager_list(self->tasks, assignee, FALSE);

    for (i = 0; mine != NULL && i < mine->len; i++) {
        ClawtTask *task = g_ptr_array_index(mine, i);

        if (clawt_task_get_state(task) == CLAWT_TASK_BLOCKED &&
            g_strcmp0(clawt_task_get_waiting_on(task), party) == 0)
            g_ptr_array_add(out, task);
    }

    return out;
}

static gboolean
on_answer_idle(gpointer user_data)
{
    Answered *answered = user_data;
    ClawtDaemon *self = answered->daemon;
    g_autoptr(GPtrArray) waiting = NULL;
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *by = NULL;
    guint i;

    waiting = tasks_waiting_for(self, answered->assignee, answered->party);

    /* Ids first: resuming emits signals that may change the list. */
    for (i = 0; i < waiting->len; i++)
        g_ptr_array_add(ids, g_strdup(clawt_task_get_id(
            g_ptr_array_index(waiting, i))));

    by = g_strdup_printf("%s answered you in your direct room",
                         answered->party);

    for (i = 0; i < ids->len; i++)
        clawt_task_manager_resume(self->tasks, g_ptr_array_index(ids, i),
                                  answered->party, by);

    return G_SOURCE_REMOVE;
}

static void
on_answer_queued(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    const gchar *assignee;
    const gchar *party;
    const gchar *room;
    g_autofree gchar *direct = NULL;
    g_autoptr(GPtrArray) waiting = NULL;
    Answered *answered;
    GSource *source;

    (void)bus;

    if (g_strcmp0(clawt_event_get_kind(event), "mailbox.queued") != 0)
        return;

    assignee = clawt_event_get_subject(event);
    party = clawt_event_get_detail(event, "from");
    room = clawt_event_get_detail(event, "room");

    if (assignee == NULL || party == NULL || room == NULL ||
        self->agents == NULL ||
        clawt_agent_manager_get(self->agents, party) == NULL)
        return;

    /* Their direct room: where the question was asked, so where it is answered. */
    direct = clawt_room_manager_direct_id(assignee, party);
    if (g_strcmp0(direct, room) != 0)
        return;

    /* Checked now so that ordinary mail does not schedule anything. */
    waiting = tasks_waiting_for(self, assignee, party);
    if (waiting->len == 0)
        return;

    answered = g_new0(Answered, 1);
    answered->daemon = g_object_ref(self);
    answered->assignee = g_strdup(assignee);
    answered->party = g_strdup(party);

    source = g_idle_source_new();
    g_source_set_callback(source, on_answer_idle, answered, answered_free);
    g_source_attach(source, self->main_context);
    g_source_unref(source);
}

/*
 * The wait is over, said by somebody other than the assignee: the
 * operator's `task unblock`, or a service coming back.  The assignee is
 * woken in the task's room, since the work is back with it.
 */
static void
on_task_resumed(ClawtTaskManager *tasks, const gchar *task_id,
                const gchar *waiting_on, const gchar *by,
                gpointer user_data)
{
    ClawtDaemon *self = user_data;
    ClawtTask *task = clawt_task_manager_get(tasks, task_id);
    g_autoptr(ClawtMessage) message = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *body = NULL;
    const gchar *assignee;
    ClawtRoom *room = NULL;

    if (task == NULL || self->router == NULL || self->rooms == NULL)
        return;

    assignee = clawt_task_get_assignee(task);
    if (assignee == NULL)
        return;

    if (clawt_task_get_room(task) != NULL)
        room = clawt_room_manager_get(self->rooms, clawt_task_get_room(task));
    if (room == NULL && clawt_task_get_origin(task) != NULL)
        room = clawt_room_manager_get_direct(self->rooms,
                                             clawt_task_get_origin(task),
                                             assignee);
    if (room == NULL)
        return;

    body = g_strdup_printf(
        "[clawtilla] Task %s no longer waits on %s%s%s. It is running and "
        "yours again: carry on with it.",
        task_id, waiting_on != NULL ? waiting_on : "anybody",
        by != NULL ? " -- " : "", by != NULL ? by : "");

    message = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER,
                                body);
    clawt_message_set_task_id(message, task_id);
    clawt_message_set_only_for(message, assignee);

    if (clawt_mailbox_router_send(self->router, message, &error) < 0)
        g_warning("daemon: task %s resumed and %s could not be told: %s",
                  task_id, assignee, error->message);
}

void
clawt_daemon_tasks_watch_waits(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->tasks == NULL)
        return;

    g_signal_connect(self->tasks, "task-waiting",
                     G_CALLBACK(on_task_waiting), self);
    g_signal_connect(self->tasks, "task-resumed",
                     G_CALLBACK(on_task_resumed), self);

    if (self->bus != NULL && self->waits_bus_handler == 0)
        self->waits_bus_handler = g_signal_connect(
            self->bus, "event", G_CALLBACK(on_answer_queued), self);
}

void
clawt_daemon_tasks_unwatch_waits(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->bus != NULL && self->waits_bus_handler != 0)
        g_signal_handler_disconnect(self->bus, self->waits_bus_handler);
    self->waits_bus_handler = 0;
}

JsonNode *
clawt_daemon_handle_task(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /* ── tasks ── */
    if (g_strcmp0(kind, "task.list") == 0) {
        g_autoptr(GPtrArray) tasks = NULL;
        gboolean history = clawt_ipc_payload_boolean(payload, "history",
                                                     FALSE);
        guint i;

        /*
         * Two lists.  Without `history`, the live set from memory: every
         * task that has not finished and those that finished in the last
         * day, which is what a Tasks page shows.  With it, every task the
         * store still holds, newest first and a page at a time -- `before`
         * is the paging cursor, the created_at of the last task of the
         * previous page.
         */
        if (history) {
            g_autoptr(GError) error = NULL;
            ClawtTaskQuery query = { NULL, -1, 0, 0 };
            const gchar *state = clawt_ipc_payload_string(payload, "state");
            gint value;

            query.assignee = clawt_ipc_payload_string(payload, "agent");
            query.before = clawt_ipc_payload_int(payload, "before", 0);
            query.limit = (guint)CLAMP(
                clawt_ipc_payload_int(payload, "limit",
                                      CLAWT_TASK_STORE_PAGE), 1, 1000);

            if (state != NULL) {
                if (!clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state,
                                          &value))
                    return clawt_ipc_error_new(
                        request, CLAWT_ERROR_INVALID_ARGUMENT,
                        "state must be a task state, such as failed");

                query.state = value;
            }

            tasks = clawt_task_manager_list_history(self->tasks, &query,
                                                    &error);

            if (tasks == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           error->message);
        } else {
            tasks = clawt_task_manager_list(
                self->tasks, clawt_ipc_payload_string(payload, "agent"),
                clawt_ipc_payload_boolean(payload, "all", TRUE));

            /*
             * Borrowed from the manager (the array frees nothing), where
             * the history branch's are copies the array owns: either way
             * the array is all there is to free.
             */
        }

        json_builder_begin_object(builder);

        /*
         * When this daemon started, kept for clients that still print
         * it; and how far back the list reaches now that tasks are kept,
         * which is what clawt_task_list_caption() says.
         */
        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder, self->started_at);
        json_builder_set_member_name(builder, "kept");
        json_builder_add_boolean_value(builder, self->task_store != NULL);
        json_builder_set_member_name(builder, "retention_days");
        json_builder_add_int_value(builder, clawt_config_get_int(
            self->config, "orchestration.task_retention_days"));

        json_builder_set_member_name(builder, "tasks");
        json_builder_begin_array(builder);

        for (i = 0; i < tasks->len; i++)
            clawt_daemon_add_task_object(builder, g_ptr_array_index(tasks, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.diff") == 0 ||
        g_strcmp0(kind, "task.checkout_remove") == 0)
        return handle_checkout_request(self, kind, request, payload);

    /*
     * The operator's Retry on a task the mirror gave up on, or found
     * unmatched: its dead rows queued again, and the task looked at
     * afresh.
     */
    if (g_strcmp0(kind, "task.venture_retry") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        g_autoptr(ClawtTask) stored = NULL;
        guint revived;

        /* Kept tasks too, not only the live ones in memory. */
        if (task_id != NULL && self->task_store != NULL &&
            clawt_task_manager_get(self->tasks, task_id) == NULL)
            stored = clawt_task_store_get(self->task_store, task_id);

        if (task_id == NULL ||
            (clawt_task_manager_get(self->tasks, task_id) == NULL &&
             stored == NULL))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such task");

        if (self->venture_mirror == NULL ||
            clawt_venture_mirror_account_count(self->venture_mirror) == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "no venture connector has "
                                       "mirror_tasks on");

        revived = clawt_daemon_venture_mirror_retry(self, task_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        json_builder_add_string_value(builder, task_id);
        json_builder_set_member_name(builder, "queued");
        json_builder_add_int_value(builder, revived);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.show") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        ClawtTask *task = (task_id != NULL)
                          ? clawt_task_manager_get(self->tasks, task_id)
                          : NULL;

        if (task == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such task");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        clawt_daemon_add_task_object(builder, task);

        /*
         * Its thread: what was said about it -- comments, progress,
         * results.  Always, because it is short and it is what a person
         * opening a task wants to read; absent without a store.
         */
        {
            JsonNode *notes = clawt_task_manager_notes(self->tasks, task_id,
                                                       NULL);

            if (notes != NULL) {
                json_builder_set_member_name(builder, "notes");
                json_builder_add_value(builder, notes);
            }
        }

        /*
         * Its VENTURE ticket, when the mirror has looked at it: the
         * ticket and its page, or why it is not mirrored, or why the
         * mirror gave up (0.44.0).
         */
        {
            JsonNode *venture = clawt_daemon_venture_mirror_describe(self,
                                                                     task_id);

            if (venture != NULL) {
                json_builder_set_member_name(builder, "venture");
                json_builder_add_value(builder, venture);
            }
        }

        /*
         * Its history, when asked for: every state it was left in and
         * what moved it there.  Absent without a store rather than an
         * error -- the task itself is still worth answering with.
         */
        if (clawt_ipc_payload_boolean(payload, "journal", FALSE)) {
            JsonNode *journal = clawt_task_manager_journal(
                self->tasks, task_id, NULL);

            if (journal != NULL) {
                json_builder_set_member_name(builder, "journal");
                json_builder_add_value(builder, journal);
            }
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * The operator's own tasks.  "user" throughout, because the clients
     * are the operator's hands: the assignee is told the work came from
     * them, and a result goes back to them rather than to an agent.
     */
    if (g_strcmp0(kind, "task.create") == 0) {
        const gchar *prompt = clawt_ipc_payload_string(payload, "prompt");
        const gchar *title = clawt_ipc_payload_string(payload, "title");
        const gchar *agent = clawt_ipc_payload_string(payload, "agent");
        g_autoptr(GError) error = NULL;
        ClawtTask *task;

        if (prompt == NULL || *prompt == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "what is the task? (prompt)");

        if (agent != NULL && *agent == '\0')
            agent = NULL;

        /*
         * Work for another fleet's agent (0.69.0): a proxy task here, the
         * real one there, and the two kept in step over the link.
         */
        if (agent != NULL && strchr(agent, '@') != NULL) {
            g_autofree gchar *task_id = clawt_daemon_federation_delegate(
                self, "user", agent, prompt, NULL, NULL, &error);

            if (task_id == NULL)
                return clawt_ipc_error_new(request, error->code, error->message);
            task = clawt_task_manager_get(self->tasks, task_id);
            if (title != NULL && *title != '\0') {
                clawt_task_set_title(task, title);
                clawt_task_manager_save(self->tasks, task, NULL);
            }
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "task");
            clawt_daemon_add_task_object(builder, task);
            json_builder_end_object(builder);
            return clawt_ipc_response_new(request, json_builder_get_root(builder));
        }

        if (agent != NULL &&
            clawt_agent_manager_get(self->agents, agent) == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no agent by that name");

        task = clawt_task_manager_create(self->tasks, "user", agent, prompt,
                                         NULL, &error);

        if (task == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (title != NULL && *title != '\0') {
            clawt_task_set_title(task, title);
            clawt_task_manager_save(self->tasks, task, NULL);
        }

        /*
         * Who reviews it: the operator's `review`, else the assignee's
         * agents.review_by.  The delegator here is the operator, so
         * `delegator` is the operator too.
         */
        if (agent != NULL) {
            const gchar *spec = clawt_ipc_payload_string(payload, "review");
            g_autofree gchar *reviewer = NULL;

            if (spec == NULL) {
                ClawtAgentConfig *to = clawt_config_get_agent(self->config,
                                                              agent);

                spec = (to != NULL)
                    ? clawt_agent_config_get_string(to, "review_by") : NULL;
            }

            reviewer = clawt_task_resolve_reviewer(self->config, spec, "user",
                                                   agent, &error);

            if (error != NULL) {
                clawt_task_manager_cancel(self->tasks,
                                          clawt_task_get_id(task),
                                          error->message, "user");
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
            }

            if (reviewer != NULL) {
                clawt_task_set_reviewer(task, reviewer);
                clawt_task_manager_save(self->tasks, task, NULL);
            }
        }

        /*
         * A checkout of its own, when the operator named a repository.
         * Only with an assignee: the checkout lives in its workspace, and
         * a backlog task has none yet -- so that is refused, not put off.
         */
        {
            const gchar *repo = clawt_ipc_payload_string(payload, "repo");

            if (repo != NULL && *repo != '\0' && agent == NULL) {
                clawt_task_manager_cancel(self->tasks,
                                          clawt_task_get_id(task),
                                          "a checkout needs an assignee",
                                          "user");
                return clawt_ipc_error_new(request,
                    CLAWT_ERROR_INVALID_ARGUMENT,
                    "a repository needs an agent to check it out for; "
                    "assign the task, or leave the repository out");
            }

            if (repo != NULL && *repo != '\0' &&
                !clawt_task_checkout_open(task,
                    clawt_config_get_agent(self->config, agent), repo,
                    clawt_ipc_payload_string(payload, "base"), &error)) {
                clawt_task_manager_cancel(self->tasks,
                                          clawt_task_get_id(task),
                                          error->message, "user");
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
            }

            if (repo != NULL && *repo != '\0')
                clawt_task_manager_save(self->tasks, task, NULL);
        }

        /*
         * Its project, named or matched by its repository; refused, with
         * the task cancelled, when the project has stopped taking work.
         */
        if (!clawt_daemon_project_place(
                self, task, clawt_ipc_payload_string(payload, "project"),
                NULL, clawt_ipc_payload_string(payload, "review") != NULL,
                &error)) {
            clawt_task_manager_cancel(self->tasks, clawt_task_get_id(task),
                                      error->message, "user");
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        clawt_task_manager_save(self->tasks, task, NULL);

        /*
         * What it waits for: linked, and while any of it is unfinished
         * the task is held -- with an assignee, its delivery is kept to
         * be sent as the operator's when the last one completes; without,
         * it goes back to the backlog then.
         */
        if (!link_after(self, task,
                        clawt_ipc_payload_string(payload, "after"), &error)) {
            clawt_task_manager_cancel(self->tasks, clawt_task_get_id(task),
                                      "it could not wait as asked", "user");
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (clawt_task_get_state(task) == CLAWT_TASK_BLOCKED) {
            if (agent != NULL) {
                g_autofree gchar *guidance =
                    clawt_task_assignment_guidance_for(task);
                g_autofree gchar *body = g_strconcat(
                    clawt_task_get_prompt(task), guidance, NULL);
                g_autoptr(JsonNode) held = clawt_task_build_held_delivery(
                    "router", "user", agent, body, 0, CLAWT_PRIORITY_NORMAL,
                    NULL);

                if (!clawt_task_manager_hold_delivery(
                        self->tasks, clawt_task_get_id(task), held, &error))
                    return clawt_ipc_error_new(request, error->code,
                                               error->message);
            }
        } else if (agent != NULL &&
                   !clawt_daemon_deliver_operator_task(self, task, &error)) {
            /* With an assignee it goes to them now; without, it waits. */
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        clawt_daemon_add_task_object(builder, task);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * The operator changing what a task waits for.  Linking holds a
     * backlog task or adds to a held one; work already with its
     * assignee cannot be held back.  Unlinking may release it, which the
     * manager's blockers-changed does.
     */
    if (g_strcmp0(kind, "task.link") == 0 ||
        g_strcmp0(kind, "task.unlink") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        const gchar *blocker = clawt_ipc_payload_string(payload, "after");
        gboolean linking = g_strcmp0(kind, "task.link") == 0;
        g_autoptr(GError) error = NULL;
        ClawtTask *task;

        if (task_id == NULL || blocker == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task, and which task should "
                                       "it wait for? (task, after)");

        task = clawt_task_manager_get(self->tasks, task_id);

        if (task == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such task");

        if (linking) {
            ClawtTaskState state = clawt_task_get_state(task);

            if (state != CLAWT_TASK_UNASSIGNED && state != CLAWT_TASK_BLOCKED)
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_INVALID_ARGUMENT,
                    "only a task on the backlog, or one already waiting, "
                    "can be made to wait: this one's work has been handed "
                    "over");

            if (!link_after(self, task, blocker, &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        } else if (!clawt_task_manager_unlink(self->tasks, task_id, blocker,
                                              &error)) {
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        clawt_daemon_add_task_object(builder,
                                     clawt_task_manager_get(self->tasks,
                                                            task_id));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * The operator's verdict on a result in review -- theirs to give
     * whoever the reviewer is.  The same as answering the inbox's
     * decision, or an agent reviewer's clawtilla_task_review.
     */
    if (g_strcmp0(kind, "task.review") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        const gchar *verdict = clawt_ipc_payload_string(payload, "verdict");
        g_autoptr(GError) error = NULL;
        g_autofree gchar *said = NULL;

        if (task_id == NULL ||
            (g_strcmp0(verdict, "accept") != 0 &&
             g_strcmp0(verdict, "changes") != 0))
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task, and accept or changes?");

        said = clawt_daemon_tasks_review(
            self, "user", task_id, g_strcmp0(verdict, "accept") == 0,
            clawt_ipc_payload_string(payload, "notes"), &error);

        if (said == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "said", said);
        json_builder_set_member_name(builder, "task");
        clawt_daemon_add_task_object(builder,
                                     clawt_task_manager_get(self->tasks,
                                                            task_id));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.assign") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        const gchar *agent = clawt_ipc_payload_string(payload, "agent");
        g_autoptr(GError) error = NULL;
        ClawtTask *task;

        if (task_id == NULL || agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task, and to whom?");

        if (clawt_agent_manager_get(self->agents, agent) == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no agent by that name");

        if (!clawt_task_manager_assign(self->tasks, task_id, agent, "user",
                                       &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        task = clawt_task_manager_get(self->tasks, task_id);

        if (!clawt_daemon_deliver_operator_task(self, task, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        clawt_daemon_add_task_object(builder, task);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.comment") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        const gchar *text = clawt_ipc_payload_string(payload, "text");
        g_autoptr(GError) error = NULL;
        gboolean told = FALSE;
        gint64 note_id;

        if (task_id == NULL || text == NULL || *text == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task, and what to say?");

        note_id = clawt_daemon_task_comment(self, task_id, "user", NULL, text,
                                            &told, &error);

        if (note_id == 0)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "note");
        json_builder_add_int_value(builder, note_id);
        json_builder_set_member_name(builder, "delivered");
        json_builder_add_boolean_value(builder, told);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.cancel") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        guint cancelled;

        if (task_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task?");

        /*
         * "user", because the clients are the operator's hands.  The
         * delegating agent is then told its task was cancelled over its
         * head -- which is the notice it is waiting on, where its own
         * cancel would be a notice about what it just did.
         */
        cancelled = clawt_task_manager_cancel(self->tasks, task_id,
                                              "cancelled from a client",
                                              "user");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "cancelled");
        json_builder_add_int_value(builder, cancelled);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * The wait a task's assignee declared is over: one task, or every
     * task waiting on one party (`on`, e.g. external:venture, when the
     * service is back).  "user" says, because the clients are the
     * operator's hands.
     */
    if (g_strcmp0(kind, "task.unblock") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        const gchar *on = clawt_ipc_payload_string(payload, "on");
        guint resumed;

        if (task_id == NULL && on == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task, or waiting on what?");

        resumed = clawt_task_manager_resume(self->tasks, task_id, on,
                                            "the operator");

        if (resumed == 0 && task_id != NULL) {
            ClawtTask *task = clawt_task_manager_get(self->tasks, task_id);

            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                task == NULL ? "there is no such task"
                : clawt_task_get_state(task) == CLAWT_TASK_BLOCKED
                    ? "that task waits on the tasks it was delegated after, "
                      "not on somebody; `task link` changes those"
                    : "that task is not waiting on anybody");
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "resumed");
        json_builder_add_int_value(builder, resumed);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── incidents ── */
    if (g_strcmp0(kind, "task.retry") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        const gchar *incident_id = clawt_ipc_payload_string(payload,
                                                            "incident");
        g_autoptr(GError) error = NULL;
        g_autofree gchar *now = NULL;
        gboolean rerun = FALSE;

        if (task_id == NULL && incident_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task or incident?");

        if (self->incidents == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "retrying is not available");

        /*
         * "user", because the clients are the operator's hands -- and
         * the operator's retry is not counted against the agents'
         * limit, since a person pressing the button has looked.
         */
        now = clawt_incident_desk_retry(
            self->incidents, task_id, incident_id, "user",
            clawt_ipc_payload_string(payload, "reason"), &rerun, &error);

        if (now == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "task", now);
        json_builder_set_member_name(builder, "rerun");
        json_builder_add_boolean_value(builder, rerun);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "incident.list") == 0) {
        gboolean all = clawt_ipc_payload_boolean(payload, "all", FALSE);
        g_autoptr(GPtrArray) incidents = NULL;
        guint i;

        json_builder_begin_object(builder);

        /*
         * The settings travel with the list, so the page that shows the
         * incidents can say who they went to without a second request.
         */
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(
            builder, clawt_config_get_boolean(
                         self->config, "orchestration.incidents.enabled"));
        clawt_daemon_add_string_member(
            builder, "to",
            clawt_incident_recipient_nick((ClawtIncidentRecipient)
                clawt_config_get_enum(self->config,
                                      "orchestration.incidents.to")));

        /* Kept with the tasks they describe; see task.list. */
        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder, self->started_at);

        json_builder_set_member_name(builder, "incidents");
        json_builder_begin_array(builder);

        if (self->incidents != NULL)
            incidents = clawt_incident_desk_list(self->incidents);

        for (i = 0; incidents != NULL && i < incidents->len; i++) {
            ClawtIncident *incident = g_ptr_array_index(incidents, i);

            if (!all && clawt_incident_is_resolved(incident))
                continue;

            add_incident_object(self, builder, incident);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "incident.configure") == 0) {
        const gchar *to = clawt_ipc_payload_string(payload, "to");
        g_autoptr(GError) error = NULL;

        /*
         * Refused where it arrives rather than saved and read back as
         * the default: an unknown recipient would otherwise become
         * `lead` with nothing to say so.
         */
        if (to != NULL) {
            gint value = 0;

            if (!clawt_enum_from_nick(CLAWT_TYPE_INCIDENT_RECIPIENT, to,
                                      &value))
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_INVALID_ARGUMENT,
                    "orchestration.incidents.to is lead, chief or none");

            clawt_config_set_string(self->config,
                                    "orchestration.incidents.to",
                                    clawt_incident_recipient_nick(
                                        (ClawtIncidentRecipient)value));
        }

        if (json_object_has_member(payload, "enabled"))
            clawt_config_set_boolean(
                self->config, "orchestration.incidents.enabled",
                clawt_ipc_payload_boolean(payload, "enabled", TRUE));

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "incident.configured", NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(
            builder, clawt_config_get_boolean(
                         self->config, "orchestration.incidents.enabled"));
        clawt_daemon_add_string_member(
            builder, "to",
            clawt_incident_recipient_nick((ClawtIncidentRecipient)
                clawt_config_get_enum(self->config,
                                      "orchestration.incidents.to")));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}

/* ── The task store ──────────────────────────────────────────────── */

/*
 * Opens tasks.db and restores the live set into the manager.
 *
 * Called straight after the manager is built and before anything
 * connects to ::task-changed, which is what keeps the restore silent: a
 * task read back must not send its settle notice, raise its incident or
 * reach a client as news.  A store that will not open is said once and
 * survived -- the daemon runs as it always did, in memory -- because a
 * damaged file must not cost the operator their fleet.
 */
void
clawt_daemon_tasks_setup(
	ClawtDaemon *self
){
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	guint loaded;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	path = g_build_filename(self->state_dir, "tasks.db", NULL);
	self->task_store = clawt_task_store_new(path, &error);

	if (self->task_store == NULL) {
		g_warning("tasks: %s: %s; tasks and incidents are held in memory "
		          "only until it opens", path,
		          error != NULL ? error->message : "unknown error");
		return;
	}

	clawt_task_manager_set_store(self->tasks, self->task_store);
	clawt_daemon_set_wire_tasks(self->tasks);

	loaded = clawt_task_manager_load(
		self->tasks, now - CLAWT_DAEMON_TASK_LIVE_SECONDS, &error);

	if (error != NULL)
		g_warning("tasks: could not read back the live tasks: %s",
		          error->message);
	else if (loaded > 0)
		g_message("tasks: %u restored from %s", loaded, path);

	/*
	 * A task with a room of its own (`thread: new`) points at a room the
	 * room manager holds in memory: made again here, with the same two
	 * members, and it reads its transcript back as it is inserted.
	 * Without this the task would name a room nothing can open, and its
	 * assignee's next message would go nowhere.
	 */
	if (loaded > 0 && self->rooms != NULL) {
		g_autoptr(GPtrArray) live = clawt_task_manager_list(self->tasks, NULL,
		                                                    TRUE);
		guint i;

		for (i = 0; i < live->len; i++) {
			ClawtTask *task = g_ptr_array_index(live, i);

			if (clawt_task_get_thread_room(task) == NULL ||
			    clawt_task_get_origin(task) == NULL ||
			    clawt_task_get_assignee(task) == NULL)
				continue;

			clawt_room_manager_get_task(self->rooms, clawt_task_get_id(task),
			                            clawt_task_get_origin(task),
			                            clawt_task_get_assignee(task));
		}
	}
}

/*
 * The incidents the last daemon left, as it left them.  After the desk
 * exists and before anything can raise one, so a restored incident and
 * a new one for the same run are one incident counting up.
 */
void
clawt_daemon_tasks_restore_incidents(
	ClawtDaemon *self
){
	guint restored;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->incidents == NULL || self->task_store == NULL)
		return;

	clawt_incident_desk_set_store(self->incidents, self->task_store);
	restored = clawt_incident_desk_restore(self->incidents);

	if (restored > 0)
		g_message("tasks: %u incident%s restored", restored,
		          restored == 1 ? "" : "s");
}

/*
 * Whether anything is still carrying @task forward after a restart.
 *
 * A task that has not finished is alive when any of these holds, and
 * each is something that will move it without anybody's help:
 *
 *   - its assignee armed a completion hold (it said it is not done);
 *   - a task it handed on has not finished (the child's settle notice
 *     wakes it);
 *   - its assignee's mailbox holds a pending or leased item for it,
 *     which includes a turn the restart cut short and turn recovery
 *     handed back;
 *   - its assignee has a wait pending (the wait's ending wakes it);
 *   - a handoff of it is still queued.
 *
 * Read after turn recovery, so a cut-short turn has already become a
 * queued item or an interrupted notice.
 */
static gboolean
task_is_carried(
	ClawtDaemon *self,
	ClawtTask   *task
){
	const gchar *task_id = clawt_task_get_id(task);
	const gchar *assignee = clawt_task_get_assignee(task);
	ClawtAgent *agent;
	ClawtMailbox *mailbox;

	if (clawt_task_get_holds_completion(task))
		return TRUE;

	/*
	 * A proxy for work another fleet is doing (0.69.0) is carried by
	 * that fleet, whatever this one did: its state comes back over the
	 * link when the fleet answers again.
	 */
	if (clawt_task_get_remote_peer(task) != NULL &&
	    clawt_sender_kind_of(clawt_task_get_origin(task)) != CLAWT_SENDER_REMOTE)
		return TRUE;

	if (clawt_task_manager_count_unfinished_children(self->tasks,
	                                                 task_id) > 0)
		return TRUE;

	if (self->handoffs != NULL) {
		g_autoptr(GPtrArray) moves =
			clawt_handoff_store_for_task(self->handoffs, task_id);
		guint i;

		for (i = 0; moves != NULL && i < moves->len; i++)
			if (clawt_handoff_get_state(g_ptr_array_index(moves, i)) ==
			    CLAWT_HANDOFF_QUEUED)
				return TRUE;
	}

	if (assignee == NULL)
		return FALSE;

	if (self->waits != NULL &&
	    clawt_wait_manager_count(self->waits, assignee) > 0)
		return TRUE;

	agent = clawt_agent_manager_get(self->agents, assignee);
	mailbox = agent != NULL ? clawt_agent_get_mailbox(agent) : NULL;

	if (mailbox != NULL) {
		ClawtMailboxFilter filter = { -1, 0, TRUE };
		g_autoptr(GPtrArray) items = clawt_mailbox_list(mailbox, &filter);
		guint i;

		for (i = 0; items != NULL && i < items->len; i++) {
			ClawtMailboxItem *item = g_ptr_array_index(items, i);
			ClawtMailboxState state = clawt_mailbox_item_get_state(item);

			if ((state == CLAWT_MAILBOX_PENDING ||
			     state == CLAWT_MAILBOX_LEASED) &&
			    g_strcmp0(clawt_mailbox_item_get_task_id(item),
			              task_id) == 0)
				return TRUE;
		}
	}

	return FALSE;
}

/*
 * Seeds the loop guard with what each restored task has already cost,
 * charged up its tree as a live turn is, so
 * orchestration.task_budget_usd keeps counting where it left off:
 * a budget that reset on every restart would be a budget a crash loop
 * never meets.
 */
static void
seed_task_spend(
	ClawtDaemon *self,
	GPtrArray   *live
){
	g_autoptr(GHashTable) ids = NULL;
	g_autoptr(GHashTable) spend = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	GPtrArray *agents;
	guint i;

	if (self->guard == NULL || live == NULL || live->len == 0)
		return;

	ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	spend = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < live->len; i++)
		g_hash_table_add(ids, g_strdup(clawt_task_get_id(
			g_ptr_array_index(live, i))));

	/* transfer none: the manager's own list */
	agents = clawt_agent_manager_list(self->agents);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		const gchar *agent_id = clawt_agent_get_id(
			g_ptr_array_index(agents, i));
		g_autofree gchar *state_dir = clawt_config_agent_state_dir(
			self->config, agent_id);
		g_autofree gchar *db_path = NULL;
		g_autoptr(GError) error = NULL;

		if (state_dir == NULL)
			continue;

		db_path = clawt_usage_database_path(state_dir);

		if (!clawt_usage_task_spend(db_path, ids, spend, &error))
			g_warning("%s: could not read what its tasks spent: %s",
			          agent_id, error != NULL ? error->message : "unknown");
	}

	g_hash_table_iter_init(&iter, spend);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		gint64 micros = (gint64)GPOINTER_TO_SIZE(value);

		/*
		 * Up the tree, as a live charge is, so a restart leaves every
		 * figure covering its subtree.
		 */
		if (micros > 0)
			clawt_daemon_tree_charge(self, key,
			                         (gdouble)micros / 1000000.0);
	}
}

guint
clawt_daemon_tasks_reconcile(
	ClawtDaemon *self
){
	g_autoptr(GPtrArray) live = NULL;
	g_autoptr(GPtrArray) orphans = NULL;
	guint i;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

	if (self->task_store == NULL)
		return 0;

	live = clawt_task_manager_list(self->tasks, NULL, FALSE);

	/*
	 * Every task, finished ones too: a subtask that ended before the
	 * restart still spent against the live parent above it.
	 */
	{
		g_autoptr(GPtrArray) every = clawt_task_manager_list(self->tasks, NULL, TRUE);

		seed_task_spend(self, every);
	}

	/*
	 * A held task whose blockers all completed while nobody was
	 * listening -- the daemon stopped between the two -- goes now.
	 */
	for (i = 0; i < live->len; i++) {
		ClawtTask *task = g_ptr_array_index(live, i);

		if (clawt_task_get_state(task) == CLAWT_TASK_BLOCKED)
			clawt_daemon_tasks_release_ready(self, clawt_task_get_id(task));
	}

	/*
	 * Decided first and failed after, because failing a task emits and
	 * a handler could change what the rest of the walk would see.
	 */
	orphans = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < live->len; i++) {
		ClawtTask *task = g_ptr_array_index(live, i);

		/*
		 * Only what was with an assignee.  A backlog item, a task held
		 * for its blockers and one in review were waiting on somebody
		 * other than an agent's turn, and a restart changes nothing
		 * about that.
		 */
		if (!clawt_task_state_is_active(clawt_task_get_state(task)))
			continue;

		if (!task_is_carried(self, task))
			g_ptr_array_add(orphans, g_strdup(clawt_task_get_id(task)));
	}

	/*
	 * Failed rather than stalled: nothing stopped it on a limit, the
	 * daemon went away under it -- the same answer an agent crash gets,
	 * and the one a delegator knows to retry.  Quietly, as far as the
	 * incident desk goes, as when an agent's tasks are orphaned: one
	 * restart is not a dozen problem reports.  The settle notices still
	 * go, each to the room it was asked from, and the router folds a
	 * delegator's several into one turn.
	 */
	if (orphans->len > 0 && self->incidents != NULL)
		clawt_incident_desk_quiet_begin(self->incidents);

	for (i = 0; i < orphans->len; i++)
		clawt_task_manager_fail(self->tasks, g_ptr_array_index(orphans, i),
		                        "the daemon restarted while this ran, and "
		                        "nothing was left to carry it on: no turn, "
		                        "queued message, wait, handoff or unfinished "
		                        "task of its own. Retry it to run it again.");

	if (orphans->len > 0 && self->incidents != NULL)
		clawt_incident_desk_quiet_end(self->incidents);

	if (orphans->len > 0)
		g_message("tasks: %u left with nothing carrying them on after the "
		          "restart, and failed so their delegators know",
		          orphans->len);

	return orphans->len;
}

/*
 * Moves finished tasks out of memory after a day, and out of the store
 * after the retention window.  Hourly: neither is urgent, and a task
 * that finished a minute ago belongs on everybody's list.
 */
/* ── Checkouts ───────────────────────────────────────────────────── */

/* The largest diff answered; the rest is cut at a line, and said so. */
#define CLAWT_DAEMON_DIFF_MAX_BYTES (512 * 1024)

/*
 * Removes a task's checkout, keeping its branch, and says so in its
 * history.  Refused for work still under way, and -- unless @force --
 * for a checkout with uncommitted changes, which would go with it.
 */
gboolean
clawt_daemon_tasks_remove_checkout(
	ClawtDaemon  *self,
	ClawtTask    *task,
	gboolean      force,
	const gchar  *who,
	GError      **error
){
	ClawtAgentConfig *config;
	g_autofree gchar *root = NULL;
	g_autofree gchar *what = NULL;
	const gchar *path;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

	path = clawt_task_get_checkout_path(task);

	if (clawt_task_get_checkout_mode(task) == CLAWT_CHECKOUT_NONE ||
	    path == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "task %s has no checkout", clawt_task_get_id(task));
		return FALSE;
	}

	if (clawt_task_state_is_active(clawt_task_get_state(task))) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "task %s is still being worked on in it; cancel it "
		            "or wait for it to finish", clawt_task_get_id(task));
		return FALSE;
	}

	if (!force && g_file_test(path, G_FILE_TEST_IS_DIR) &&
	    clawt_task_checkout_is_dirty(path)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "the checkout of task %s has uncommitted changes, "
		            "which removing it would lose; remove it with force "
		            "to do so anyway", clawt_task_get_id(task));
		return FALSE;
	}

	config = clawt_config_get_agent(self->config,
	                                clawt_task_get_assignee(task));
	root = (config != NULL) ? clawt_task_checkout_root_for(config) : NULL;

	if (root == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "%s, whose workspace holds the checkout, is no longer "
		            "configured; remove %s by hand",
		            clawt_task_get_assignee(task), path);
		return FALSE;
	}

	if (!clawt_task_checkout_remove(clawt_task_get_checkout_repo(task),
	                                root, path,
	                                clawt_task_get_checkout_branch(task),
	                                clawt_task_get_checkout_mode(task),
	                                error))
		return FALSE;

	what = g_strdup_printf("checkout removed by %s; branch %s kept in %s",
	                       who, clawt_task_get_checkout_branch(task),
	                       clawt_task_get_checkout_repo(task));
	clawt_task_manager_record(self->tasks, clawt_task_get_id(task), what);

	return TRUE;
}

/*
 * The hourly sweep: every agent's tasks/ directory, each checkout whose
 * task finished more than checkout_keep_days ago, removed if clean.  A
 * directory whose task is unknown -- pruned, or never one of ours -- is
 * left alone: there is no record of which repository to give its branch
 * back to.
 */
void
clawt_daemon_tasks_sweep_checkouts(
	ClawtDaemon *self,
	gint64       now
){
	GPtrArray *agents;
	gint64 days;
	guint i;

	days = clawt_config_get_int(self->config,
	                            "orchestration.checkout_keep_days");

	if (days <= 0 || self->tasks == NULL)
		return;

	agents = clawt_config_get_agents(self->config);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtAgentConfig *config = g_ptr_array_index(agents, i);
		g_autofree gchar *root = clawt_task_checkout_root_for(config);
		g_autoptr(GDir) dir = NULL;
		const gchar *name;

		if (root == NULL ||
		    (dir = g_dir_open(root, 0, NULL)) == NULL)
			continue;

		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *id = g_strconcat("task-", name, NULL);
			g_autoptr(GError) error = NULL;
			ClawtTask *task = clawt_task_manager_get(self->tasks, id);

			if (task == NULL ||
			    clawt_task_get_checkout_mode(task) == CLAWT_CHECKOUT_NONE ||
			    !clawt_task_is_finished(task) ||
			    clawt_task_get_finished_at(task) > now - days * 24 * 60 * 60)
				continue;

			if (!clawt_daemon_tasks_remove_checkout(self, task, FALSE,
			                                        "clawtilla", &error))
				g_message("tasks: kept the checkout of %s: %s", id,
				          error->message);
		}
	}
}

typedef struct {
	ClawtIpcPending *pending;
	gchar           *task_id;
	gboolean         from_branch;
} DiffJob;

static void
on_diffed(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	DiffJob *job = user_data;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) untracked = NULL;
	gboolean truncated = FALSE;
	g_autofree gchar *diff = clawt_task_checkout_diff_finish(
		result, &truncated, &untracked, &error);
	JsonNode *request = clawt_ipc_pending_get_request(job->pending);

	(void)source;

	if (diff == NULL) {
		clawt_ipc_pending_respond(job->pending,
			clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
			                    error->message));
	} else {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		guint i;

		json_builder_begin_object(builder);
		clawt_daemon_add_string_member(builder, "task", job->task_id);
		clawt_daemon_add_string_member(builder, "diff", diff);
		json_builder_set_member_name(builder, "truncated");
		json_builder_add_boolean_value(builder, truncated);
		json_builder_set_member_name(builder, "from_branch");
		json_builder_add_boolean_value(builder, job->from_branch);
		json_builder_set_member_name(builder, "untracked");
		json_builder_begin_array(builder);
		for (i = 0; untracked != NULL && untracked[i] != NULL; i++)
			json_builder_add_string_value(builder, untracked[i]);
		json_builder_end_array(builder);
		json_builder_end_object(builder);

		clawt_ipc_pending_respond(job->pending,
			clawt_ipc_response_new(request, json_builder_get_root(builder)));
	}

	g_free(job->task_id);
	g_free(job);
}

/* task.diff and task.checkout_remove. */
static JsonNode *
handle_checkout_request(
	ClawtDaemon *self,
	const gchar *kind,
	JsonNode    *request,
	JsonObject  *payload
){
	const gchar *task_id = clawt_ipc_payload_string(payload, "task");
	ClawtTask *task = clawt_task_manager_get(self->tasks, task_id);

	if (task == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
		                           "no such task");

	if (clawt_task_get_checkout_mode(task) == CLAWT_CHECKOUT_NONE)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
		                           "this task has no checkout: it was not "
		                           "given a repository");

	if (g_strcmp0(kind, "task.diff") == 0) {
		DiffJob *job = g_new0(DiffJob, 1);
		const gchar *path = clawt_task_get_checkout_path(task);

		/*
		 * The checkout as it stands, uncommitted work included; or, once
		 * it has been removed, the branch it left in the repository.
		 */
		job->from_branch = !g_file_test(path, G_FILE_TEST_IS_DIR);
		job->task_id = g_strdup(task_id);
		job->pending = clawt_ipc_server_defer(self->ipc_server, request);

		if (job->pending == NULL) {
			g_free(job->task_id);
			g_free(job);
			return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
			                           "this request cannot be answered "
			                           "later");
		}

		clawt_task_checkout_diff_async(
			job->from_branch ? clawt_task_get_checkout_repo(task) : path,
			clawt_task_get_checkout_base(task),
			job->from_branch ? clawt_task_get_checkout_branch(task) : NULL,
			CLAWT_DAEMON_DIFF_MAX_BYTES, NULL, on_diffed, job);

		return NULL;
	}

	{
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonBuilder) builder = json_builder_new();

		if (!clawt_daemon_tasks_remove_checkout(
		        self, task, clawt_ipc_payload_boolean(payload, "force", FALSE),
		        "user", &error))
			return clawt_ipc_error_new(request, error->code, error->message);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "task");
		clawt_daemon_add_task_object(builder, task);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}
}

static gboolean
on_task_prune_tick(
	gpointer user_data
){
	ClawtDaemon *self = user_data;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 days;

	clawt_task_manager_forget_finished(self->tasks,
	                                   now - CLAWT_DAEMON_TASK_LIVE_SECONDS);

	clawt_daemon_tasks_sweep_checkouts(self, now);

	days = clawt_config_get_int(self->config,
	                            "orchestration.task_retention_days");

	if (self->task_store != NULL && days > 0) {
		g_autoptr(GError) error = NULL;
		guint removed = clawt_task_store_prune(
			self->task_store, now - days * 24 * 60 * 60, &error);

		if (error != NULL)
			g_warning("tasks: could not prune old tasks: %s",
			          error->message);
		else if (removed > 0)
			g_message("tasks: removed %u finished more than %"
			          G_GINT64_FORMAT " days ago", removed, days);
	}

	/*
	 * The notification list on the same hourly tick: notices.keep and
	 * notices.keep_days, and any mute that has lapsed.
	 */
	clawt_daemon_notices_prune(self, now);

	return G_SOURCE_CONTINUE;
}

void
clawt_daemon_tasks_start_pruning(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->task_prune != NULL)
		return;

	on_task_prune_tick(self);

	/* On the daemon's own context; see clawt_timeout_add_seconds(). */
	self->task_prune = clawt_timeout_add_seconds(60 * 60, on_task_prune_tick,
	                                              self);
}

void
clawt_daemon_tasks_stop(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->task_prune != NULL) {
		g_source_destroy(self->task_prune);
		g_clear_pointer(&self->task_prune, g_source_unref);
	}
}

/* ── Blockers ────────────────────────────────────────────────────── */

/*
 * Hands a held task over, now that nothing holds it.
 *
 * Its delivery was kept exactly as it would have been sent, so it goes
 * the same way: a delegation through the delegation path, as the
 * delegator, at its hop depth, into the same conversation; an operator's
 * task as a message from `user`.  A send that is refused fails the task,
 * so its delegator's settle notice says why rather than leaving it
 * pending with nobody told.
 */
gboolean
clawt_daemon_tasks_release_ready(
	ClawtDaemon *self,
	const gchar *task_id
){
	g_autoptr(GPtrArray) open = NULL;
	g_autoptr(JsonNode) held = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *delivery;
	ClawtTask *task;
	gboolean sent = FALSE;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

	task = clawt_task_manager_get(self->tasks, task_id);

	if (task == NULL || clawt_task_get_state(task) != CLAWT_TASK_BLOCKED)
		return FALSE;

	open = clawt_task_manager_waiting_on(self->tasks, task_id);

	if (open->len > 0)
		return FALSE;

	/*
	 * A project that stopped taking work while this waited: the release
	 * is new work, so it is refused as a delegation would be -- the task
	 * fails with the reason, and its delivery goes with it.
	 */
	if (!clawt_daemon_project_admit(self, clawt_task_get_project(task),
	                                &error)) {
		g_autoptr(JsonNode) dropped = clawt_task_manager_take_delivery(
			self->tasks, task_id);

		clawt_task_manager_unblock(self->tasks, task_id);
		clawt_task_manager_fail(self->tasks, task_id, error->message);
		return TRUE;
	}

	held = clawt_task_manager_take_delivery(self->tasks, task_id);
	clawt_task_manager_unblock(self->tasks, task_id);

	/* Nothing to hand over: an operator's backlog task goes back there. */
	if (held == NULL || !JSON_NODE_HOLDS_OBJECT(held))
		return TRUE;

	delivery = json_node_get_object(held);

	if (g_strcmp0(json_object_get_string_member_with_default(delivery, "via",
	                                                         ""), "tool") == 0
	    && self->mcp_tools != NULL) {
		sent = clawt_mcp_tools_deliver_held(
			self->mcp_tools,
			json_object_get_string_member_with_default(delivery, "from", ""),
			json_object_get_string_member_with_default(delivery, "target", ""),
			json_object_get_string_member_with_default(delivery, "body", ""),
			task_id,
			(gint)json_object_get_int_member_with_default(delivery, "depth", 0),
			(ClawtPriority)json_object_get_int_member_with_default(
				delivery, "priority", CLAWT_PRIORITY_NORMAL),
			json_object_get_string_member_with_default(delivery, "turn_room",
			                                           NULL),
			&error);
	} else if (self->router != NULL) {
		sent = clawt_mailbox_router_send_to(
			self->router,
			json_object_get_string_member_with_default(delivery, "from", "user"),
			json_object_get_string_member_with_default(delivery, "target", ""),
			json_object_get_string_member_with_default(delivery, "body", ""),
			task_id,
			(gint)json_object_get_int_member_with_default(delivery, "depth", 0),
			&error) >= 0;
	}

	if (!sent) {
		g_autofree gchar *why = g_strdup_printf(
			"everything it waited for completed, but it could not be handed "
			"over: %s", error != NULL ? error->message : "no way to send it");

		clawt_task_manager_fail(self->tasks, task_id, why);
		return FALSE;
	}

	return TRUE;
}

/*
 * One notice to whoever is waiting on @dependent: a task it waits for
 * ended without completing, so it will not start by itself.  To the
 * delegating agent as a system notice in the conversation it delegated
 * from; for the operator's own task, a note in its thread, which every
 * client shows.
 */
static void
tell_blocker_ended(
	ClawtDaemon *self,
	ClawtTask   *dependent,
	ClawtTask   *blocker
){
	const gchar *origin = clawt_task_get_origin(dependent);
	const gchar *how = clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
	                                      clawt_task_get_state(blocker));
	g_autofree gchar *body = NULL;
	g_autofree gchar *title = clawt_preamble_escape(
		clawt_task_get_title(dependent));
	g_autofree gchar *blocker_title = clawt_preamble_escape(
		clawt_task_get_title(blocker));

	body = g_strdup_printf(
		"[clawtilla] Task %s (%s) is waiting for task %s (%s), which %s. "
		"It will not be handed over until that one completes. Retry it "
		"(clawtilla_task_retry), cancel the waiting task "
		"(clawtilla_task_cancel), or stop waiting for it "
		"(clawtilla_task_link with remove: true), which hands the work "
		"over now if nothing else holds it.",
		clawt_task_get_id(dependent), title, clawt_task_get_id(blocker),
		blocker_title,
		g_strcmp0(how, "cancelled") == 0 ? "was cancelled" :
		g_strcmp0(how, "stalled") == 0 ? "stalled" : "failed");

	if (origin == NULL || g_strcmp0(origin, "user") == 0 ||
	    clawt_agent_manager_get(self->agents, origin) == NULL) {
		clawt_task_manager_add_note(self->tasks, clawt_task_get_id(dependent),
		                            NULL, "blocker", body, NULL);
		publish_note(self, dependent, NULL);
		return;
	}

	{
		ClawtRoom *room = NULL;
		g_autoptr(ClawtMessage) message = NULL;
		g_autoptr(GError) error = NULL;

		if (clawt_task_get_room(dependent) != NULL)
			room = clawt_room_manager_get(self->rooms,
			                              clawt_task_get_room(dependent));
		if (room == NULL && clawt_task_get_assignee(dependent) != NULL)
			room = clawt_room_manager_get_direct(
				self->rooms, origin, clawt_task_get_assignee(dependent));
		if (room == NULL || self->router == NULL)
			return;

		message = clawt_message_new(clawt_room_get_id(room),
		                            CLAWT_SYSTEM_SENDER, body);
		clawt_message_set_task_id(message, clawt_task_get_id(dependent));
		clawt_message_set_depth(message, 0);
		clawt_message_set_invites_reply(message, FALSE);
		clawt_message_set_only_for(message, origin);

		if (clawt_mailbox_router_send(self->router, message, &error) < 0)
			g_warning("task %s: could not say that %s ended: %s",
			          clawt_task_get_id(dependent),
			          clawt_task_get_id(blocker),
			          error != NULL ? error->message : "unknown");
	}
}

void
clawt_daemon_tasks_blocker_settled(
	ClawtDaemon *self,
	ClawtTask   *task
){
	g_autoptr(GPtrArray) dependents = NULL;
	gboolean completed;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (task == NULL)
		return;

	/*
	 * A task that ended while still held -- cancelled, say -- drops the
	 * delivery it was waiting to send: nothing will ever release it.
	 */
	{
		g_autoptr(JsonNode) dropped = clawt_task_manager_take_delivery(
			self->tasks, clawt_task_get_id(task));

		(void)dropped;
	}

	dependents = clawt_task_manager_dependents(self->tasks,
	                                           clawt_task_get_id(task));
	completed = clawt_task_get_state(task) == CLAWT_TASK_COMPLETED;

	for (i = 0; i < dependents->len; i++) {
		const gchar *id = g_ptr_array_index(dependents, i);
		ClawtTask *dependent = clawt_task_manager_get(self->tasks, id);

		if (dependent == NULL ||
		    clawt_task_get_state(dependent) != CLAWT_TASK_BLOCKED)
			continue;

		if (completed)
			clawt_daemon_tasks_release_ready(self, id);
		else
			tell_blocker_ended(self, dependent, task);
	}
}

void
clawt_daemon_tasks_on_blockers_changed(
	ClawtTaskManager *manager,
	const gchar      *task_id,
	gpointer          user_data
){
	(void)manager;

	/* A blocker taken away may be the last: release if nothing holds it. */
	clawt_daemon_tasks_release_ready(user_data, task_id);
}

/* ── Review ──────────────────────────────────────────────────────── */

#define REVIEW_DECISION_PREFIX "review-"

/*
 * A system notice to @to about @task, in the conversation the work is
 * in: the task's own room, the one it was delegated from, or the direct
 * room between @to and @other.  Daemon-composed, depth 0, invites no
 * reply, for @to alone.
 */
static void
notice_about_task(
	ClawtDaemon *self,
	ClawtTask   *task,
	const gchar *to,
	const gchar *other,
	const gchar *body
){
	g_autoptr(ClawtMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	ClawtRoom *room = NULL;

	if (self->router == NULL || to == NULL)
		return;

	if (clawt_task_get_thread_room(task) != NULL)
		room = clawt_room_manager_get(self->rooms,
		                              clawt_task_get_thread_room(task));
	if (room == NULL && clawt_task_get_room(task) != NULL)
		room = clawt_room_manager_get(self->rooms, clawt_task_get_room(task));
	if (room != NULL && !clawt_room_has_member(room, to))
		room = NULL;
	if (room == NULL && other != NULL)
		room = clawt_room_manager_get_direct(self->rooms, to, other);
	if (room == NULL)
		return;

	message = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER,
	                            body);
	clawt_message_set_task_id(message, clawt_task_get_id(task));
	clawt_message_set_depth(message, 0);
	clawt_message_set_invites_reply(message, FALSE);
	clawt_message_set_only_for(message, to);

	if (clawt_mailbox_router_send(self->router, message, &error) < 0)
		g_warning("task %s: could not tell %s: %s", clawt_task_get_id(task),
		          to, error != NULL ? error->message : "unknown");
}

static gchar *
review_decision_id(const gchar *task_id)
{
	return g_strconcat(REVIEW_DECISION_PREFIX, task_id, NULL);
}

/*
 * A result is waiting for its reviewer.  An agent reviewer is told, with
 * the result and how to answer; the operator gets a decision in the
 * inbox (answer `accept`, or write what to change), and the task's page
 * has the same two buttons.  The tree's budget renews, as on a settle:
 * a review round is owed work, not a runaway.
 */
void
clawt_daemon_tasks_review_requested(
	ClawtDaemon *self,
	ClawtTask   *task
){
	const gchar *reviewer;
	g_autofree gchar *result = NULL;
	g_autofree gchar *title = NULL;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	reviewer = clawt_task_get_reviewer(task);

	if (reviewer == NULL)
		return;

	clawt_daemon_tree_settled(self, task);

	result = clawt_preamble_escape(clawt_task_get_result(task));
	title = clawt_preamble_escape(clawt_task_get_title(task));

	if (g_strcmp0(reviewer, "user") == 0) {
		g_autofree gchar *id = review_decision_id(clawt_task_get_id(task));
		g_autofree gchar *question = NULL;
		g_autoptr(ClawtDecision) decision = NULL;
		g_autofree gchar *stored = NULL;
		static const gchar *const options[] = { "accept", NULL };

		if (self->decisions == NULL)
			return;

		/* One open decision per task; a second round replaces the first. */
		clawt_decision_store_delete(self->decisions, id, NULL);

		question = g_strdup_printf(
			"Review task %s (%s), finished by %s%s. Answer accept, or write "
			"what to change and it goes back to them.\n\nThe result:\n%s",
			clawt_task_get_id(task), clawt_task_get_title(task),
			clawt_task_get_assignee(task) != NULL
				? clawt_task_get_assignee(task) : "its assignee",
			clawt_task_get_review_round(task) > 0 ? " again" : "",
			clawt_task_get_result(task) != NULL
				? clawt_task_get_result(task) : "(nothing written)");
		decision = clawt_decision_new(id, clawt_task_get_assignee(task),
		                              question);
		clawt_decision_set_options(decision, options);
		clawt_decision_set_task(decision, clawt_task_get_id(task));
		clawt_decision_set_default(decision, "accept",
			"nothing happens until somebody answers; the task waits in "
			"review");
		stored = clawt_decision_store_post(self->decisions, decision, NULL);

		if (stored != NULL)
			clawt_daemon_publish_decision_asked(self, decision);

		return;
	}

	{
		g_autofree gchar *body = g_strdup_printf(
			"[clawtilla] Task %s (%s) is finished by %s and waiting for your "
			"review%s.\n\nThe result:\n%s\n\nGive your verdict with "
			"clawtilla_task_review: accept: true if it is right, or accept: "
			"false with notes saying exactly what to change -- it goes back "
			"to them with your notes.",
			clawt_task_get_id(task), title,
			clawt_task_get_assignee(task) != NULL
				? clawt_task_get_assignee(task) : "its assignee",
			clawt_task_get_review_round(task) > 0 ? " again" : "",
			result != NULL ? result : "(nothing written)");

		notice_about_task(self, task, reviewer,
		                  clawt_task_get_assignee(task), body);
	}
}

void
clawt_daemon_tasks_on_review_requested(
	ClawtTaskManager *manager,
	const gchar      *task_id,
	gpointer          user_data
){
	ClawtDaemon *self = user_data;
	ClawtTask *task = clawt_task_manager_get(manager, task_id);

	if (task != NULL)
		clawt_daemon_tasks_review_requested(self, task);
}

/*
 * A verdict, from whichever door: the reviewer's tool, a client, the
 * decision inbox.  Changes go back to the assignee with the notes; an
 * accept completes the task, and the usual settle notice tells its
 * delegator -- unless the delegator was the reviewer, who knows.
 */
gchar *
clawt_daemon_tasks_review(
	ClawtDaemon  *self,
	const gchar  *who,
	const gchar  *task_id,
	gboolean      accept,
	const gchar  *notes,
	GError      **error
){
	ClawtTaskReviewOutcome outcome;
	ClawtTask *task;
	gint64 rounds;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

	rounds = clawt_config_get_int(self->config, "orchestration.review_rounds");
	outcome = clawt_task_manager_review(self->tasks, task_id, who, accept,
	                                    notes, (guint)MAX(rounds, 0), error);

	if (outcome == CLAWT_TASK_REVIEW_REFUSED)
		return NULL;

	task = clawt_task_manager_get(self->tasks, task_id);

	/* The other door closes: a verdict given here settles the inbox's. */
	if (self->decisions != NULL && outcome != CLAWT_TASK_REVIEW_ESCALATED) {
		g_autofree gchar *id = review_decision_id(task_id);

		clawt_decision_store_dismiss(self->decisions, id, NULL);
	}

	clawt_daemon_tree_settled(self, task);

	switch (outcome) {
	case CLAWT_TASK_REVIEW_ACCEPTED:
		return g_strdup_printf("Accepted. Task %s is done.", task_id);

	case CLAWT_TASK_REVIEW_CHANGES: {
		g_autofree gchar *said = clawt_preamble_escape(notes);
		g_autofree gchar *body = g_strdup_printf(
			"[clawtilla] %s asked for changes to task %s (%s), round %u:"
			"\n\n%s\n\nMake them, then finish the task again "
			"(clawtilla_task_complete) -- it goes back for review.",
			g_strcmp0(who, "user") == 0 ? "The operator" : who, task_id,
			clawt_task_get_title(task), clawt_task_get_review_round(task),
			said);

		notice_about_task(self, task, clawt_task_get_assignee(task),
		                  g_strcmp0(who, "user") == 0
		                      ? clawt_task_get_origin(task) : who, body);
		return g_strdup_printf("Sent back to %s with your notes; you will "
		                       "be asked again when it is finished.",
		                       clawt_task_get_assignee(task));
	}

	case CLAWT_TASK_REVIEW_ESCALATED:
		return g_strdup_printf(
			"Changes have been asked for %u times, so task %s is now the "
			"operator's to decide. Nothing more is needed from you.",
			clawt_task_get_review_round(task), task_id);

	case CLAWT_TASK_REVIEW_REFUSED:
		break;
	}

	return NULL;
}

/* The decision inbox's door: `accept`, or anything else as the notes. */
gboolean
clawt_daemon_review_settle(
	ClawtDaemon   *self,
	ClawtDecision *decision,
	const gchar   *how
){
	const gchar *id;
	const gchar *answer;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *said = NULL;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

	if (decision == NULL ||
	    !g_str_has_prefix(clawt_decision_get_id(decision),
	                      REVIEW_DECISION_PREFIX))
		return FALSE;

	if (g_strcmp0(how, "answered") != 0)
		return TRUE;

	id = clawt_decision_get_id(decision) + strlen(REVIEW_DECISION_PREFIX);
	answer = clawt_decision_get_answer(decision);

	said = clawt_daemon_tasks_review(
		self, "user", id,
		g_ascii_strcasecmp(answer != NULL ? answer : "", "accept") == 0,
		answer, &error);

	if (said == NULL)
		g_warning("review %s: %s", id, error->message);

	return TRUE;
}

/* clawtilla_task_review's hook. */
gchar *
clawt_daemon_review_for_tools(
	const gchar  *reviewer,
	const gchar  *task_id,
	gboolean      accept,
	const gchar  *notes,
	gpointer      user_data,
	GError      **error
){
	return clawt_daemon_tasks_review(user_data, reviewer, task_id, accept,
	                                 notes, error);
}
