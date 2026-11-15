/*
 * daemon-ack.c - An acknowledgement does not wake its sender
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A deliberate message earns one answer, and the answer is delivered to
 * whoever asked as a turn -- which costs that agent whatever its context
 * costs to reread, for "Thanks, noted." as much as for a page of
 * findings.  With `orchestration.ack_check_model` set, an answer that
 * qualifies (clawt_ack_check_gate()) is put to a small model first.  A
 * plain ACK is recorded in the room, marked, and folded into the
 * asker's next turn as one line; anything else is routed as it always
 * was.
 *
 * The question leaves the thread: the model is asked asynchronously,
 * with a hard limit, so neither the link handler nor anything else on
 * the daemon's loop waits on it.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* One answer waiting on the model. */
typedef struct {
	ClawtDaemon  *daemon;      /* owned */
	ClawtMessage *message;     /* owned: the answer, ready to route */
	gchar        *replier;
	gchar        *asker;
	gchar        *room_id;
} AckJob;

static void
ack_job_free(
	AckJob *job
){
	g_clear_object(&job->daemon);
	g_clear_pointer(&job->message, clawt_message_free);
	g_free(job->replier);
	g_free(job->asker);
	g_free(job->room_id);
	g_free(job);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AckJob, ack_job_free)

void
clawt_daemon_ack_set_provider(
	ClawtDaemon *self,
	AiProvider  *provider
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	g_clear_object(&self->ack_provider_override);

	if (provider != NULL)
		self->ack_provider_override = g_object_ref(provider);
}

/*
 * The model to ask, or NULL when the check is off or the key names
 * something ai-glib cannot build.  Built under the daemon's own context:
 * a provider's HTTP session binds to whichever context is thread-default
 * when it is made, and one made under another loop would send its
 * request and never read the answer.
 */
static AiProvider *
ack_provider(
	ClawtDaemon *self
){
	const gchar *spec;
	g_autofree gchar *provider_name = NULL;
	g_autofree gchar *model = NULL;
	g_autoptr(AiConfig) ai_config = NULL;
	g_autoptr(GError) error = NULL;
	GObject *provider;

	if (self->ack_provider_override != NULL)
		return self->ack_provider_override;

	spec = clawt_config_get_string(self->config,
	                               "orchestration.ack_check_model");

	if (spec == NULL || *spec == '\0')
		return NULL;

	if (self->ack_provider != NULL &&
		g_strcmp0(self->ack_provider_spec, spec) == 0)
		return self->ack_provider;

	g_clear_object(&self->ack_provider);
	g_free(self->ack_provider_spec);
	self->ack_provider_spec = g_strdup(spec);

	if (!clawt_ack_check_split_model(spec, &provider_name, &model)) {
		g_warning("orchestration.ack_check_model '%s' is not provider/model; "
		          "the acknowledgement check is off", spec);
		return NULL;
	}

	ai_config = ai_config_new();
	ai_config_set_default_model(ai_config, model);

	g_main_context_push_thread_default(self->main_context);
	provider = ai_provider_factory_new_from_string(provider_name, ai_config,
	                                               &error);
	g_main_context_pop_thread_default(self->main_context);

	if (provider == NULL) {
		g_warning("orchestration.ack_check_model: provider '%s': %s; the "
		          "acknowledgement check is off", provider_name,
		          error != NULL ? error->message : "unknown");
		return NULL;
	}

	self->ack_provider = AI_PROVIDER(provider);

	return self->ack_provider;
}

/* Routes the answer the way it would have gone without the check. */
static void
ack_route(
	AckJob *job
){
	g_autoptr(GError) error = NULL;

	if (job->daemon->router == NULL)
		return;

	/*
	 * By the road every other answer takes: an answer that needed its
	 * asker goes back to the conversation that asked, and the check
	 * used to send it into the two agents' room instead.
	 */
	if (!clawt_daemon_route_answer(job->daemon, job->message, &error))
		g_info("daemon: %s's message was not routed: %s", job->replier,
		       error != NULL ? error->message : "no reason");
}

static void
on_ack_checked(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	g_autoptr(AckJob) job = data;
	g_autoptr(GError) error = NULL;
	ClawtAckVerdict verdict;

	(void)source;

	verdict = clawt_ack_check_run_finish(result, &error);

	/*
	 * Three outcomes, logged as three.  Only "it said ACK" skips the
	 * wake; "it said NEEDS" and "it did not say" both route the answer,
	 * and a log that called a timeout a NEEDS would send whoever reads
	 * it to look at the model rather than the network.
	 */
	switch (verdict) {
	case CLAWT_ACK_VERDICT_ACK:
		break;

	case CLAWT_ACK_VERDICT_NEEDS:
		g_info("daemon: %s's answer to %s needs reading; delivered",
		       job->replier, job->asker);
		ack_route(job);
		return;

	case CLAWT_ACK_VERDICT_UNSAID:
		g_info("daemon: the acknowledgement check did not say (%s); %s's "
		       "answer to %s is delivered", error != NULL ? error->message
		       : "no verdict", job->replier, job->asker);
		ack_route(job);
		return;
	}

	if (job->daemon->router == NULL)
		return;

	/*
	 * Recorded, marked and noted -- never dropped.  The room keeps the
	 * answer for anybody reading it, every client draws it as
	 * acknowledged, and the asker's next turn opens with one line saying
	 * it came.
	 */
	clawt_message_set_acknowledged(job->message, TRUE);

	{
		g_autoptr(GError) record_error = NULL;
		g_autofree gchar *note = NULL;

		if (!clawt_mailbox_router_record(job->daemon->router, job->message,
		                                 &record_error)) {
			/*
			 * An answer that cannot be recorded must still reach
			 * somebody, so it goes the ordinary way.
			 */
			g_warning("daemon: %s's acknowledgement could not be recorded "
			          "(%s); delivering it instead", job->replier,
			          record_error->message);
			clawt_message_set_acknowledged(job->message, FALSE);
			ack_route(job);
			return;
		}

		note = clawt_ack_check_note(job->replier, job->room_id,
		                            clawt_message_get_body(job->message));
		clawt_mailbox_router_add_ack_note(job->daemon->router, job->asker,
		                                  note);
	}

	g_info("daemon: %s only acknowledged %s; recorded without a wake",
	       job->replier, job->asker);
}

gboolean
clawt_daemon_ack_consider(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	const gchar  *room_id,
	const gchar  *destination,
	ClawtMessage *message
){
	ClawtAgent *replier;
	ClawtAgent *asker;
	ClawtAgentConfig *config;
	ClawtMailboxItem *input;
	ClawtRoom *room;
	AiProvider *provider;
	const gchar *reason = NULL;
	gboolean evidence;
	guint tool_calls;
	AckJob *job;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

	if (message == NULL || destination == NULL || self->router == NULL)
		return FALSE;

	/* Off unless a model is configured, which is the default. */
	if (self->ack_provider_override == NULL) {
		const gchar *spec = clawt_config_get_string(self->config,
			"orchestration.ack_check_model");

		if (spec == NULL || *spec == '\0')
			return FALSE;
	}

	/* A task's thread settles by its own rules; never second-guessed. */
	if (clawt_message_get_task_id(message) != NULL)
		return FALSE;

	/*
	 * A room that posts every closing text costs nobody a turn unless
	 * the answer names somebody, and naming somebody is deliberate.
	 */
	room = self->rooms != NULL
		? clawt_room_manager_get(self->rooms, destination) : NULL;
	if (room == NULL || clawt_room_get_require_mention(room))
		return FALSE;

	replier = clawt_agent_manager_get(self->agents, agent_id);
	if (replier == NULL)
		return FALSE;

	/*
	 * The message being answered: a deliberate one, from a peer whose
	 * turn the answer would start, in the room the answer goes to.
	 * Anything else -- an operator's question, a room the answer is
	 * leaving -- is not an acknowledgement of a peer's message.
	 */
	input = clawt_agent_get_turn_input_in(replier, room_id);
	if (input == NULL || !clawt_mailbox_item_get_invites_reply(input) ||
		g_strcmp0(clawt_mailbox_item_get_room(input), destination) != 0)
		return FALSE;

	asker = clawt_agent_manager_get(self->agents,
	                                clawt_mailbox_item_get_from(input));
	if (asker == NULL || asker == replier)
		return FALSE;

	config = clawt_agent_get_config(replier);
	evidence = config != NULL &&
		clawt_agent_config_get_boolean(config, "runtime.stream_steps");
	tool_calls = clawt_agent_get_turn_tool_calls_in(replier, room_id);

	if (!clawt_ack_check_gate(clawt_message_get_body(message), evidence,
	                          tool_calls,
	                          clawt_mailbox_item_get_task_id(input) != NULL,
	                          &reason)) {
		g_debug("daemon: %s's answer to %s is not checked: %s", agent_id,
		        clawt_agent_get_id(asker), reason);
		return FALSE;
	}

	provider = ack_provider(self);
	if (provider == NULL)
		return FALSE;

	job = g_new0(AckJob, 1);
	job->daemon = g_object_ref(self);
	job->message = clawt_message_copy(message);
	job->replier = g_strdup(agent_id);
	job->asker = g_strdup(clawt_agent_get_id(asker));
	job->room_id = g_strdup(destination);

	/*
	 * Under the daemon's context, so the task and the timer attach to
	 * the loop that is actually running -- an embedded daemon's is not
	 * the process default.
	 */
	g_main_context_push_thread_default(self->main_context);
	clawt_ack_check_run_async(provider, clawt_mailbox_item_get_body(input),
	                          clawt_message_get_body(message), 0, NULL,
	                          on_ack_checked, job);
	g_main_context_pop_thread_default(self->main_context);

	return TRUE;
}

/* ── orchestration.* ─────────────────────────────────────────────── */

/* Highest tree budget the page accepts; see orchestration.set below. */
#define TREE_BUDGET_MAX (10000)

/*
 * The current settings and every choice, so a client offers the list:
 * the acknowledgement check, the tree budget, and each group room's
 * thread cap.  The rooms are here rather than left to room.list so the
 * Orchestration page is one request, and so "which rooms have rounds"
 * is answered by the daemon -- a group, and declared, since only a
 * declared room has an entry room.set can write to.
 */
static JsonNode *
orchestration_reply(
	ClawtDaemon *self,
	JsonNode    *request
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	const gchar *value = clawt_config_get_string(self->config,
		"orchestration.ack_check_model");
	g_autofree gchar *label = clawt_ack_check_model_label(value);
	g_autoptr(GPtrArray) rooms = NULL;
	guint i;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "ack_check_model");
	json_builder_add_string_value(builder, value != NULL ? value : "");
	json_builder_set_member_name(builder, "ack_check_model_label");
	json_builder_add_string_value(builder, label);
	json_builder_set_member_name(builder, "tree_message_budget");
	json_builder_add_int_value(builder, clawt_config_get_int(self->config,
		"orchestration.tree_message_budget"));
	json_builder_set_member_name(builder, "max_hops");
	json_builder_add_int_value(builder, clawt_config_get_int(self->config,
		"orchestration.max_hops"));

	json_builder_set_member_name(builder, "rooms");
	json_builder_begin_array(builder);

	rooms = (self->rooms != NULL) ? clawt_room_manager_list(self->rooms)
	                              : g_ptr_array_new();

	for (i = 0; i < rooms->len; i++) {
		ClawtRoom *room = g_ptr_array_index(rooms, i);

		if (!clawt_room_is_group(room) ||
			!clawt_room_is_declared(clawt_room_get_id(room)))
			continue;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, clawt_room_get_id(room));
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, clawt_room_get_name(room));
		json_builder_set_member_name(builder, "max_chain");
		json_builder_add_int_value(builder, clawt_room_get_max_chain(room));
		json_builder_set_member_name(builder, "max_hops");
		json_builder_add_int_value(builder, clawt_room_get_max_hops(room));
		json_builder_set_member_name(builder, "round");
		json_builder_add_int_value(builder, clawt_room_get_round(room));
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_orchestration(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	*handled = TRUE;

	if (g_strcmp0(kind, "orchestration.get") == 0)
		return orchestration_reply(self, request);

	if (g_strcmp0(kind, "orchestration.set") == 0) {
		const gchar *value = clawt_ipc_payload_string(payload,
			"ack_check_model");
		gboolean has_budget = payload != NULL &&
			json_object_has_member(payload, "tree_message_budget");
		gint64 budget = clawt_ipc_payload_int(payload,
			"tree_message_budget", -1);
		g_autoptr(GError) error = NULL;

		/*
		 * Each setting on its own: a page that saves one must not have
		 * to resend the other, and a client from before the budget
		 * existed still sends only the model.
		 */
		if (value == NULL && !has_budget)
			return clawt_ipc_error_new(request,
				CLAWT_ERROR_INVALID_ARGUMENT,
				"nothing to set: send ack_check_model (\"\" turns the "
				"check off) or tree_message_budget");

		/*
		 * Refused when it cannot be split, so a client cannot save a
		 * value the daemon would then warn about and ignore.  A model
		 * the catalogue does not list is accepted: the catalogue goes
		 * stale, and a model named by hand is a legitimate choice.
		 */
		if (value != NULL && *value != '\0' &&
			!clawt_ack_check_split_model(value, NULL, NULL))
			return clawt_ipc_error_new(request,
				CLAWT_ERROR_INVALID_ARGUMENT,
				"ack_check_model is provider/model, or empty for off");

		/*
		 * Bounded both ways.  Negative has no meaning, and a number so
		 * large it can never be reached is "off" spelled in a way that
		 * looks like a limit.
		 */
		if (has_budget && (budget < 0 || budget > TREE_BUDGET_MAX))
			return clawt_ipc_error_new(request,
				CLAWT_ERROR_INVALID_ARGUMENT,
				"tree_message_budget is how many requests a task tree "
				"may send between settlements: 0 (no budget) to 10000");

		if (value != NULL &&
			!clawt_config_set_string(self->config,
			                         "orchestration.ack_check_model", value))
			return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
				"the acknowledgement check could not be set");

		if (has_budget &&
			!clawt_config_set_int(self->config,
			                      "orchestration.tree_message_budget",
			                      budget))
			return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
				"the tree budget could not be set");

		if (!clawt_config_save(self->config, &error))
			return clawt_ipc_error_new(request,
				error != NULL ? error->code : CLAWT_ERROR_FAILED,
				error != NULL ? error->message
				              : "the setting could not be saved");

		/* Into the guard now, not at the next reload. */
		if (has_budget && self->guard != NULL)
			clawt_loop_guard_set_tree_budget(self->guard, (guint)budget);

		clawt_event_bus_emit(self->bus, "config.changed", NULL);

		return orchestration_reply(self, request);
	}

	*handled = FALSE;
	return NULL;
}
