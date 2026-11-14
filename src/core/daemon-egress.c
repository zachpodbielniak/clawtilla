/*
 * daemon-egress.c - The egress gate and broker, as the daemon runs them,
 * and the client surface for them: egress.allow.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Both are started the first time a gated or closed agent starts, never
 * at daemon start: a fleet with no such agent binds nothing, and `make
 * test` -- whose fixtures start daemons by the hundred -- opens no socket
 * for a feature none of them use.  Binding a unix socket waits on
 * nothing, so starting them from an agent start holds no loop.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

static gchar *
egress_dir(
	ClawtDaemon *self
){
	return g_build_filename(self->state_dir, "egress", NULL);
}

void
clawt_daemon_egress_refresh_allowlist(
	ClawtDaemon *self
){
	g_auto(GStrv) entries = NULL;
	g_autoptr(ClawtEgressAllowlist) allow = NULL;
	g_autoptr(GError) error = NULL;

	if (self->egress_gate == NULL || self->config == NULL)
		return;

	entries = clawt_config_get_string_list(self->config, "egress.allow");
	allow = clawt_egress_allowlist_new((const gchar * const *)entries,
	                                   &error);

	/*
	 * A list that will not parse opens nothing: the config loader has
	 * already put the reason among its warnings, and the safe direction
	 * for a list nobody can read is the one that permits less.
	 */
	if (allow == NULL)
		g_warning("egress.allow: %s; gated agents reach public addresses "
		          "only until it is fixed", error->message);

	clawt_egress_gate_set_allowlist(self->egress_gate, allow);
}

/*
 * Every refusal is announced, so a client showing the agent can redraw
 * its count; the subject is the agent, which is all a client needs to
 * decide whether it is looking at it.
 */
static void
on_refused(
	ClawtEgressGate    *gate,
	const gchar        *agent_id,
	ClawtEgressRefusal *refusal,
	gpointer            user_data
){
	ClawtDaemon *self = user_data;

	g_message("egress: refused %s for agent %s: %s",
	          refusal->host != NULL ? refusal->host : "?", agent_id,
	          refusal->reason != NULL ? refusal->reason : "");

	if (self->bus != NULL)
		clawt_event_bus_emit(self->bus, "egress.refused", agent_id);
}

gboolean
clawt_daemon_egress_ensure(
	ClawtDaemon  *self,
	GError      **error
){
	g_autofree gchar *dir = egress_dir(self);

	if (self->egress_gate == NULL) {
		g_autofree gchar *gate_dir = g_build_filename(dir, "gate", NULL);

		self->egress_gate = clawt_egress_gate_new(self->main_context,
		                                          gate_dir);
		g_signal_connect(self->egress_gate, "refused",
		                 G_CALLBACK(on_refused), self);
		clawt_daemon_egress_refresh_allowlist(self);
	}

	if (self->egress_broker == NULL)
		self->egress_broker = clawt_egress_broker_new(self->main_context);

	if (clawt_egress_broker_get_socket_path(self->egress_broker) == NULL) {
		g_autofree gchar *path = g_build_filename(dir, "broker.sock", NULL);

		if (!clawt_egress_broker_listen(self->egress_broker, path, error))
			return FALSE;
	}

	return TRUE;
}

void
clawt_daemon_egress_stop(
	ClawtDaemon *self
){
	if (self->egress_gate != NULL)
		clawt_egress_gate_stop(self->egress_gate);

	if (self->egress_broker != NULL)
		clawt_egress_broker_stop(self->egress_broker);
}

void
clawt_daemon_egress_release(
	ClawtDaemon *self
){
	clawt_daemon_egress_stop(self);

	if (self->egress_gate != NULL)
		g_signal_handlers_disconnect_by_data(self->egress_gate, self);

	g_clear_object(&self->egress_gate);
	g_clear_object(&self->egress_broker);
}

gboolean
clawt_daemon_egress_prepare_agent(
	ClawtDaemon          *self,
	ClawtAgentConfig     *config,
	ClawtProcessRuntime  *runtime,
	GError              **error
){
	ClawtRuntimeNetwork network;
	g_autofree gchar *gate_socket = NULL;

	network = clawt_agent_confine_wanted(config)
	          ? (ClawtRuntimeNetwork)clawt_agent_config_get_enum(
	                config, "runtime.network")
	          : CLAWT_RUNTIME_NETWORK_OPEN;

	/*
	 * Asked of the configuration, not of the sandbox: the validator
	 * shadows an agent with a network and no sandbox, and this repeats
	 * the refusal rather than trusting it, since confinement is the
	 * one place a silent downgrade is the worst outcome there is.
	 */
	if (!clawt_agent_confine_wanted(config) &&
	    clawt_agent_config_get_enum(config, "runtime.network") !=
	        CLAWT_RUNTIME_NETWORK_OPEN) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
		                    "runtime.network is not open, and that needs "
		                    "runtime.confine: bwrap; the agent is not run "
		                    "on an open network instead");
		return FALSE;
	}

	if (network != CLAWT_RUNTIME_NETWORK_OPEN) {
		if (!clawt_daemon_egress_ensure(self, error))
			return FALSE;

		if (network == CLAWT_RUNTIME_NETWORK_GATED) {
			gate_socket = clawt_egress_gate_open(
				self->egress_gate, clawt_agent_config_get_id(config), error);

			if (gate_socket == NULL)
				return FALSE;
		}
	}

	clawt_process_runtime_set_egress(runtime, network, gate_socket,
	                                 self->egress_broker);
	return TRUE;
}

/*
 * The agent's credential-looking variables by name, with a placeholder
 * for every value except the base URLs -- which the plan needs to read,
 * and which are addresses rather than secrets.  Nothing is resolved:
 * this answers a listing, and a listing must not run a password
 * manager.
 */
static GHashTable *
names_for_plan(
	ClawtAgentConfig *config
){
	g_autoptr(GHashTable) env = clawt_agent_config_get_env(config);
	g_autoptr(GHashTable) credentials =
		clawt_agent_config_get_credentials(config);
	GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                        g_free);
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (env != NULL) {
		g_hash_table_iter_init(&iter, env);

		while (g_hash_table_iter_next(&iter, &key, &value))
			g_hash_table_insert(out, g_strdup(key),
			                    g_str_has_suffix(key, "_BASE_URL")
			                    ? g_strdup(value) : g_strdup("x"));
	}

	if (credentials != NULL) {
		g_hash_table_iter_init(&iter, credentials);

		while (g_hash_table_iter_next(&iter, &key, &value))
			g_hash_table_insert(out, g_ascii_strup(key, -1), g_strdup("x"));
	}

	return out;
}

void
clawt_daemon_egress_add_status(
	ClawtDaemon *self,
	JsonBuilder *builder,
	ClawtAgent  *agent
){
	ClawtAgentConfig *config = clawt_agent_get_config(agent);
	const gchar *agent_id = clawt_agent_get_id(agent);
	g_autoptr(GHashTable) names = NULL;
	g_autoptr(ClawtEgressBrokerPlan) plan = NULL;
	g_autoptr(GPtrArray) refusals = NULL;
	ClawtRuntimeNetwork network;
	guint i;

	network = (ClawtRuntimeNetwork)clawt_agent_config_get_enum(
		config, "runtime.network");

	names = names_for_plan(config);
	plan = clawt_egress_broker_plan(
		names, clawt_agent_config_get_string(config, "model.provider"),
		clawt_agent_confine_wanted(config) ? network
		                                   : CLAWT_RUNTIME_NETWORK_OPEN,
		NULL);

	refusals = (self->egress_gate != NULL)
	           ? clawt_egress_gate_recent_refusals(self->egress_gate,
	                                               agent_id)
	           : g_ptr_array_new();

	json_builder_set_member_name(builder, "egress");
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "network");
	json_builder_add_string_value(
		builder, clawt_enum_to_nick(CLAWT_TYPE_RUNTIME_NETWORK, network));

	json_builder_set_member_name(builder, "refused_today");
	json_builder_add_int_value(
		builder, self->egress_gate != NULL
		         ? (gint64)clawt_egress_gate_refused_today(self->egress_gate,
		                                                   agent_id)
		         : 0);

	json_builder_set_member_name(builder, "refusals");
	json_builder_begin_array(builder);

	for (i = 0; i < refusals->len; i++) {
		ClawtEgressRefusal *refusal = g_ptr_array_index(refusals, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "at");
		json_builder_add_int_value(builder, refusal->at);
		json_builder_set_member_name(builder, "host");
		json_builder_add_string_value(builder, refusal->host);
		json_builder_set_member_name(builder, "port");
		json_builder_add_int_value(builder, refusal->port);
		if (refusal->address != NULL) {
			json_builder_set_member_name(builder, "address");
			json_builder_add_string_value(builder, refusal->address);
		}
		json_builder_set_member_name(builder, "reason");
		json_builder_add_string_value(builder, refusal->reason);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	/*
	 * Every key-looking variable and whether the broker holds it, with
	 * the reason when it does not -- so "keys brokered" can never be
	 * read off an agent that has one the broker could not take.
	 */
	json_builder_set_member_name(builder, "keys");
	json_builder_begin_array(builder);

	for (i = 0; i < plan->keys->len; i++) {
		ClawtEgressKey *key = g_ptr_array_index(plan->keys, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, key->name);
		json_builder_set_member_name(builder, "brokered");
		json_builder_add_boolean_value(builder, key->reason == NULL);

		if (key->reason != NULL) {
			json_builder_set_member_name(builder, "reason");
			json_builder_add_string_value(builder, key->reason);
		}

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
}

/* ── The client surface ──────────────────────────────────────────── */

static JsonNode *
allow_list_reply(
	ClawtDaemon *self,
	JsonNode    *request
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_auto(GStrv) entries =
		clawt_config_get_string_list(self->config, "egress.allow");
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtEgressAllowlist) parsed =
		clawt_egress_allowlist_new((const gchar * const *)entries, &error);

	guint i;

	/*
	 * Always present, empty or not: a client reading an absent member
	 * gets its fallback, and "no list" must not read as "no reply".
	 */
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "entries");
	json_builder_begin_array(builder);

	for (i = 0; entries != NULL && entries[i] != NULL; i++)
		json_builder_add_string_value(builder, entries[i]);

	json_builder_end_array(builder);

	/*
	 * Said beside the list rather than hidden: a hand-edited entry that
	 * does not parse leaves the whole list opening nothing, and the
	 * person looking at it is the one who can fix it.
	 */
	if (parsed == NULL) {
		json_builder_set_member_name(builder, "problem");
		json_builder_add_string_value(builder, error->message);
	}

	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_egress(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(GError) error = NULL;

	*handled = TRUE;

	if (g_strcmp0(kind, "egress.allow.list") == 0)
		return allow_list_reply(self, request);

	/*
	 * Added and removed one entry at a time, like the fleet's shared
	 * folders, and checked where it arrives: an entry that would not
	 * parse would otherwise be saved, reported as saved, and leave the
	 * whole list opening nothing at the next load.
	 */
	if (g_strcmp0(kind, "egress.allow.add") == 0 ||
	    g_strcmp0(kind, "egress.allow.remove") == 0) {
		const gchar *entry = clawt_ipc_payload_string(payload, "entry");
		g_auto(GStrv) entries =
			clawt_config_get_string_list(self->config, "egress.allow");
		g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
		g_autofree gchar *trimmed = NULL;
		gboolean found = FALSE;
		guint i;

		if (entry == NULL || *entry == '\0')
			return clawt_ipc_error_new(request,
			                           CLAWT_ERROR_INVALID_ARGUMENT,
			                           "entry is required");

		trimmed = g_strstrip(g_strdup(entry));

		if (g_strcmp0(kind, "egress.allow.add") == 0 &&
		    !clawt_egress_allowlist_check_entry(trimmed, &error))
			return clawt_ipc_error_new(request, error->code,
			                           error->message);

		for (i = 0; entries != NULL && entries[i] != NULL; i++) {
			if (g_strcmp0(entries[i], trimmed) == 0) {
				found = TRUE;

				if (g_strcmp0(kind, "egress.allow.remove") == 0)
					continue;
			}

			g_ptr_array_add(out, g_strdup(entries[i]));
		}

		if (g_strcmp0(kind, "egress.allow.remove") == 0 && !found)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			                           "that entry is not in egress.allow");

		if (g_strcmp0(kind, "egress.allow.add") == 0 && !found)
			g_ptr_array_add(out, g_strdup(trimmed));

		g_ptr_array_add(out, NULL);
		clawt_config_set_string_list(self->config, "egress.allow",
		                             (const gchar * const *)out->pdata);

		if (!clawt_config_save(self->config, &error))
			return clawt_ipc_error_new(request, error->code,
			                           error->message);

		/*
		 * Applied to the running gate at once: the list is read per
		 * connection, so unlike a mount nothing needs restarting.
		 */
		clawt_daemon_egress_refresh_allowlist(self);
		clawt_event_bus_emit(self->bus, "config.changed", NULL);

		return allow_list_reply(self, request);
	}

	*handled = FALSE;
	return NULL;
}
