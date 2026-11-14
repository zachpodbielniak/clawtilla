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

	if (clawt_mailbox_router_send(job->daemon->router, job->message,
	                              &error) < 0)
		g_info("daemon: %s's message was not routed: %s", job->replier,
		       error->message);
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

/* The current setting and every choice, so a client offers the list. */
static JsonNode *
orchestration_reply(
	ClawtDaemon *self,
	JsonNode    *request
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	const gchar *value = clawt_config_get_string(self->config,
		"orchestration.ack_check_model");
	g_autofree gchar *label = clawt_ack_check_model_label(value);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "ack_check_model");
	json_builder_add_string_value(builder, value != NULL ? value : "");
	json_builder_set_member_name(builder, "ack_check_model_label");
	json_builder_add_string_value(builder, label);
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
		g_autoptr(GError) error = NULL;

		if (value == NULL)
			return clawt_ipc_error_new(request,
				CLAWT_ERROR_INVALID_ARGUMENT,
				"ack_check_model is required (\"\" turns the check off)");

		/*
		 * Refused when it cannot be split, so a client cannot save a
		 * value the daemon would then warn about and ignore.  A model
		 * the catalogue does not list is accepted: the catalogue goes
		 * stale, and a model named by hand is a legitimate choice.
		 */
		if (*value != '\0' &&
			!clawt_ack_check_split_model(value, NULL, NULL))
			return clawt_ipc_error_new(request,
				CLAWT_ERROR_INVALID_ARGUMENT,
				"ack_check_model is provider/model, or empty for off");

		if (!clawt_config_set_string(self->config,
		                             "orchestration.ack_check_model",
		                             value) ||
			!clawt_config_save(self->config, &error))
			return clawt_ipc_error_new(request,
				error != NULL ? error->code : CLAWT_ERROR_FAILED,
				error != NULL ? error->message
				              : "the setting could not be saved");

		clawt_event_bus_emit(self->bus, "config.changed", NULL);

		return orchestration_reply(self, request);
	}

	*handled = FALSE;
	return NULL;
}
