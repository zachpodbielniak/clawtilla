/*
 * daemon-broker.c - The integration broker, as the daemon runs it, and
 * the client surface for it: broker.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Started when the daemon starts with `broker.enabled` on, or when a
 * reload turns it on; stopped with the daemon.  Never waited on: a start
 * pulls an image of several hundred megabytes, so it answers at once and
 * `broker.status` says starting until the gateway answers its health
 * check.  A fleet with the key off makes no broker object at all, so
 * `make test` -- whose fixtures start daemons by the hundred -- never
 * reaches podman for a feature none of them use.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * Every move is announced, so a client drawing the Broker row redraws
 * when it starts, comes up, fails or stops.
 */
static void
on_broker_changed(
	ClawtBroker *broker,
	gpointer     user_data
){
	ClawtDaemon *self = user_data;

	if (self->bus != NULL)
		clawt_event_bus_emit(self->bus, "broker.changed",
		                     clawt_enum_to_nick(
		                         CLAWT_TYPE_BROKER_STATE,
		                         (gint)clawt_broker_get_state(broker)));
}

/* Made on first need, so a fleet without the broker never has one. */
static ClawtBroker *
ensure_broker(
	ClawtDaemon *self
){
	if (self->broker == NULL) {
		self->broker = clawt_broker_new(self->pod_bridge,
		                                self->main_context);
		g_signal_connect(self->broker, "changed",
		                 G_CALLBACK(on_broker_changed), self);
	}

	clawt_broker_set_config(self->broker, self->config, self->state_dir);

	return self->broker;
}

void
clawt_daemon_broker_sync(
	ClawtDaemon *self
){
	ClawtBrokerState state;

	if (self->config == NULL)
		return;

	if (!clawt_config_get_boolean(self->config, "broker.enabled")) {
		/* Turned off while it ran: stop it, and keep what it holds. */
		if (self->broker == NULL)
			return;

		clawt_broker_set_config(self->broker, self->config, self->state_dir);
		state = clawt_broker_get_state(self->broker);

		if (state == CLAWT_BROKER_STATE_STARTING ||
		    state == CLAWT_BROKER_STATE_RUNNING ||
		    state == CLAWT_BROKER_STATE_ERROR)
			clawt_broker_stop(self->broker);

		return;
	}

	/*
	 * On, and not running: start it.  A broker that failed stays failed
	 * until somebody presses start -- a reload for an unrelated edit is
	 * not a request to try again.
	 */
	state = clawt_broker_get_state(ensure_broker(self));

	if (state == CLAWT_BROKER_STATE_STOPPED ||
	    state == CLAWT_BROKER_STATE_DISABLED)
		clawt_broker_start(self->broker);
}

void
clawt_daemon_broker_stop(
	ClawtDaemon *self
){
	if (self->broker == NULL)
		return;

	clawt_broker_stop_sync(self->broker);
}

void
clawt_daemon_broker_release(
	ClawtDaemon *self
){
	if (self->broker == NULL)
		return;

	g_signal_handlers_disconnect_by_data(self->broker, self);
	g_clear_object(&self->broker);
}

JsonNode *
clawt_daemon_broker_refuse_managed(
	ClawtDaemon *self,
	JsonNode    *request,
	const gchar *name
){
	ClawtIntegrationConfig *instance;

	if (name == NULL || self->config == NULL)
		return NULL;

	instance = clawt_config_get_integration(self->config, name);

	if (instance == NULL || !clawt_integration_config_is_managed(instance))
		return NULL;

	/*
	 * Refused where it arrives.  An edit to a connector made from the
	 * config would be accepted, written into a node no save reaches, and
	 * gone at the next reload -- reported as done and never done.
	 */
	{
		g_autofree gchar *message = g_strdup_printf(
			"'%s' is the integration broker's connector, made by "
			"clawtilla from broker.* and the agent's broker_server; "
			"change those, or store its key with `clawtilla broker key`",
			name);

		return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
		                           message);
	}
}

/* ── broker.logs, answered from a worker ── */

typedef struct {
	ClawtDaemon     *daemon;
	ClawtIpcPending *pending;
} LogsJob;

static void
on_logs(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	LogsJob *job = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = clawt_broker_logs_finish(CLAWT_BROKER(source),
	                                                  result, &error);
	JsonNode *request = clawt_ipc_pending_get_request(job->pending);

	if (text == NULL) {
		clawt_ipc_pending_respond(job->pending,
		                          clawt_ipc_error_new(request,
		                                              CLAWT_ERROR_FAILED,
		                                              error->message));
	} else {
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "logs");
		json_builder_add_string_value(builder, text);
		json_builder_end_object(builder);

		clawt_ipc_pending_respond(job->pending,
		                          clawt_ipc_response_new(
		                              request,
		                              json_builder_get_root(builder)));
	}

	g_free(job);
}

static JsonNode *
status_reply(
	ClawtDaemon *self,
	JsonNode    *request
){
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	clawt_broker_build_status(self->broker, self->config, builder);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_broker(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(GError) error = NULL;

	*handled = TRUE;

	if (g_strcmp0(kind, "broker.status") == 0)
		return status_reply(self, request);

	/*
	 * The lifecycle verbs answer with the status straight away, saying
	 * starting or stopping: the work is the broker's, on worker threads,
	 * and a client that waited for a pull would look hung.
	 */
	if (g_strcmp0(kind, "broker.start") == 0 ||
	    g_strcmp0(kind, "broker.restart") == 0) {
		if (!clawt_config_get_boolean(self->config, "broker.enabled"))
			return clawt_ipc_error_new(
				request, CLAWT_ERROR_NOT_SUPPORTED,
				"broker.enabled is off; turn it on first (`clawtilla "
				"config set broker.enabled true`), which also starts it");

		/* Refused here as well as at start, so the reason is the reply. */
		if (!clawt_broker_check_confinement(self->config, &error))
			return clawt_ipc_error_new(request, error->code,
			                           error->message);

		if (g_strcmp0(kind, "broker.start") == 0)
			clawt_broker_start(ensure_broker(self));
		else
			clawt_broker_restart(ensure_broker(self));

		return status_reply(self, request);
	}

	if (g_strcmp0(kind, "broker.stop") == 0) {
		clawt_broker_stop(ensure_broker(self));

		return status_reply(self, request);
	}

	if (g_strcmp0(kind, "broker.logs") == 0) {
		LogsJob *job;

		job = g_new0(LogsJob, 1);
		job->daemon = self;
		job->pending = clawt_ipc_server_defer(self->ipc_server, request);

		if (job->pending == NULL) {
			g_free(job);

			return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
			                           "this request cannot be answered "
			                           "later");
		}

		clawt_broker_logs_async(ensure_broker(self), NULL, on_logs, job);

		return NULL;
	}

	/*
	 * A key for one gateway server, kept 0600 in the shape every
	 * connector's token has.  The path comes back, never the value; and
	 * the key arrives over the socket rather than as an argument, which
	 * would put it in the shell history and the process table.
	 */
	if (g_strcmp0(kind, "broker.key") == 0) {
		const gchar *target = clawt_ipc_payload_string(payload, "target");
		const gchar *key = clawt_ipc_payload_string(payload, "key");
		g_autoptr(ClawtOauthToken) token = NULL;
		g_autoptr(JsonBuilder) builder = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *dir = NULL;
		g_autofree gchar *keys = NULL;
		g_autofree gchar *trimmed = NULL;

		if (!clawt_broker_target_is_valid(target))
			return clawt_ipc_error_new(
				request, CLAWT_ERROR_INVALID_ARGUMENT,
				"name an agent, or a team as team:<id>");

		if (key == NULL || *key == '\0')
			return clawt_ipc_error_new(request,
			                           CLAWT_ERROR_INVALID_ARGUMENT,
			                           "key is required");

		dir = clawt_broker_dir(self->state_dir);
		keys = g_build_filename(dir, "keys", NULL);

		if (!clawt_ensure_dir(dir, 0700, &error) ||
		    !clawt_ensure_dir(keys, 0700, &error))
			return clawt_ipc_error_new(request, error->code,
			                           error->message);

		trimmed = g_strstrip(g_strdup(key));
		token = g_new0(ClawtOauthToken, 1);
		token->access_token = g_steal_pointer(&trimmed);
		path = clawt_broker_key_path(self->state_dir, target);

		if (!clawt_oauth_token_save(token, path, &error))
			return clawt_ipc_error_new(request, error->code,
			                           error->message);

		/*
		 * Re-rendered so a confined agent's box binds the key at its next
		 * start; the connector itself already named this path.
		 */
		clawt_event_bus_emit(self->bus, "broker.changed", "key");

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "key_file");
		json_builder_add_string_value(builder, path);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request,
		                              json_builder_get_root(builder));
	}

	*handled = FALSE;
	return NULL;
}
