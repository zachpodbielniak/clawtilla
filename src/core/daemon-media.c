/*
 * daemon-media.c - Meshy and PixelLab for agents: keys, jobs and files
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent's `meshy` or `pixellab` integration names a key; the
 * clawtilla_meshy_* and clawtilla_pixellab_* tools reach the service
 * through the client built here, which the tools ask both whether to
 * offer themselves and what to call -- the daemon-search.c shape.
 *
 * - The key is resolved here and handed to ai-glib's client; it never
 *   reaches the agent, a log line or a reply.
 * - A job is remembered (`<state_dir>/media/jobs.json`) with the agent
 *   that started it, so one agent cannot read or cancel another's, and
 *   a restart does not lose the way to its results.  Meshy deletes them
 *   after about three days, which is why they are saved when first seen
 *   done rather than linked.
 * - Results are fetched with a cap enforced while they are read, https
 *   only and with no credential, into the agent's exchange directory
 *   (`<agent>/media/<job>/`), else its workspace, beside a
 *   provenance.yaml.
 * - What each call cost goes into the agent's media-spend file, which
 *   the daily caps read (clawt-mailbox-router.c).
 * - Whether a generation may run at all is decided before this is
 *   reached: every generate tool asks unless a policy rule allows it.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"
#include "clawt-http.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ── Which agents have which service ─────────────────────────────── */

/* The agent's binding of @service, or NULL.  one_per_agent: the first. */
static ClawtIntegrationBinding *
media_binding(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *service
){
	ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
	g_autoptr(GPtrArray) bindings = NULL;
	guint i;

	if (agent == NULL || clawt_agent_config_is_shadow(agent))
		return NULL;

	bindings = clawt_integration_resolve_for_agent(self->config, agent);
	for (i = 0; bindings != NULL && i < bindings->len; i++) {
		ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

		if (g_strcmp0(clawt_integration_binding_get_info(binding)->id,
		              service) == 0)
			return clawt_integration_binding_ref(binding);
	}

	return NULL;
}

static gboolean
media_available(
	const gchar *agent_id,
	const gchar *service,
	gpointer     user_data
){
	ClawtIntegrationBinding *binding = media_binding(user_data, agent_id,
	                                                 service);

	if (binding == NULL)
		return FALSE;
	clawt_integration_binding_unref(binding);
	return TRUE;
}

/*
 * The client for @agent_id's @service, cached per agent, server and key
 * locator so a changed key is a new client.  Built with the daemon's
 * context pushed: libsoup binds a session to the context that was
 * thread-default when it was made.
 */
static AiMediaGenerator *
media_client(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	const gchar  *service,
	GError      **error
){
	ClawtIntegrationBinding *binding;
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *secrets_dir = NULL;
	const gchar *url;
	AiMediaGenerator *client;

	binding = media_binding(self, agent_id, service);
	if (binding == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "no %s integration is bound to %s",
		            clawt_media_service_label(service), agent_id);
		return NULL;
	}

	/* The test seam: the binding is still required, the service is not. */
	if (self->media_client_factory != NULL) {
		clawt_integration_binding_unref(binding);
		return self->media_client_factory(self, service);
	}

	url = clawt_integration_binding_get_string(binding, "url");
	ref = clawt_integration_binding_get_secret(binding, "token");
	key = g_strdup_printf("%s\x1f%s\x1f%s\x1f%s", agent_id, service,
	                      url != NULL ? url : "",
	                      ref != NULL ? clawt_secret_ref_get_locator(ref) : "");

	client = g_hash_table_lookup(self->media_clients, key);
	if (client != NULL) {
		clawt_integration_binding_unref(binding);
		return g_object_ref(client);
	}

	if (ref == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "the %s integration '%s' names no key (token)",
		            clawt_media_service_label(service),
		            clawt_integration_binding_get_name(binding));
		clawt_integration_binding_unref(binding);
		return NULL;
	}

	secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");
	token = clawt_secret_ref_resolve(ref, secrets_dir,
		(guint)clawt_config_get_int(self->config,
		                            "secrets.command_timeout_seconds"),
		error);
	if (token == NULL) {
		g_prefix_error(error, "the %s key for integration '%s': ",
		               clawt_media_service_label(service),
		               clawt_integration_binding_get_name(binding));
		clawt_integration_binding_unref(binding);
		return NULL;
	}

	g_main_context_push_thread_default(self->main_context);
	if (g_strcmp0(service, "meshy") == 0) {
		AiMeshyClient *meshy = ai_meshy_client_new_with_api_key(token);

		if (url != NULL && *url != '\0')
			ai_meshy_client_set_base_url(meshy, url);
		client = AI_MEDIA_GENERATOR(meshy);
	} else {
		AiPixellabClient *pixellab = ai_pixellab_client_new_with_api_key(token);

		if (url != NULL && *url != '\0')
			ai_pixellab_client_set_base_url(pixellab, url);
		client = AI_MEDIA_GENERATOR(pixellab);
	}
	g_main_context_pop_thread_default(self->main_context);

	g_hash_table_insert(self->media_clients, g_steal_pointer(&key),
	                    g_object_ref(client));
	clawt_integration_binding_unref(binding);

	return client;
}

/* ── The jobs we started ─────────────────────────────────────────── */

static gchar *
jobs_path(
	ClawtDaemon *self
){
	return g_build_filename(self->state_dir, "media", "jobs.json", NULL);
}

static void
jobs_save(
	ClawtDaemon *self
){
	g_autofree gchar *path = jobs_path(self);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	if (self->media_jobs == NULL)
		return;

	text = json_to_string(self->media_jobs, TRUE);
	if (g_mkdir_with_parents(dir, 0700) != 0 ||
	    !clawt_write_file_atomic(path, text, -1, 0600, FALSE, &error))
		g_warning("media: the job list was not saved: %s",
		          error != NULL ? error->message : g_strerror(errno));
}

static void
jobs_load(
	ClawtDaemon *self
){
	g_autofree gchar *path = jobs_path(self);
	g_autoptr(JsonParser) parser = json_parser_new();

	g_clear_pointer(&self->media_jobs, json_node_unref);

	if (g_file_test(path, G_FILE_TEST_EXISTS) &&
	    json_parser_load_from_file(parser, path, NULL) &&
	    JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		self->media_jobs = json_node_copy(json_parser_get_root(parser));

	if (self->media_jobs == NULL) {
		/* take, not init: init refs the object, and a new one would leak */
		self->media_jobs = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(self->media_jobs, json_object_new());
	}
}

/* The record of @job_id, or NULL; keyed by service, since ids may collide. */
static JsonObject *
job_record(
	ClawtDaemon *self,
	const gchar *service,
	const gchar *job_id
){
	g_autofree gchar *key = g_strdup_printf("%s:%s", service, job_id);
	JsonObject *jobs = json_node_get_object(self->media_jobs);

	if (job_id == NULL || !json_object_has_member(jobs, key))
		return NULL;

	return json_object_get_object_member(jobs, key);
}

static void
job_remember(
	ClawtDaemon *self,
	const gchar *service,
	const gchar *job_id,
	const gchar *agent_id,
	const gchar *operation,
	const gchar *prompt
){
	JsonObject *record = json_object_new();

	json_object_set_string_member(record, "agent", agent_id);
	json_object_set_string_member(record, "operation",
	                              operation != NULL ? operation : "");
	if (prompt != NULL)
		json_object_set_string_member(record, "prompt", prompt);
	json_object_set_int_member(record, "created",
	                           g_get_real_time() / G_USEC_PER_SEC);
	{
		g_autofree gchar *key = g_strdup_printf("%s:%s", service, job_id);

		/* the name is copied; the record is taken */
		json_object_set_object_member(json_node_get_object(self->media_jobs),
		                              key, record);
	}
	jobs_save(self);
}

/* ── A call in flight ────────────────────────────────────────────── */

typedef struct {
	ClawtDaemon         *daemon;     /* owned ref */
	gchar               *agent_id;
	gchar               *service;
	gchar               *tool;
	gchar               *operation;
	gchar               *prompt;
	gchar               *job_id;     /* as the agent knows it */
	gchar               *poll_id;    /* as the service knows it now */
	AiMediaGenerator    *client;     /* owned */
	ClawtMcpMediaReply   reply;
	gpointer             reply_data;

	/* saving a finished job */
	AiMediaJob          *job;        /* owned */
	gchar               *dir;
	gchar              **names;      /* result names still to fetch */
	guint                at;
	GPtrArray           *files;      /* file names written */
	GBytes              *preview;    /* a small PNG to show, or NULL */
	gchar               *failures;   /* what could not be fetched */
} MediaCall;

static void
media_call_free(
	MediaCall *call
){
	g_clear_object(&call->daemon);
	g_free(call->agent_id);
	g_free(call->service);
	g_free(call->tool);
	g_free(call->operation);
	g_free(call->prompt);
	g_free(call->job_id);
	g_free(call->poll_id);
	g_clear_object(&call->client);
	g_clear_object(&call->job);
	g_free(call->dir);
	g_strfreev(call->names);
	g_clear_pointer(&call->files, g_ptr_array_unref);
	g_clear_pointer(&call->preview, g_bytes_unref);
	g_free(call->failures);
	g_free(call);
}

/* Answers once and frees the call. */
static void
answer(
	MediaCall   *call,
	const gchar *text,
	gboolean     is_error
){
	call->reply(text, is_error ? NULL : call->preview, is_error,
	            call->reply_data);
	media_call_free(call);
}

/* ── Reading pictures the agent named ────────────────────────────── */

/*
 * @path made real and checked against the places the agent may read
 * from: its workspace and the exchange.  Canonicalised first, so `..`
 * and a symlink pointing outward fail the same test.  A relative path is
 * the workspace's.
 */
static gchar *
readable_path(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	const gchar  *path,
	GError      **error
){
	ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
	g_autofree gchar *workspace = agent != NULL
		? clawt_agent_config_get_workspace(agent) : NULL;
	g_autofree gchar *joined = NULL;
	g_autofree gchar *workspace_real = NULL;
	gchar real[PATH_MAX];

	if (workspace == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		                    "you have no workspace to read a picture from");
		return NULL;
	}

	joined = g_path_is_absolute(path) ? g_strdup(path)
	                                  : g_build_filename(workspace, path, NULL);

	if (realpath(joined, real) == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "%s: %s", path, g_strerror(errno));
		return NULL;
	}

	{
		gchar ws[PATH_MAX];

		if (realpath(workspace, ws) != NULL)
			workspace_real = g_strdup(ws);
	}

	if (workspace_real != NULL &&
	    g_str_has_prefix(real, workspace_real) &&
	    real[strlen(workspace_real)] == '/')
		return g_strdup(real);

	if (self->exchange != NULL) {
		gchar ex[PATH_MAX];
		const gchar *root = clawt_exchange_get_root(self->exchange);

		if (root != NULL && realpath(root, ex) != NULL &&
		    g_str_has_prefix(real, ex) && real[strlen(ex)] == '/') {
			/* the exchange's own rule: read anywhere in it */
			g_autofree gchar *checked = clawt_exchange_resolve(
				self->exchange, agent_id, real + strlen(ex) + 1, FALSE, error);

			return g_steal_pointer(&checked);
		}
	}

	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
	            "%s is outside your workspace and the exchange, so it is "
	            "not sent", path);
	return NULL;
}

/* ── Where results go ────────────────────────────────────────────── */

/* A job id as one safe path segment. */
static gchar *
job_segment(
	const gchar *job_id
){
	gchar *safe = g_strdup(job_id != NULL ? job_id : "job");
	gchar *p;

	for (p = safe; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			*p = '-';
	}
	if (*safe == '\0' || *safe == '-')
		*safe = 'j';

	return safe;
}

/*
 * `<exchange>/<agent>/media/<job>/`, made 0700, or the workspace's
 * `media/<job>/` for an agent the exchange is not offered to.
 */
static gchar *
results_dir(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	const gchar  *job_id,
	GError      **error
){
	g_autofree gchar *segment = job_segment(job_id);
	g_autofree gchar *base = NULL;
	gchar *dir;

	if (self->exchange != NULL) {
		ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);

		if (agent != NULL &&
		    clawt_agent_config_get_boolean(agent, "computer.exchange"))
			base = clawt_exchange_resolve(self->exchange, agent_id, agent_id,
			                              TRUE, NULL);
	}

	if (base == NULL) {
		ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);

		base = agent != NULL ? clawt_agent_config_get_workspace(agent) : NULL;
	}

	if (base == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		                    "there is nowhere to save the results");
		return NULL;
	}

	dir = g_build_filename(base, "media", segment, NULL);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "cannot make %s: %s", dir, g_strerror(errno));
		g_free(dir);
		return NULL;
	}

	return dir;
}

/* ── Saving a finished job ───────────────────────────────────────── */

static void fetch_next(MediaCall *call);

/* What the job cost, as a note says it, for provenance and the reply. */
static gchar *
cost_text(
	AiMediaJob *job
){
	gint64 micros = ai_media_job_get_cost_micro_usd(job);
	gint64 credits = ai_media_job_get_credits(job);

	if (micros >= 0)
		return g_strdup_printf("$%.4f", (gdouble)micros / 1e6);
	if (credits >= 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " credits", credits);

	return NULL;
}

static void
finish_saving(
	MediaCall *call
){
	ClawtDaemon *self = call->daemon;
	g_autofree gchar *cost = cost_text(call->job);
	g_autofree gchar *provenance = NULL;
	g_autofree gchar *prov_path = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autoptr(GString) text = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	JsonObject *record;
	guint i;

	provenance = clawt_media_provenance(call->service, call->operation,
	                                    call->prompt, call->job_id, cost,
	                                    call->files);
	prov_path = g_build_filename(call->dir, "provenance.yaml", NULL);
	if (!clawt_write_file_atomic(prov_path, provenance, -1, 0600, FALSE,
	                             &error))
		g_warning("media: %s: provenance not written: %s", call->agent_id,
		          error->message);

	/* What it cost, where the daily caps read it, once per job. */
	record = job_record(self, call->service, call->job_id);
	state_dir = clawt_config_agent_state_dir(self->config, call->agent_id);
	if (record != NULL && !json_object_has_member(record, "saved") &&
	    state_dir != NULL) {
		g_autofree gchar *spend = clawt_media_spend_path(state_dir);
		g_autoptr(GError) spend_error = NULL;

		if (!clawt_media_spend_record(spend, call->service, call->operation,
		                              call->job_id,
		                              ai_media_job_get_cost_micro_usd(call->job),
		                              ai_media_job_get_credits(call->job),
		                              &spend_error))
			g_warning("media: %s: spend not recorded: %s", call->agent_id,
			          spend_error->message);
	}
	if (record != NULL) {
		json_object_set_string_member(record, "saved", call->dir);
		jobs_save(self);
	}

	g_string_append_printf(text, "%s job %s is done%s%s. Saved to %s:\n",
	                       clawt_media_service_label(call->service),
	                       call->job_id, cost != NULL ? ", costing " : "",
	                       cost != NULL ? cost : "", call->dir);
	for (i = 0; i < call->files->len; i++)
		g_string_append_printf(text, "- %s\n",
		                       (const gchar *)g_ptr_array_index(call->files, i));
	g_string_append(text, "- provenance.yaml (where it came from, and the "
	                      "service's terms)\n");
	/*
	 * The service's own id for what was made, which the next step needs:
	 * Meshy's input_task_id / rig_task_id is the last task, PixelLab's
	 * character_id the object.  Ids are `<operation>:<...>` in ai-glib.
	 */
	{
		const gchar *id = call->poll_id != NULL ? call->poll_id : call->job_id;
		g_auto(GStrv) parts = g_strsplit(id, ":", -1);
		guint n = g_strv_length(parts);

		if (g_strcmp0(call->service, "meshy") == 0 && n >= 2)
			g_string_append_printf(text, "Meshy task id, for input_task_id or "
			                       "rig_task_id in a next step: %s\n",
			                       parts[n - 1]);
		else if (g_strcmp0(call->service, "pixellab") == 0 && n >= 2 &&
		         g_str_has_prefix(call->operation != NULL ? call->operation
		                                                  : "", "character"))
			g_string_append_printf(text, "PixelLab character id, for "
			                       "character_id in a next step: %s\n",
			                       parts[1]);
	}

	if (call->failures != NULL)
		g_string_append_printf(text, "Not saved: %s\n", call->failures);
	if (self->exchange != NULL && g_str_has_prefix(call->dir,
	        clawt_exchange_get_root(self->exchange)))
		g_string_append_printf(text, "Inside a computer it is under %s%s.\n",
		                       CLAWT_EXCHANGE_MOUNT_POINT,
		                       call->dir + strlen(clawt_exchange_get_root(
		                           self->exchange)));

	answer(call, text->str, call->files->len == 0);
}

/* Writes one fetched result into the job's directory. */
static void
keep_result(
	MediaCall   *call,
	const gchar *file_name,
	GBytes      *bytes
){
	g_autofree gchar *path = g_build_filename(call->dir, file_name, NULL);
	g_autoptr(GError) error = NULL;
	gsize size = 0;
	gconstpointer data = g_bytes_get_data(bytes, &size);

	if (!clawt_write_file_atomic(path, data, (gssize)size, 0600, FALSE,
	                             &error)) {
		g_autofree gchar *was = call->failures;

		call->failures = g_strdup_printf("%s%s%s (%s)", was != NULL ? was : "",
		                                 was != NULL ? ", " : "", file_name,
		                                 error->message);
		return;
	}

	g_ptr_array_add(call->files, g_strdup(file_name));

	/* The first small PNG is shown to the agent as well as saved. */
	if (call->preview == NULL && g_str_has_suffix(file_name, ".png") &&
	    size <= 1024 * 1024)
		call->preview = g_bytes_ref(bytes);
}

static void
note_failure(
	MediaCall   *call,
	const gchar *file_name,
	const gchar *why
){
	g_autofree gchar *was = call->failures;

	call->failures = g_strdup_printf("%s%s%s (%s)", was != NULL ? was : "",
	                                 was != NULL ? ", " : "", file_name, why);
}

static void
on_result_decoded(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	MediaCall *call = data;
	const gchar *name = call->names[call->at];
	g_autofree gchar *file_name = clawt_media_file_name(name,
		ai_media_job_get_result_url(call->job, name));
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = ai_media_generator_download_finish(
		AI_MEDIA_GENERATOR(source), result, &error);

	if (bytes == NULL)
		note_failure(call, file_name, error->message);
	else if (g_bytes_get_size(bytes) > clawt_media_max_bytes_for(file_name))
		note_failure(call, file_name, "larger than a result may be");
	else
		keep_result(call, file_name, bytes);

	call->at++;
	fetch_next(call);
}

static void
on_result_fetched(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	MediaCall *call = data;
	const gchar *name = call->names[call->at];
	g_autofree gchar *file_name = clawt_media_file_name(name,
		ai_media_job_get_result_url(call->job, name));
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = clawt_mcp_http_send_bounded_finish(result, &error);

	(void)source;

	if (bytes == NULL)
		note_failure(call, file_name, error->message);
	else
		keep_result(call, file_name, bytes);

	call->at++;
	fetch_next(call);
}

/*
 * The next result: a `data:` URL is decoded by the client that made it
 * (no network); anything else must be https, is fetched with no
 * credential, and is refused past its cap while it is read.
 */
static void
fetch_next(
	MediaCall *call
){
	ClawtDaemon *self = call->daemon;
	const gchar *name;
	const gchar *url;
	g_autofree gchar *file_name = NULL;
	g_autoptr(SoupMessage) message = NULL;

	if (call->names == NULL || call->names[call->at] == NULL) {
		finish_saving(call);
		return;
	}

	name = call->names[call->at];
	url = ai_media_job_get_result_url(call->job, name);
	file_name = clawt_media_file_name(name, url);

	if (url == NULL) {
		call->at++;
		fetch_next(call);
		return;
	}

	g_main_context_push_thread_default(self->main_context);

	if (g_str_has_prefix(url, "data:")) {
		ai_media_generator_download_async(call->client, url, NULL,
		                                  on_result_decoded, call);
		g_main_context_pop_thread_default(self->main_context);
		return;
	}

	if (!g_str_has_prefix(url, "https://")) {
		g_main_context_pop_thread_default(self->main_context);
		note_failure(call, file_name, "not an https link");
		call->at++;
		fetch_next(call);
		return;
	}

	message = clawt_http_message_new("GET", url, CLAWT_HTTP_NO_CREDENTIALS);
	if (message == NULL) {
		g_main_context_pop_thread_default(self->main_context);
		note_failure(call, file_name, "the link cannot be parsed");
		call->at++;
		fetch_next(call);
		return;
	}

	if (self->media_session == NULL)
		self->media_session = soup_session_new();

	clawt_mcp_http_send_bounded_async(self->media_session, message,
	                                  clawt_media_max_bytes_for(file_name),
	                                  NULL, on_result_fetched, call);
	g_main_context_pop_thread_default(self->main_context);
}

/* Saves every named result of a finished job, then answers. */
static void
save_job(
	MediaCall  *call,
	AiMediaJob *job
){
	g_autoptr(GError) error = NULL;
	const gchar * const *names = ai_media_job_get_result_names(job);

	call->job = g_object_ref(job);
	call->dir = results_dir(call->daemon, call->agent_id, call->job_id, &error);
	if (call->dir == NULL) {
		answer(call, error->message, TRUE);
		return;
	}

	call->names = g_strdupv((gchar **)names);
	call->at = 0;
	call->files = g_ptr_array_new_with_free_func(g_free);
	fetch_next(call);
}

/* ── The tools ───────────────────────────────────────────────────── */

/* A job seen again: done and saved, still going, or failed. */
static void
on_polled(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	MediaCall *call = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = ai_media_generator_poll_finish(
		AI_MEDIA_GENERATOR(source), result, &error);
	g_autofree gchar *text = NULL;

	if (job == NULL) {
		text = g_strdup_printf("%s could not say how job %s is doing: %s",
		                       clawt_media_service_label(call->service),
		                       call->job_id, error->message);
		answer(call, text, TRUE);
		return;
	}

	/*
	 * A job can change its id as it goes -- Meshy's text-to-3d becomes
	 * <preview>:<refine> once the refine starts, and only the newer id
	 * resumes in a fresh process -- so the newest is kept beside the one
	 * the agent was given.
	 */
	if (ai_media_job_get_id(job) != NULL &&
	    g_strcmp0(ai_media_job_get_id(job),
	              call->poll_id != NULL ? call->poll_id : call->job_id) != 0) {
		JsonObject *record = job_record(call->daemon, call->service,
		                                call->job_id);

		g_free(call->poll_id);
		call->poll_id = g_strdup(ai_media_job_get_id(job));
		if (record != NULL) {
			json_object_set_string_member(record, "latest", call->poll_id);
			jobs_save(call->daemon);
		}
	}

	switch (ai_media_job_get_state(job)) {
	case AI_MEDIA_JOB_STATE_DONE:
		save_job(call, job);
		return;
	case AI_MEDIA_JOB_STATE_FAILED:
	case AI_MEDIA_JOB_STATE_CANCELLED:
		text = g_strdup_printf("%s job %s %s: %s",
		                       clawt_media_service_label(call->service),
		                       call->job_id,
		                       ai_media_job_get_state(job) ==
		                           AI_MEDIA_JOB_STATE_FAILED
		                       ? "failed" : "was cancelled",
		                       ai_media_job_get_error_message(job) != NULL
		                       ? ai_media_job_get_error_message(job)
		                       : "the service gave no reason");
		answer(call, text, TRUE);
		return;
	case AI_MEDIA_JOB_STATE_QUEUED:
	case AI_MEDIA_JOB_STATE_RUNNING:
		break;
	}

	text = g_strdup_printf("%s job %s is %s%s%s. Check again with %s in a "
	                       "minute or two -- or finish this turn and come "
	                       "back to it.",
	                       clawt_media_service_label(call->service),
	                       call->job_id,
	                       ai_media_job_get_state(job) ==
	                           AI_MEDIA_JOB_STATE_QUEUED ? "queued" : "running",
	                       ai_media_job_get_provider_status(job) != NULL
	                       ? " -- " : "",
	                       ai_media_job_get_provider_status(job) != NULL
	                       ? ai_media_job_get_provider_status(job) : "",
	                       call->tool);
	answer(call, text, FALSE);
}

static void
poll_job(
	MediaCall *call
){
	g_main_context_push_thread_default(call->daemon->main_context);
	ai_media_generator_poll_async(call->client,
	                              call->poll_id != NULL ? call->poll_id
	                                                    : call->job_id,
	                              NULL, on_polled, call);
	g_main_context_pop_thread_default(call->daemon->main_context);
}

static void
on_submitted(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	MediaCall *call = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *job_id = ai_media_generator_submit_finish(
		AI_MEDIA_GENERATOR(source), result, &error);
	g_autofree gchar *job_tool = NULL;

	if (job_id == NULL) {
		g_autofree gchar *text = g_strdup_printf("%s refused it: %s",
			clawt_media_service_label(call->service), error->message);

		answer(call, text, TRUE);
		return;
	}

	call->job_id = g_steal_pointer(&job_id);
	job_remember(call->daemon, call->service, call->job_id, call->agent_id,
	             call->operation, call->prompt);

	/*
	 * Looked at once at once: PixelLab's pictures are done at submit,
	 * and saving them now is the whole answer.  Anything still running
	 * answers with the id and the tool to check it with.
	 */
	job_tool = g_strdup_printf("clawtilla_%s_job", call->service);
	g_free(call->tool);
	call->tool = g_steal_pointer(&job_tool);
	poll_job(call);
}

static void
on_cancelled(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	MediaCall *call = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	if (!ai_media_generator_cancel_finish(AI_MEDIA_GENERATOR(source), result,
	                                      &error)) {
		text = g_strdup_printf("Job %s was not cancelled: %s", call->job_id,
		                       error->message);
		answer(call, text, TRUE);
		return;
	}

	text = g_strdup_printf("Job %s is cancelled.", call->job_id);
	answer(call, text, FALSE);
}

static void
on_balance(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	MediaCall *call = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	if (AI_IS_MESHY_CLIENT(source)) {
		gint64 balance = 0;
		gint64 monthly = 0;
		gint64 permanent = 0;

		if (ai_meshy_client_balance_finish(AI_MESHY_CLIENT(source), result,
		                                   &balance, &monthly, &permanent,
		                                   &error))
			text = g_strdup_printf("Meshy: %" G_GINT64_FORMAT " credits left "
			                       "(%" G_GINT64_FORMAT " this month's, %"
			                       G_GINT64_FORMAT " kept).",
			                       balance, monthly, permanent);
	} else {
		gint64 micros = 0;
		gdouble generations = 0;
		gdouble total = 0;
		g_autofree gchar *status = NULL;

		if (ai_pixellab_client_balance_finish(AI_PIXELLAB_CLIENT(source),
		                                      result, &micros, &generations,
		                                      &total, &status, &error))
			text = (total > 0)
				? g_strdup_printf("PixelLab: $%.2f of credit, and %.0f of %.0f "
				                  "subscription generations left (%s).",
				                  (gdouble)micros / 1e6, generations, total,
				                  status != NULL ? status : "?")
				: g_strdup_printf("PixelLab: $%.2f of credit.",
				                  (gdouble)micros / 1e6);
	}

	if (text == NULL) {
		text = g_strdup_printf("%s did not say: %s",
		                       clawt_media_service_label(call->service),
		                       error != NULL ? error->message : "unknown");
		answer(call, text, TRUE);
		return;
	}

	answer(call, text, FALSE);
}

/* A string argument, or NULL. */
static const gchar *
argument(
	JsonObject  *arguments,
	const gchar *name
){
	if (arguments == NULL || !json_object_has_member(arguments, name))
		return NULL;
	if (json_node_get_value_type(json_object_get_member(arguments, name)) !=
	    G_TYPE_STRING)
		return NULL;

	return json_object_get_string_member(arguments, name);
}

static void
start_generate(
	MediaCall  *call,
	JsonObject *arguments
){
	ClawtDaemon *self = call->daemon;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaRequest) request = clawt_media_request_new(call->service,
	                                                            arguments, &error);
	JsonNode *images;

	if (request == NULL) {
		answer(call, error->message, TRUE);
		return;
	}

	call->operation = g_strdup(argument(arguments, "operation"));
	call->prompt = g_strdup(argument(arguments, "prompt"));

	/* Each picture read from where the agent may read, and nowhere else. */
	images = (arguments != NULL && json_object_has_member(arguments, "images"))
		? json_object_get_member(arguments, "images") : NULL;
	if (images != NULL && JSON_NODE_HOLDS_OBJECT(images)) {
		JsonObject *object = json_node_get_object(images);
		GList *members = json_object_get_members(object);
		GList *l;

		for (l = members; l != NULL; l = l->next) {
			const gchar *param = l->data;
			JsonNode *value = json_object_get_member(object, param);
			g_autofree gchar *real = NULL;

			if (!JSON_NODE_HOLDS_VALUE(value) ||
			    json_node_get_value_type(value) != G_TYPE_STRING) {
				g_set_error(&error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
				            "images.%s must be a path", param);
				break;
			}

			real = readable_path(self, call->agent_id,
			                     json_node_get_string(value), &error);
			if (real == NULL ||
			    !clawt_media_request_add_image(call->service, request, param,
			                                   real, &error))
				break;
		}

		g_list_free(members);
		if (error != NULL) {
			answer(call, error->message, TRUE);
			return;
		}
	}

	g_main_context_push_thread_default(self->main_context);
	ai_media_generator_submit_async(call->client, request, NULL, on_submitted,
	                                call);
	g_main_context_pop_thread_default(self->main_context);
}

static void
media_call(
	const gchar        *agent_id,
	const gchar        *tool,
	JsonObject         *arguments,
	ClawtMcpMediaReply  reply,
	gpointer            reply_data,
	gpointer            user_data
){
	ClawtDaemon *self = user_data;
	const gchar *service = clawt_media_service_for_tool(tool);
	g_autoptr(GError) error = NULL;
	MediaCall *call = g_new0(MediaCall, 1);

	call->daemon = g_object_ref(self);
	call->agent_id = g_strdup(agent_id);
	call->service = g_strdup(service);
	call->tool = g_strdup(tool);
	call->reply = reply;
	call->reply_data = reply_data;

	call->client = media_client(self, agent_id, service, &error);
	if (call->client == NULL) {
		answer(call, error->message, TRUE);
		return;
	}

	if (g_str_has_suffix(tool, "_generate")) {
		start_generate(call, arguments);
		return;
	}

	if (g_str_has_suffix(tool, "_balance")) {
		g_main_context_push_thread_default(self->main_context);
		if (AI_IS_MESHY_CLIENT(call->client))
			ai_meshy_client_balance_async(AI_MESHY_CLIENT(call->client), NULL,
			                              on_balance, call);
		else
			ai_pixellab_client_balance_async(AI_PIXELLAB_CLIENT(call->client),
			                                 NULL, on_balance, call);
		g_main_context_pop_thread_default(self->main_context);
		return;
	}

	/* _job and _cancel: only a job this agent started. */
	{
		const gchar *job_id = argument(arguments, "job_id");
		JsonObject *record = job_record(self, service, job_id);

		if (record == NULL ||
		    g_strcmp0(json_object_get_string_member_with_default(record,
		                  "agent", ""), agent_id) != 0) {
			answer(call, "There is no job by that id that you started.", TRUE);
			return;
		}

		call->job_id = g_strdup(job_id);
		call->poll_id = g_strdup(json_object_get_string_member_with_default(
			record, "latest", NULL));
		call->operation = g_strdup(json_object_get_string_member_with_default(
			record, "operation", ""));
		call->prompt = g_strdup(json_object_get_string_member_with_default(
			record, "prompt", NULL));

		if (g_str_has_suffix(tool, "_cancel")) {
			g_main_context_push_thread_default(self->main_context);
			ai_media_generator_cancel_async(call->client,
			                                call->poll_id != NULL
			                                ? call->poll_id : job_id,
			                                NULL, on_cancelled, call);
			g_main_context_pop_thread_default(self->main_context);
			return;
		}

		poll_job(call);
	}
}

/* ── For the office ──────────────────────────────────────────────── */

/*
 * The client the office makes characters with: the key stored with
 * `clawtilla office api-key <service>` (secrets.dir/office/<service>.key),
 * else the first agent's binding of that service -- the account somebody
 * already gave the fleet.  The test seam stands in for both.
 */
AiMediaGenerator *
clawt_daemon_media_office_client(
	ClawtDaemon  *self,
	const gchar  *service,
	GError      **error
){
	g_autofree gchar *secrets_dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *key = NULL;
	GPtrArray *agents;
	AiMediaGenerator *client = NULL;
	guint i;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

	if (self->media_client_factory != NULL)
		return self->media_client_factory(self, service);

	secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");
	path = secrets_dir != NULL
		? g_build_filename(secrets_dir, "office", service, NULL) : NULL;
	if (path != NULL) {
		g_autofree gchar *file = g_strconcat(path, ".key", NULL);

		if (g_file_get_contents(file, &key, NULL, NULL))
			g_strstrip(key);
	}

	if (key != NULL && *key != '\0') {
		g_main_context_push_thread_default(self->main_context);
		client = g_strcmp0(service, "meshy") == 0
			? AI_MEDIA_GENERATOR(ai_meshy_client_new_with_api_key(key))
			: AI_MEDIA_GENERATOR(ai_pixellab_client_new_with_api_key(key));
		g_main_context_pop_thread_default(self->main_context);
		return client;
	}

	agents = clawt_config_get_agents(self->config);
	for (i = 0; agents != NULL && i < agents->len; i++) {
		const gchar *id = clawt_agent_config_get_id(g_ptr_array_index(agents, i));

		if (media_available(id, service, self))
			return media_client(self, id, service, error);
	}

	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
	            "no %s key: store one with `clawtilla office api-key %s`, or "
	            "bind a %s integration to an agent",
	            clawt_media_service_label(service), service, service);
	return NULL;
}

static void
on_office_fetched(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	GTask *task = data;
	g_autoptr(GError) error = NULL;
	GBytes *bytes = AI_IS_MEDIA_GENERATOR(source)
		? ai_media_generator_download_finish(AI_MEDIA_GENERATOR(source), result,
		                                     &error)
		: clawt_mcp_http_send_bounded_finish(result, &error);
	gsize cap = GPOINTER_TO_SIZE(g_task_get_task_data(task));

	if (bytes == NULL)
		g_task_return_error(task, g_steal_pointer(&error));
	else if (g_bytes_get_size(bytes) > cap) {
		g_bytes_unref(bytes);
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                        "the result is larger than one may be");
	} else
		g_task_return_pointer(task, bytes, (GDestroyNotify)g_bytes_unref);
	g_object_unref(task);
}

void
clawt_daemon_media_fetch_async(
	ClawtDaemon         *self,
	AiMediaGenerator    *client,
	const gchar         *url,
	gsize                max_bytes,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	g_autoptr(SoupMessage) message = NULL;

	g_main_context_push_thread_default(self->main_context);
	task = g_task_new(NULL, NULL, callback, user_data);
	g_task_set_task_data(task, GSIZE_TO_POINTER(max_bytes), NULL);

	/* the same rules as an agent's results: data: decoded, else https only */
	if (url != NULL && g_str_has_prefix(url, "data:")) {
		ai_media_generator_download_async(client, url, NULL, on_office_fetched,
		                                  task);
	} else if (url == NULL || !g_str_has_prefix(url, "https://") ||
	           (message = clawt_http_message_new("GET", url,
	                                             CLAWT_HTTP_NO_CREDENTIALS)) == NULL) {
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		                        "the result is not an https link");
		g_object_unref(task);
	} else {
		if (self->media_session == NULL)
			self->media_session = soup_session_new();
		clawt_mcp_http_send_bounded_async(self->media_session, message,
		                                  max_bytes, NULL, on_office_fetched,
		                                  task);
	}
	g_main_context_pop_thread_default(self->main_context);
}

GBytes *
clawt_daemon_media_fetch_finish(
	GAsyncResult  *result,
	GError       **error
){
	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Wiring ──────────────────────────────────────────────────────── */

void
clawt_daemon_media_setup(
	ClawtDaemon *self
){
	static const ClawtMcpMediaHooks hooks = { media_available, media_call };

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->media_clients == NULL)
		self->media_clients = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                            g_free, g_object_unref);
	jobs_load(self);

	if (self->mcp_tools != NULL)
		clawt_mcp_tools_set_media_hooks(self->mcp_tools, &hooks, self, NULL);
}

void
clawt_daemon_media_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	/* The tools lose the hook before the clients it hands out go. */
	if (self->mcp_tools != NULL)
		clawt_mcp_tools_set_media_hooks(self->mcp_tools, NULL, NULL, NULL);
	g_clear_pointer(&self->media_clients, g_hash_table_unref);
	g_clear_pointer(&self->media_jobs, json_node_unref);
	g_clear_object(&self->media_session);
}
