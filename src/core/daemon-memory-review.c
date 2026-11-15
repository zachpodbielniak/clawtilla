/*
 * daemon-memory-review.c - The periodic memory review, and its proposals
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Off unless `memories.review.model` names a model.  On, a timer looks
 * at the fleet every few minutes and starts at most one pass at a time:
 * an agent whose last pass is older than `memories.review.interval_hours`
 * and who has no proposals left unanswered.  The model is asked
 * asynchronously with a hard limit (clawt_memory_review_run_async()), so
 * nothing on the daemon's loop waits for it.  What it proposes is written
 * to `<state_dir>/memory-review/<agent>.json` and announced with
 * `memory.changed`; nothing reaches the memory store until a person
 * accepts a proposal (`memory.proposal_accept`), and then only through
 * the store's journaled functions.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* How often the fleet is looked at for an agent that is due. */
#define REVIEW_TICK_SECONDS (600)

/* The bounds the interval and the batch are clamped to. */
#define INTERVAL_HOURS_MIN (1)
#define INTERVAL_HOURS_MAX (720)
#define MAX_MEMORIES_MIN   (5)
#define MAX_MEMORIES_MAX   (500)

/* A merge or a forget needs something to compare; one memory is nothing. */
#define REVIEW_MIN_MEMORIES (2)

static gchar *
review_dir(
	ClawtDaemon *self
){
	return g_build_filename(self->state_dir, "memory-review", NULL);
}

static void
publish_changed(
	ClawtDaemon *self,
	const gchar *agent_id
){
	g_autoptr(ClawtEvent) event = NULL;

	if (self->bus == NULL)
		return;

	event = clawt_event_new("memory.changed", agent_id);
	clawt_event_bus_publish(self->bus, event);
}

void
clawt_daemon_memory_review_set_provider(
	ClawtDaemon *self,
	AiProvider  *provider
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	g_clear_object(&self->review_provider_override);

	if (provider != NULL)
		self->review_provider_override = g_object_ref(provider);
}

/* The configured model, or "" when the review is off. */
static const gchar *
review_spec(
	ClawtDaemon *self
){
	const gchar *spec = clawt_config_get_string(self->config,
		"memories.review.model");

	return (spec != NULL) ? spec : "";
}

static gboolean
review_enabled(
	ClawtDaemon *self
){
	return self->review_provider_override != NULL || *review_spec(self) != '\0';
}

/*
 * The model to ask, or NULL when the review is off or the key names
 * something ai-glib cannot build.  Built under the daemon's context, for
 * the same reason as the acknowledgement check's: a provider's HTTP
 * session binds to whichever context is thread-default when it is made.
 */
static AiProvider *
review_provider(
	ClawtDaemon  *self,
	GError      **error
){
	const gchar *spec = review_spec(self);
	g_autofree gchar *provider_name = NULL;
	g_autofree gchar *model = NULL;
	g_autoptr(AiConfig) ai_config = NULL;
	GObject *provider;

	if (self->review_provider_override != NULL)
		return self->review_provider_override;

	if (*spec == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
			"the memory review is off; set memories.review.model");
		return NULL;
	}

	if (self->review_provider != NULL &&
		g_strcmp0(self->review_provider_spec, spec) == 0)
		return self->review_provider;

	g_clear_object(&self->review_provider);
	g_free(self->review_provider_spec);
	self->review_provider_spec = g_strdup(spec);

	if (!clawt_ack_check_split_model(spec, &provider_name, &model)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			"memories.review.model '%s' is not provider/model", spec);
		return NULL;
	}

	ai_config = ai_config_new();
	ai_config_set_default_model(ai_config, model);

	g_main_context_push_thread_default(self->main_context);
	provider = ai_provider_factory_new_from_string(provider_name, ai_config,
		error);
	g_main_context_pop_thread_default(self->main_context);

	if (provider == NULL) {
		g_prefix_error(error, "memories.review.model: provider '%s': ",
			provider_name);
		return NULL;
	}

	self->review_provider = AI_PROVIDER(provider);

	return self->review_provider;
}

static void
note_error(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *message
){
	if (self->review_errors == NULL)
		self->review_errors = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, g_free);

	if (message == NULL)
		g_hash_table_remove(self->review_errors, agent_id);
	else
		g_hash_table_replace(self->review_errors, g_strdup(agent_id),
			g_strdup(message));
}

/* ── One pass ────────────────────────────────────────────────────── */

typedef struct {
	ClawtDaemon *daemon;    /* owned */
	gchar       *agent_id;
} ReviewJob;

static void
review_job_free(
	ReviewJob *job
){
	g_clear_object(&job->daemon);
	g_free(job->agent_id);
	g_free(job);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ReviewJob, review_job_free)

static void
on_review_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	g_autoptr(ReviewJob) job = data;
	ClawtDaemon *self = job->daemon;
	g_autoptr(GPtrArray) proposals = NULL;
	g_autoptr(GPtrArray) open = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = review_dir(self);
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	guint i;

	(void)source;

	g_clear_pointer(&self->review_running, g_free);

	proposals = clawt_memory_review_run_finish(result, &error);

	/*
	 * A pass that failed still counts as a pass, so a model that is down
	 * or answers rubbish is asked again an interval later rather than on
	 * every tick -- a failing pass is spend too.  Its error is kept for
	 * the clients to show beside the setting.
	 */
	open = clawt_memory_proposals_load(dir, job->agent_id, NULL, NULL);
	if (open == NULL)
		open = g_ptr_array_new_with_free_func(
			(GDestroyNotify)clawt_memory_proposal_free);

	if (proposals == NULL) {
		g_message("%s: the memory review proposed nothing: %s",
			job->agent_id, error != NULL ? error->message : "no reason");
		note_error(self, job->agent_id,
			error != NULL ? error->message : "the review failed");
	} else {
		note_error(self, job->agent_id, NULL);

		for (i = 0; i < proposals->len; i++)
			g_ptr_array_add(open, clawt_memory_proposal_copy(
				g_ptr_array_index(proposals, i)));

		g_info("%s: the memory review proposed %u change(s)",
			job->agent_id, proposals->len);
	}

	{
		g_autoptr(GError) save_error = NULL;

		if (!clawt_memory_proposals_save(dir, job->agent_id, open, now,
				&save_error))
			g_warning("%s: memory review proposals were not kept: %s",
				job->agent_id, save_error->message);
	}

	publish_changed(self, job->agent_id);
}

gboolean
clawt_daemon_memory_review_start(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	GError      **error
){
	ClawtMemoryStore *store;
	AiProvider *provider;
	g_autoptr(GPtrArray) memories = NULL;
	ReviewJob *job;
	gint64 batch;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

	if (agent_id == NULL || self->agents == NULL ||
		clawt_agent_manager_get(self->agents, agent_id) == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			"no such agent");
		return FALSE;
	}

	/* One pass at a time, fleet-wide: it is spend nobody is watching. */
	if (self->review_running != NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
			"a memory review is already running (for %s)",
			self->review_running);
		return FALSE;
	}

	/* The agent's own database only: shared memories are the team's. */
	store = clawt_agent_manager_memory_store_for_scope(self->agents, agent_id,
		CLAWT_MEMORY_SCOPE_AGENT);

	if (store == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
			"that agent has no memory store; memories.enabled is off");
		return FALSE;
	}

	provider = review_provider(self, error);
	if (provider == NULL)
		return FALSE;

	batch = CLAMP(clawt_config_get_int(self->config,
		"memories.review.max_memories"), MAX_MEMORIES_MIN, MAX_MEMORIES_MAX);
	memories = clawt_memory_store_list(store, NULL, FALSE, (guint)batch,
		error);

	if (memories == NULL)
		return FALSE;

	if (memories->len < REVIEW_MIN_MEMORIES) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			"that agent has fewer than two memories; there is nothing to "
			"review");
		return FALSE;
	}

	job = g_new0(ReviewJob, 1);
	job->daemon = g_object_ref(self);
	job->agent_id = g_strdup(agent_id);
	self->review_running = g_strdup(agent_id);

	/* Under the daemon's context, where the task and its timer attach. */
	g_main_context_push_thread_default(self->main_context);
	clawt_memory_review_run_async(provider, memories,
		self->review_timeout_ms, NULL, on_review_done, job);
	g_main_context_pop_thread_default(self->main_context);

	return TRUE;
}

/*
 * One agent that is due, or NULL.  Due means: its own memory store
 * exists, its last pass is older than the interval, and nothing it was
 * last proposed is still waiting -- an unread list never grows by
 * itself.
 */
static const gchar *
next_due(
	ClawtDaemon *self
){
	GPtrArray *agents;   /* transfer none: the manager's own list */
	g_autofree gchar *dir = review_dir(self);
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 hours = CLAMP(clawt_config_get_int(self->config,
		"memories.review.interval_hours"), INTERVAL_HOURS_MIN,
		INTERVAL_HOURS_MAX);
	guint i;

	agents = clawt_agent_manager_list(self->agents);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtAgent *agent = g_ptr_array_index(agents, i);
		const gchar *id = clawt_agent_get_id(agent);
		g_autoptr(GPtrArray) open = NULL;
		gint64 last = 0;

		if (clawt_agent_manager_memory_store_for_scope(self->agents, id,
				CLAWT_MEMORY_SCOPE_AGENT) == NULL)
			continue;

		open = clawt_memory_proposals_load(dir, id, &last, NULL);

		if (open == NULL || open->len > 0)
			continue;

		if (last > 0 && now - last < hours * 3600)
			continue;

		return id;
	}

	return NULL;
}

void
clawt_daemon_memory_review_tick(
	ClawtDaemon *self
){
	const gchar *agent_id;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (!review_enabled(self) || self->review_running != NULL ||
		self->agents == NULL)
		return;

	agent_id = next_due(self);

	if (agent_id != NULL) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *id = g_strdup(agent_id);

		if (!clawt_daemon_memory_review_start(self, id, &error)) {
			/*
			 * Stamped anyway, so an agent that cannot be reviewed --
			 * one memory, a model that cannot be built -- is not
			 * retried on every tick ahead of every other agent.
			 */
			g_autofree gchar *dir = review_dir(self);
			g_autoptr(GPtrArray) none = g_ptr_array_new();

			g_debug("%s: no memory review: %s", id, error->message);
			note_error(self, id, error->message);
			clawt_memory_proposals_save(dir, id, none,
				g_get_real_time() / G_USEC_PER_SEC, NULL);
		}
	}
}

static gboolean
on_review_tick(
	gpointer data
){
	clawt_daemon_memory_review_tick(CLAWT_DAEMON(data));

	return G_SOURCE_CONTINUE;
}

void
clawt_daemon_memory_review_setup(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	/* On the daemon's own context, named here where the source attaches. */
	if (self->review_tick == NULL) {
		if (self->main_context != NULL)
			g_main_context_push_thread_default(self->main_context);
		self->review_tick = clawt_timeout_add_seconds(REVIEW_TICK_SECONDS,
			on_review_tick, self);
		if (self->main_context != NULL)
			g_main_context_pop_thread_default(self->main_context);
	}
}

void
clawt_daemon_memory_review_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->review_tick != NULL) {
		g_source_destroy(self->review_tick);
		g_clear_pointer(&self->review_tick, g_source_unref);
	}

	g_clear_pointer(&self->review_errors, g_hash_table_unref);
}

/* ── The client surface ──────────────────────────────────────────── */

/* The excerpt of one memory, for a proposal's row; NULL when it is gone. */
static gchar *
memory_excerpt(
	GPtrArray   *current,
	const gchar *id
){
	guint i;

	for (i = 0; current != NULL && i < current->len; i++) {
		ClawtMemory *memory = g_ptr_array_index(current, i);

		if (g_strcmp0(memory->id, id) == 0)
			return clawt_clip_line(memory->content, 120);
	}

	return NULL;
}

static JsonNode *
proposals_reply(
	ClawtDaemon *self,
	JsonNode    *request,
	const gchar *agent_id
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) open = NULL;
	g_autoptr(GPtrArray) current = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = review_dir(self);
	g_autofree gchar *label = clawt_ack_check_model_label(review_spec(self));
	ClawtMemoryStore *store;
	gint64 last = 0;
	guint i;

	open = clawt_memory_proposals_load(dir, agent_id, &last, &error);
	if (open == NULL)
		return clawt_ipc_error_new(request, error->code, error->message);

	store = clawt_agent_manager_memory_store_for_scope(self->agents,
		agent_id, CLAWT_MEMORY_SCOPE_AGENT);
	current = (store != NULL)
		? clawt_memory_store_list(store, NULL, FALSE, G_MAXINT, NULL) : NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, agent_id);
	json_builder_set_member_name(builder, "enabled");
	json_builder_add_boolean_value(builder, review_enabled(self));
	json_builder_set_member_name(builder, "model");
	json_builder_add_string_value(builder, review_spec(self));
	json_builder_set_member_name(builder, "model_label");
	json_builder_add_string_value(builder, label);
	json_builder_set_member_name(builder, "interval_hours");
	json_builder_add_int_value(builder, clawt_config_get_int(self->config,
		"memories.review.interval_hours"));
	json_builder_set_member_name(builder, "last_run_at");
	json_builder_add_int_value(builder, last);
	json_builder_set_member_name(builder, "running");
	json_builder_add_boolean_value(builder,
		g_strcmp0(self->review_running, agent_id) == 0);
	clawt_daemon_add_string_member(builder, "last_error",
		self->review_errors != NULL
			? g_hash_table_lookup(self->review_errors, agent_id) : NULL);

	json_builder_set_member_name(builder, "proposals");
	json_builder_begin_array(builder);

	for (i = 0; i < open->len; i++) {
		ClawtMemoryProposal *proposal = g_ptr_array_index(open, i);
		g_autofree gchar *line = clawt_memory_proposal_describe(proposal);
		guint j;

		json_builder_begin_object(builder);
		clawt_daemon_add_string_member(builder, "id", proposal->id);
		clawt_daemon_add_string_member(builder, "kind",
			clawt_enum_to_nick(CLAWT_TYPE_MEMORY_PROPOSAL_KIND,
				(gint)proposal->kind));
		clawt_daemon_add_string_member(builder, "kind_label",
			clawt_memory_proposal_kind_label(proposal->kind));
		clawt_daemon_add_string_member(builder, "line", line);
		clawt_daemon_add_string_member(builder, "content",
			proposal->content);
		clawt_daemon_add_string_member(builder, "reason", proposal->reason);
		json_builder_set_member_name(builder, "created_at");
		json_builder_add_int_value(builder, proposal->created_at);

		/*
		 * Each memory it is about, with its text as it is now -- a
		 * person deciding about "forget m01" needs to read m01 -- and
		 * an empty excerpt for one that has gone since.
		 */
		json_builder_set_member_name(builder, "memories");
		json_builder_begin_array(builder);

		for (j = 0; proposal->memory_ids != NULL &&
			proposal->memory_ids[j] != NULL; j++) {
			g_autofree gchar *excerpt = memory_excerpt(current,
				proposal->memory_ids[j]);

			json_builder_begin_object(builder);
			clawt_daemon_add_string_member(builder, "id",
				proposal->memory_ids[j]);
			clawt_daemon_add_string_member(builder, "excerpt", excerpt);
			json_builder_set_member_name(builder, "present");
			json_builder_add_boolean_value(builder, excerpt != NULL);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

/* Takes one proposal out of the list, applying it first when @accept. */
static JsonNode *
settle_proposal(
	ClawtDaemon *self,
	JsonNode    *request,
	const gchar *agent_id,
	const gchar *proposal_id,
	gboolean     accept
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) open = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = review_dir(self);
	ClawtMemoryProposal *found = NULL;
	gint64 last = 0;
	guint at = 0;
	guint i;

	if (!clawt_memory_proposal_id_is_valid(proposal_id))
		return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			"that is not a proposal id");

	open = clawt_memory_proposals_load(dir, agent_id, &last, &error);
	if (open == NULL)
		return clawt_ipc_error_new(request, error->code, error->message);

	for (i = 0; i < open->len; i++) {
		ClawtMemoryProposal *proposal = g_ptr_array_index(open, i);

		if (g_strcmp0(proposal->id, proposal_id) == 0) {
			found = proposal;
			at = i;
		}
	}

	if (found == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			"no such proposal; it may have been answered already");

	if (accept) {
		ClawtMemoryStore *store = clawt_agent_manager_memory_store_for_scope(
			self->agents, agent_id, CLAWT_MEMORY_SCOPE_AGENT);

		if (store == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
				"that agent has no memory store; memories.enabled is off");

		/*
		 * Through the store's journaled functions with the operator as
		 * the actor, so the change is on the Memory review list with an
		 * Undo like every other.  A proposal that cannot be applied
		 * stays in the list, so the person can read why and reject it.
		 */
		if (!clawt_memory_proposal_apply(found, store, "operator", &error))
			return clawt_ipc_error_new(request, error->code, error->message);
	}

	g_ptr_array_remove_index(open, at);

	if (!clawt_memory_proposals_save(dir, agent_id, open, last, &error))
		return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
			error->message);

	publish_changed(self, agent_id);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, accept ? "accepted" : "rejected");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_memory_review(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

	*handled = TRUE;

	/* The fleet's setting: the model and the interval. */
	if (g_strcmp0(kind, "memory.review_set") == 0) {
		const gchar *model = clawt_ipc_payload_string(payload, "model");
		gboolean has_interval = payload != NULL &&
			json_object_has_member(payload, "interval_hours");
		gint64 hours = clawt_ipc_payload_int(payload, "interval_hours", 0);
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonBuilder) builder = json_builder_new();

		if (model == NULL && !has_interval)
			return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
				"nothing to set: send model (\"\" turns the review off) or "
				"interval_hours");

		if (model != NULL && *model != '\0' &&
			!clawt_ack_check_split_model(model, NULL, NULL))
			return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
				"model is provider/model, or empty for off");

		if (has_interval &&
			(hours < INTERVAL_HOURS_MIN || hours > INTERVAL_HOURS_MAX))
			return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
				"interval_hours is 1 to 720");

		if (model != NULL &&
			!clawt_config_set_string(self->config, "memories.review.model",
				model))
			return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
				"the review model could not be set");

		if (has_interval &&
			!clawt_config_set_int(self->config,
				"memories.review.interval_hours", hours))
			return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
				"the review interval could not be set");

		if (!clawt_config_save(self->config, &error))
			return clawt_ipc_error_new(request,
				error != NULL ? error->code : CLAWT_ERROR_FAILED,
				error != NULL ? error->message
				              : "the setting could not be saved");

		clawt_event_bus_emit(self->bus, "config.changed", NULL);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "model");
		json_builder_add_string_value(builder, review_spec(self));
		json_builder_set_member_name(builder, "interval_hours");
		json_builder_add_int_value(builder, clawt_config_get_int(self->config,
			"memories.review.interval_hours"));
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "memory.proposals") != 0 &&
		g_strcmp0(kind, "memory.proposal_accept") != 0 &&
		g_strcmp0(kind, "memory.proposal_reject") != 0 &&
		g_strcmp0(kind, "memory.review") != 0) {
		*handled = FALSE;
		return NULL;
	}

	if (agent_id == NULL ||
		clawt_agent_manager_get(self->agents, agent_id) == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			"no such agent");

	if (g_strcmp0(kind, "memory.proposals") == 0)
		return proposals_reply(self, request, agent_id);

	if (g_strcmp0(kind, "memory.proposal_accept") == 0 ||
		g_strcmp0(kind, "memory.proposal_reject") == 0)
		return settle_proposal(self, request, agent_id,
			clawt_ipc_payload_string(payload, "id"),
			g_strcmp0(kind, "memory.proposal_accept") == 0);

	/* memory.review: a pass now, for somebody who asked for one. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonBuilder) builder = json_builder_new();

		if (!clawt_daemon_memory_review_start(self, agent_id, &error))
			return clawt_ipc_error_new(request, error->code, error->message);

		publish_changed(self, agent_id);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "started");
		json_builder_add_boolean_value(builder, TRUE);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}
}
