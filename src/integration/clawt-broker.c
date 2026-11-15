/*
 * clawt-broker.c - The integration broker clawtilla runs in podman
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything below was run by hand under rootless podman before it was
 * written down (docs/connectors.org, "Running it by hand"), and the
 * shapes here are what that showed rather than what the upstream compose
 * files say:
 *
 * - AnythingMCP's compose file puts Caddy in front for TLS and a public
 *   name.  A loopback gateway reached by a relay on this machine needs
 *   neither, so it is two containers: the gateway, and PostgreSQL.
 * - The two share no network.  podomation's create cannot join one
 *   container to another's namespace, and pasta maps the host address
 *   containers see to the machine's *external* address, so a database
 *   published on loopback is unreachable from beside it.  They share a
 *   unix socket in a bind mount instead, and the database publishes no
 *   port at all.
 * - Jentic One's control plane hands executions to its broker on its own
 *   127.0.0.1:8100 and sends a bearer over plain http only to loopback.
 *   The compose file gets that with `network_mode: service:app`, which
 *   podomation cannot express either, so both run as two processes of
 *   one container -- still each the sole surface of its process, which
 *   is what Jentic asks.
 */

#include "clawtilla.h"
#include "clawt-http.h"
#include "integration/clawt-broker.h"
#include "integration/clawt-broker-private.h"

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <yaml-glib.h>

/* Where the database's socket appears in both AnythingMCP containers. */
#define BROKER_SOCKET_DIR "/clawt-run"

/* A gateway that has not answered in this long is reported, not waited on. */
#define BROKER_START_DEADLINE_SECONDS (10 * 60)

/* The restart policy's ceiling: this many restarts within the window. */
#define BROKER_RESTART_LIMIT 5
#define BROKER_RESTART_WINDOW_SECONDS (10 * 60)

/* How often the watch looks, while starting and once running. */
#define BROKER_WATCH_STARTING_SECONDS 3
#define BROKER_WATCH_RUNNING_SECONDS 30

/* Health checks in a row that may fail before the state says so. */
#define BROKER_UNHEALTHY_LIMIT 3

/* The most of a gateway's own reason a refusal repeats. */
#define BROKER_DETAIL_CHARS 200

/* ── What each broker is ─────────────────────────────────────────── */

const gchar *
clawt_broker_type_label(
	ClawtBrokerType type
){
	switch (type) {
	case CLAWT_BROKER_ANYTHINGMCP:
		return "AnythingMCP";
	case CLAWT_BROKER_JENTIC:
		return "Jentic One";
	}

	return "the integration broker";
}

const gchar *
clawt_broker_type_default_image(
	ClawtBrokerType type
){
	switch (type) {
	case CLAWT_BROKER_ANYTHINGMCP:
		return CLAWT_BROKER_ANYTHINGMCP_IMAGE;
	case CLAWT_BROKER_JENTIC:
		return CLAWT_BROKER_JENTIC_IMAGE;
	}

	return CLAWT_BROKER_ANYTHINGMCP_IMAGE;
}

gboolean
clawt_broker_type_for_provider(
	const gchar     *provider,
	ClawtBrokerType *type
){
	gint value = 0;

	if (provider == NULL)
		return FALSE;

	if (!clawt_enum_from_nick(CLAWT_TYPE_BROKER_TYPE, provider, &value))
		return FALSE;

	if (type != NULL)
		*type = (ClawtBrokerType)value;

	return TRUE;
}

/*
 * No default: -Wswitch names a state somebody adds and forgets to word,
 * and a word every client draws is not one to fall through to.
 */
const gchar *
clawt_broker_state_label(
	ClawtBrokerState state
){
	switch (state) {
	case CLAWT_BROKER_STATE_DISABLED:
		return "Off";
	case CLAWT_BROKER_STATE_STOPPED:
		return "Stopped";
	case CLAWT_BROKER_STATE_STARTING:
		return "Starting";
	case CLAWT_BROKER_STATE_RUNNING:
		return "Running";
	case CLAWT_BROKER_STATE_STOPPING:
		return "Stopping";
	case CLAWT_BROKER_STATE_ERROR:
		return "Failed";
	}

	return "Unknown";
}

gboolean
clawt_broker_check_image(
	const gchar  *image,
	GError      **error
){
	const gchar *last_slash;
	const gchar *colon;

	if (image == NULL || *image == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		                    "the broker needs an image");
		return FALSE;
	}

	/* A digest names exactly one image, which is the point of a pin. */
	if (strchr(image, '@') != NULL)
		return TRUE;

	/*
	 * The tag is after the last colon *after the last slash*: a
	 * registry with a port (localhost:5000/anythingmcp) has a colon that
	 * is not a tag at all.
	 */
	last_slash = strrchr(image, '/');
	colon = strrchr(last_slash != NULL ? last_slash : image, ':');

	if (colon == NULL || colon[1] == '\0') {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "broker image '%s' has no tag; pin one, such as %s",
		            image, CLAWT_BROKER_ANYTHINGMCP_IMAGE);
		return FALSE;
	}

	if (g_ascii_strcasecmp(colon + 1, "latest") == 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "broker image '%s' is tagged latest, which changes "
		            "under you; pin a release tag instead", image);
		return FALSE;
	}

	return TRUE;
}

ClawtBrokerType
clawt_broker_get_type_value(
	ClawtConfig *config
){
	g_return_val_if_fail(CLAWT_IS_CONFIG(config), CLAWT_BROKER_ANYTHINGMCP);

	return (ClawtBrokerType)clawt_config_get_enum(config, "broker.type");
}

gchar *
clawt_broker_get_image(
	ClawtConfig *config
){
	const gchar *image;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	image = clawt_config_get_string(config, "broker.image");

	if (image != NULL && *image != '\0')
		return g_strdup(image);

	return g_strdup(clawt_broker_type_default_image(
		clawt_broker_get_type_value(config)));
}

static gchar *
database_image(
	ClawtConfig *config
){
	const gchar *image = clawt_config_get_string(config,
	                                             "broker.database_image");

	if (image != NULL && *image != '\0')
		return g_strdup(image);

	return g_strdup(CLAWT_BROKER_DATABASE_IMAGE);
}

/* ── Where it is ─────────────────────────────────────────────────── */

gchar *
clawt_broker_dir(
	const gchar *state_dir
){
	g_return_val_if_fail(state_dir != NULL, NULL);

	return g_build_filename(state_dir, "broker", NULL);
}

gchar *
clawt_broker_dial_host(
	const gchar *bind
){
	/*
	 * A gateway published on every address is reached on loopback: the
	 * relay runs here, and a wildcard is not an address anything can
	 * dial.
	 */
	if (bind == NULL || *bind == '\0' || g_strcmp0(bind, "0.0.0.0") == 0)
		return g_strdup("127.0.0.1");

	if (g_strcmp0(bind, "::") == 0 || g_strcmp0(bind, "[::]") == 0)
		return g_strdup("[::1]");

	if (strchr(bind, ':') != NULL && bind[0] != '[')
		return g_strdup_printf("[%s]", bind);

	return g_strdup(bind);
}

static gchar *
base_url(
	ClawtConfig *config,
	const gchar *port_key
){
	g_autofree gchar *host = clawt_broker_dial_host(
		clawt_config_get_string(config, "broker.bind"));

	return g_strdup_printf("http://%s:%" G_GINT64_FORMAT, host,
	                       clawt_config_get_int(config, port_key));
}

gchar *
clawt_broker_mcp_url(
	ClawtConfig *config,
	const gchar *server
){
	g_autofree gchar *base = NULL;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	base = base_url(config, "broker.port");

	/*
	 * AnythingMCP serves one endpoint per gateway server, by id; a key
	 * made for one server is refused by every other, so the URL and the
	 * key say the same thing twice and a mismatch is a 403 rather than a
	 * call landing on the wrong server.  Jentic One has one endpoint and
	 * tells agents apart by key.
	 */
	if (clawt_broker_get_type_value(config) == CLAWT_BROKER_ANYTHINGMCP &&
	    server != NULL && *server != '\0')
		return g_strdup_printf("%s/mcp/%s", base, server);

	return g_strdup_printf("%s/mcp", base);
}

gchar *
clawt_broker_dashboard_url(
	ClawtConfig *config
){
	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	if (clawt_broker_get_type_value(config) == CLAWT_BROKER_JENTIC)
		return base_url(config, "broker.port");

	return base_url(config, "broker.dashboard_port");
}

gchar *
clawt_broker_health_url(
	ClawtConfig *config
){
	g_autofree gchar *base = NULL;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	base = base_url(config, "broker.port");

	return g_strdup_printf("%s/health", base);
}

/* ── Who reaches it ──────────────────────────────────────────────── */

gboolean
clawt_broker_target_is_valid(
	const gchar *target
){
	if (target == NULL || *target == '\0')
		return FALSE;

	if (g_str_has_prefix(target, "team:"))
		return clawt_is_valid_id(target + 5);

	return clawt_is_valid_id(target);
}

gchar *
clawt_broker_connector_name(
	const gchar *target
){
	g_return_val_if_fail(target != NULL, NULL);

	if (g_str_has_prefix(target, "team:"))
		return g_strdup_printf("broker-team-%s", target + 5);

	return g_strdup_printf("broker-agent-%s", target);
}

gchar *
clawt_broker_target_for_connector(
	const gchar *name
){
	if (name == NULL)
		return NULL;

	if (g_str_has_prefix(name, "broker-team-") && name[12] != '\0')
		return g_strdup_printf("team:%s", name + 12);

	if (g_str_has_prefix(name, "broker-agent-") && name[13] != '\0')
		return g_strdup(name + 13);

	return NULL;
}

gchar *
clawt_broker_key_path(
	const gchar *state_dir,
	const gchar *target
){
	g_autofree gchar *dir = NULL;
	g_autofree gchar *file = NULL;

	g_return_val_if_fail(state_dir != NULL, NULL);
	g_return_val_if_fail(clawt_broker_target_is_valid(target), NULL);

	dir = clawt_broker_dir(state_dir);

	/* A colon in a file name is legal and confuses every PATH-like list. */
	if (g_str_has_prefix(target, "team:"))
		file = g_strdup_printf("team-%s.json", target + 5);
	else
		file = g_strdup_printf("agent-%s.json", target);

	return g_build_filename(dir, "keys", file, NULL);
}

gchar *
clawt_broker_agent_refusal(
	ClawtConfig      *config,
	ClawtAgentConfig *agent
){
	ClawtRuntimeNetwork network;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
	g_return_val_if_fail(agent != NULL, NULL);

	/*
	 * The gateway listens on this machine's loopback, and a gated box
	 * has a loopback of its own with nothing on it.  Offering the
	 * connector anyway would describe a tool that can only fail.
	 */
	network = (ClawtRuntimeNetwork)clawt_agent_config_get_enum(
		agent, "runtime.network");

	if (network != CLAWT_RUNTIME_NETWORK_OPEN)
		return g_strdup_printf(
			"its runtime.network is %s, and the broker is reached on this "
			"machine's loopback, which a %s sandbox cannot reach",
			clawt_enum_to_nick(CLAWT_TYPE_RUNTIME_NETWORK, (gint)network),
			clawt_enum_to_nick(CLAWT_TYPE_RUNTIME_NETWORK, (gint)network));

	/*
	 * Jentic One's own threat model: an agent running as the same OS
	 * user as the instance can read the credential database and the key
	 * that decrypts it, and goes around the broker rather than through
	 * it.  bwrap gives the agent a private home and binds nothing of
	 * <state_dir>/broker but its own key, which is the isolation that
	 * document asks for.
	 */
	if (clawt_broker_get_type_value(config) == CLAWT_BROKER_JENTIC &&
	    clawt_agent_config_get_enum(agent, "runtime.confine") !=
	    CLAWT_RUNTIME_CONFINE_BWRAP)
		return g_strdup(
			"Jentic One is only given to agents with runtime.confine: "
			"bwrap -- its own documentation says an agent running as the "
			"same OS user can read its credential store");

	return NULL;
}

void
clawt_broker_assignment_free(
	ClawtBrokerAssignment *self
){
	if (self == NULL)
		return;

	g_free(self->target);
	g_free(self->server);
	g_free(self->connector);
	g_free(self->key_file);
	g_strfreev(self->agents);
	g_free(self);
}

/*
 * The server an agent names, and whose it is: its own, or its team's.
 * NULL when neither names one.
 */
static gchar *
server_for_agent(
	ClawtConfig       *config,
	ClawtAgentConfig  *agent,
	gchar            **target
){
	const gchar *own = clawt_agent_config_get_string(agent, "broker_server");
	const gchar *team;
	g_autoptr(ClawtTeamSpec) spec = NULL;

	*target = NULL;

	if (own != NULL && *own != '\0') {
		*target = g_strdup(clawt_agent_config_get_id(agent));
		return g_strdup(own);
	}

	team = clawt_agent_config_get_string(agent, "team");

	if (team == NULL || *team == '\0')
		return NULL;

	spec = clawt_config_get_team(config, team);

	if (spec == NULL || spec->broker_server == NULL ||
	    *spec->broker_server == '\0')
		return NULL;

	*target = g_strdup_printf("team:%s", team);

	return g_strdup(spec->broker_server);
}

GPtrArray *
clawt_broker_assignments(
	ClawtConfig *config,
	GPtrArray   *warnings
){
	GPtrArray *out;
	GPtrArray *agents;
	g_autoptr(GHashTable) members = NULL;   /* target -> GPtrArray of ids */
	g_autoptr(GHashTable) servers = NULL;   /* target -> server */
	g_autoptr(GPtrArray) order = NULL;      /* targets, first seen first */
	const gchar *state_dir_key = "daemon.state_dir";
	g_autofree gchar *state_dir = NULL;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_broker_assignment_free);

	if (!clawt_config_get_boolean(config, "broker.enabled"))
		return out;

	state_dir = clawt_config_get_path_value(config, state_dir_key);
	members = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                (GDestroyNotify)g_ptr_array_unref);
	servers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	order = g_ptr_array_new_with_free_func(g_free);
	agents = clawt_config_get_agents(config);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
		g_autofree gchar *target = NULL;
		g_autofree gchar *server = server_for_agent(config, agent, &target);
		g_autofree gchar *refusal = NULL;
		GPtrArray *ids;

		if (server == NULL)
			continue;

		/*
		 * A server id lands in a URL path, so it is held to the same
		 * rules as a name: one that could carry a slash or a query
		 * could aim the relay at an endpoint nobody configured.
		 */
		if (!clawt_broker_target_is_valid(target) ||
		    !g_regex_match_simple("^[A-Za-z0-9_-]+$", server, 0, 0)) {
			if (warnings != NULL)
				g_ptr_array_add(warnings, g_strdup_printf(
					"agent '%s' is not given the broker: '%s' is not a "
					"gateway server id (letters, digits, '-' and '_')",
					clawt_agent_config_get_id(agent), server));
			continue;
		}

		refusal = clawt_broker_agent_refusal(config, agent);

		if (refusal != NULL) {
			if (warnings != NULL)
				g_ptr_array_add(warnings, g_strdup_printf(
					"agent '%s' is not given the broker: %s",
					clawt_agent_config_get_id(agent), refusal));
			continue;
		}

		ids = g_hash_table_lookup(members, target);

		if (ids == NULL) {
			ids = g_ptr_array_new_with_free_func(g_free);
			g_hash_table_insert(members, g_strdup(target), ids);
			g_hash_table_insert(servers, g_strdup(target),
			                    g_strdup(server));
			g_ptr_array_add(order, g_strdup(target));
		}

		g_ptr_array_add(ids, g_strdup(clawt_agent_config_get_id(agent)));
	}

	/* Agents' own servers first, then teams', each in file order. */
	{
		guint pass;

		for (pass = 0; pass < 2; pass++) {
			for (i = 0; i < order->len; i++) {
				const gchar *target = g_ptr_array_index(order, i);
				gboolean team = g_str_has_prefix(target, "team:");
				GPtrArray *ids;
				ClawtBrokerAssignment *assignment;

				if ((pass == 0) == team)
					continue;

				ids = g_hash_table_lookup(members, target);
				assignment = g_new0(ClawtBrokerAssignment, 1);
				assignment->target = g_strdup(target);
				assignment->server = g_strdup(
					g_hash_table_lookup(servers, target));
				assignment->connector = clawt_broker_connector_name(target);
				assignment->key_file = clawt_broker_key_path(state_dir,
				                                             target);

				g_ptr_array_add(ids, NULL);
				assignment->agents = g_strdupv((GStrv)ids->pdata);
				g_ptr_array_remove_index(ids, ids->len - 1);

				g_ptr_array_add(out, assignment);
			}
		}
	}

	return out;
}

ClawtBrokerAssignment *
clawt_broker_assignment_for_agent(
	ClawtConfig *config,
	const gchar *agent_id
){
	g_autoptr(GPtrArray) all = NULL;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	if (agent_id == NULL)
		return NULL;

	all = clawt_broker_assignments(config, NULL);

	for (i = 0; i < all->len; i++) {
		ClawtBrokerAssignment *assignment = g_ptr_array_index(all, i);

		if (g_strv_contains((const gchar * const *)assignment->agents,
		                    agent_id))
			return g_ptr_array_steal_index(all, i);
	}

	return NULL;
}

GPtrArray *
clawt_broker_connector_nodes(
	ClawtConfig *config,
	GPtrArray   *warnings
){
	g_autoptr(GPtrArray) assignments = NULL;
	GPtrArray *out;
	const gchar *provider;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	if (!clawt_config_get_boolean(config, "broker.enabled"))
		return NULL;

	assignments = clawt_broker_assignments(config, warnings);
	provider = clawt_enum_to_nick(CLAWT_TYPE_BROKER_TYPE,
	                              (gint)clawt_broker_get_type_value(config));
	out = g_ptr_array_new_with_free_func((GDestroyNotify)yaml_node_unref);

	for (i = 0; i < assignments->len; i++) {
		ClawtBrokerAssignment *assignment = g_ptr_array_index(assignments, i);
		g_autofree gchar *url = clawt_broker_mcp_url(config,
		                                             assignment->server);
		YamlNode *node = yaml_node_new_mapping(NULL);
		YamlMapping *mapping = yaml_node_get_mapping(node);
		g_autoptr(YamlNode) agents = yaml_node_new_sequence(NULL);
		guint a;

		yaml_mapping_set_string_member(mapping, "name", assignment->connector);
		yaml_mapping_set_string_member(mapping, "type", "connector");
		yaml_mapping_set_string_member(mapping, "provider", provider);
		yaml_mapping_set_string_member(mapping, "account",
		                               assignment->target);
		yaml_mapping_set_string_member(mapping, "url", url);
		yaml_mapping_set_string_member(mapping, "token_file",
		                               assignment->key_file);
		yaml_mapping_set_string_member(mapping, "scope", "selected");

		for (a = 0; assignment->agents[a] != NULL; a++)
			yaml_sequence_add_string_element(yaml_node_get_sequence(agents),
			                                 assignment->agents[a]);

		yaml_mapping_set_member(mapping, "agents", agents);
		g_ptr_array_add(out, node);
	}

	return out;
}

gboolean
clawt_broker_check_confinement(
	ClawtConfig  *config,
	GError      **error
){
	g_autoptr(GString) names = NULL;
	GPtrArray *agents;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), FALSE);

	if (clawt_broker_get_type_value(config) != CLAWT_BROKER_JENTIC)
		return TRUE;

	names = g_string_new(NULL);
	agents = clawt_config_get_agents(config);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
		g_autofree gchar *target = NULL;
		g_autofree gchar *server = server_for_agent(config, agent, &target);

		if (server == NULL)
			continue;

		if (clawt_agent_config_get_enum(agent, "runtime.confine") ==
		    CLAWT_RUNTIME_CONFINE_BWRAP)
			continue;

		if (names->len > 0)
			g_string_append(names, ", ");

		g_string_append(names, clawt_agent_config_get_id(agent));
	}

	if (names->len == 0)
		return TRUE;

	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
	            "Jentic One is not started while an agent that uses it is "
	            "unconfined (%s): its own documentation says an agent running "
	            "as the same OS user can read its credential store. Set "
	            "runtime.confine: bwrap on %s, or take their broker_server "
	            "away", names->str, names->str);

	return FALSE;
}

gboolean
clawt_broker_check_setting(
	ClawtConfig  *config,
	const gchar  *agent_id,
	const gchar  *team_id,
	const gchar  *key,
	const gchar  *value,
	GError      **error
){
	static const gchar *reason =
		"Jentic One is only given to agents with runtime.confine: bwrap -- "
		"its own documentation says an agent running as the same OS user "
		"can read its credential store";
	gboolean setting_server;
	GPtrArray *agents;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), FALSE);

	if (key == NULL || !clawt_config_get_boolean(config, "broker.enabled") ||
	    clawt_broker_get_type_value(config) != CLAWT_BROKER_JENTIC)
		return TRUE;

	setting_server = g_strcmp0(key, "broker_server") == 0 &&
	                 value != NULL && *value != '\0';

	if (agent_id != NULL) {
		ClawtAgentConfig *agent = clawt_config_get_agent(config, agent_id);
		gboolean confined;
		g_autofree gchar *target = NULL;
		g_autofree gchar *server = NULL;

		if (agent == NULL)
			return TRUE;

		confined = clawt_agent_config_get_enum(agent, "runtime.confine") ==
		           CLAWT_RUNTIME_CONFINE_BWRAP;
		server = server_for_agent(config, agent, &target);

		if (setting_server && !confined) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
			            "'%s' is not confined: %s. Set runtime.confine: "
			            "bwrap on it first", agent_id, reason);
			return FALSE;
		}

		if (g_strcmp0(key, "runtime.confine") == 0 &&
		    g_strcmp0(value, "bwrap") != 0 && server != NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
			            "'%s' reaches Jentic One (gateway server %s): %s. "
			            "Take its broker_server away first", agent_id,
			            server, reason);
			return FALSE;
		}

		return TRUE;
	}

	if (team_id == NULL || !setting_server)
		return TRUE;

	/* A team's server reaches every member without one of its own. */
	agents = clawt_config_get_agents(config);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
		const gchar *own = clawt_agent_config_get_string(agent,
		                                                 "broker_server");

		if (g_strcmp0(clawt_agent_config_get_string(agent, "team"),
		              team_id) != 0 || (own != NULL && *own != '\0'))
			continue;

		if (clawt_agent_config_get_enum(agent, "runtime.confine") !=
		    CLAWT_RUNTIME_CONFINE_BWRAP) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
			            "team '%s' has an unconfined member, '%s': %s",
			            team_id, clawt_agent_config_get_id(agent), reason);
			return FALSE;
		}
	}

	return TRUE;
}

/* ── Its state on disk ───────────────────────────────────────────── */

/*
 * A directory with exactly @mode, made if missing.  chmod as well as
 * mkdir, because a directory that already exists keeps whatever mode it
 * was made with, and the mode is the point.
 */
static gboolean
ensure_dir(
	const gchar  *path,
	gint          mode,
	GError      **error
){
	if (g_mkdir_with_parents(path, mode) != 0 || g_chmod(path, mode) != 0) {
		gint saved = errno;

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "cannot make %s: %s", path, g_strerror(saved));
		return FALSE;
	}

	return TRUE;
}

/* Hex for one secret, from the kernel. */
static gchar *
random_hex(
	gsize    chars,
	GError **error
){
	g_autofree gchar *token = clawt_generate_token(error);

	if (token == NULL)
		return NULL;

	return g_strndup(token, MIN(chars, strlen(token)));
}

/* 32 random bytes, base64, which is the key Jentic One's quickstart makes. */
static gchar *
random_base64(
	GError **error
){
	g_autofree gchar *token = clawt_generate_token(error);
	guchar bytes[32];
	gsize i;

	if (token == NULL || strlen(token) < sizeof(bytes) * 2)
		return NULL;

	for (i = 0; i < sizeof(bytes); i++) {
		gchar pair[3] = { token[i * 2], token[i * 2 + 1], '\0' };

		bytes[i] = (guchar)g_ascii_strtoull(pair, NULL, 16);
	}

	return g_base64_encode(bytes, sizeof(bytes));
}

/*
 * A secret file: kept if it is there, made from /dev/urandom if not, and
 * 0600 either way.  Never regenerated -- AnythingMCP's ENCRYPTION_KEY and
 * Jentic One's key are what decrypt the SaaS credentials somebody signed
 * in with, and a new one would make every one of them unreadable.
 */
static gboolean
ensure_secret(
	const gchar  *dir,
	const gchar  *name,
	gboolean      base64,
	gsize         hex_chars,
	GError      **error
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	g_autofree gchar *value = NULL;

	if (g_file_test(path, G_FILE_TEST_EXISTS)) {
		if (g_chmod(path, 0600) != 0) {
			gint saved = errno;

			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
			            "cannot make %s private: %s", path,
			            g_strerror(saved));
			return FALSE;
		}

		return TRUE;
	}

	value = base64 ? random_base64(error) : random_hex(hex_chars, error);

	if (value == NULL)
		return FALSE;

	return g_file_set_contents_full(path, value, -1,
	                                G_FILE_SET_CONTENTS_CONSISTENT, 0600,
	                                error);
}

gboolean
clawt_broker_prepare_state(
	ClawtBrokerType   type,
	const gchar      *state_dir,
	GError          **error
){
	g_autofree gchar *dir = NULL;
	g_autofree gchar *keys = NULL;

	g_return_val_if_fail(state_dir != NULL, FALSE);

	dir = clawt_broker_dir(state_dir);
	keys = g_build_filename(dir, "keys", NULL);

	/*
	 * 0700 at the top.  The directories under it are wider on purpose --
	 * the containers' own users are other uids in podman's user
	 * namespace and must write there -- and this mode is what keeps
	 * every other user on the machine out of all of it.
	 */
	if (!ensure_dir(dir, 0700, error) || !ensure_dir(keys, 0700, error))
		return FALSE;

	switch (type) {
	case CLAWT_BROKER_ANYTHINGMCP: {
		g_autofree gchar *db = g_build_filename(dir, "db", NULL);
		g_autofree gchar *run = g_build_filename(dir, "run", NULL);

		/*
		 * db is bound at PostgreSQL's data volume and holds PGDATA one
		 * level down, which the entrypoint makes and gives to its own
		 * user.  Bound at PGDATA itself, the entrypoint chowns the bind
		 * source, and this user can then no longer relabel it at the
		 * next start.  0755 because the entrypoint re-runs as the
		 * postgres user, which must pass through it.
		 */
		if (!ensure_dir(db, 0755, error))
			return FALSE;

		/* The socket directory: written by one uid, dialled by another. */
		if (!ensure_dir(run, 0777, error))
			return FALSE;

		/* AnythingMCP wants exactly 32 characters for ENCRYPTION_KEY. */
		if (!ensure_secret(dir, "encryption-key", FALSE, 32, error) ||
		    !ensure_secret(dir, "jwt-secret", FALSE, 64, error) ||
		    !ensure_secret(dir, "database-password", FALSE, 64, error))
			return FALSE;

		break;
	}

	case CLAWT_BROKER_JENTIC: {
		g_autofree gchar *data = g_build_filename(dir, "data", NULL);

		/* The image runs as uid 10001, which is not this user. */
		if (!ensure_dir(data, 0777, error))
			return FALSE;

		if (!ensure_secret(dir, "jentic-encryption-key", TRUE, 0, error))
			return FALSE;

		break;
	}
	}

	return TRUE;
}

static gchar *
read_secret(
	const gchar  *dir,
	const gchar  *name,
	GError      **error
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	g_autofree gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, error)) {
		g_prefix_error(error, "the broker's state is not prepared: ");
		return NULL;
	}

	return g_strdup(g_strstrip(text));
}

/* ── Its containers ──────────────────────────────────────────────── */

void
clawt_broker_container_spec_free(
	ClawtBrokerContainerSpec *self
){
	if (self == NULL)
		return;

	g_free(self->name);
	g_free(self->image);
	g_free(self->command);
	g_clear_pointer(&self->env, g_hash_table_unref);
	g_free(self->ports);
	g_clear_pointer(&self->mounts, g_ptr_array_unref);
	g_free(self);
}

gchar *
clawt_broker_container_spec_env_json(
	ClawtBrokerContainerSpec *self
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GList) names = NULL;
	GList *l;

	g_return_val_if_fail(self != NULL, NULL);

	names = g_list_sort(g_hash_table_get_keys(self->env),
	                    (GCompareFunc)g_strcmp0);

	json_builder_begin_object(builder);

	for (l = names; l != NULL; l = l->next) {
		json_builder_set_member_name(builder, l->data);
		json_builder_add_string_value(builder,
		                              g_hash_table_lookup(self->env, l->data));
	}

	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);

	return json_generator_to_data(generator, NULL);
}

static ClawtBrokerContainerSpec *
spec_new(
	const gchar *name,
	const gchar *image
){
	ClawtBrokerContainerSpec *spec = g_new0(ClawtBrokerContainerSpec, 1);

	spec->name = g_strdup(name);
	spec->image = g_strdup(image);
	spec->env = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	spec->mounts = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_mount_free);

	return spec;
}

static void
spec_env(
	ClawtBrokerContainerSpec *spec,
	const gchar              *name,
	const gchar              *value
){
	g_hash_table_replace(spec->env, g_strdup(name),
	                     g_strdup(value != NULL ? value : ""));
}

static void
spec_mount(
	ClawtBrokerContainerSpec *spec,
	const gchar              *source,
	const gchar              *target,
	ClawtRelabel              relabel
){
	ClawtMount *mount = clawt_mount_new(source, target);

	clawt_mount_set_mount_type(mount, CLAWT_MOUNT_BIND);
	clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
	clawt_mount_set_relabel(mount, relabel);
	g_ptr_array_add(spec->mounts, mount);
}

/* A JSON array of strings, for a command. */
static gchar *
json_argv(
	const gchar * const *argv
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	gsize i;

	json_builder_begin_array(builder);

	for (i = 0; argv[i] != NULL; i++)
		json_builder_add_string_value(builder, argv[i]);

	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);

	return json_generator_to_data(generator, NULL);
}

/*
 * libpod's port mappings.  The host address is the configured bind, so a
 * loopback gateway is not reachable from the LAN by accident of podman's
 * default, which is every address.
 */
static gchar *
json_ports(
	const gchar *bind,
	gint64       host_port,
	gint64       container_port,
	gint64       second_host_port,
	gint64       second_container_port
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	const gchar *host_ip = (bind != NULL && *bind != '\0') ? bind
	                                                     : "127.0.0.1";
	guint i;

	json_builder_begin_array(builder);

	for (i = 0; i < 2; i++) {
		gint64 host = (i == 0) ? host_port : second_host_port;
		gint64 inside = (i == 0) ? container_port : second_container_port;

		if (host <= 0)
			continue;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "host_ip");
		json_builder_add_string_value(builder, host_ip);
		json_builder_set_member_name(builder, "host_port");
		json_builder_add_int_value(builder, host);
		json_builder_set_member_name(builder, "container_port");
		json_builder_add_int_value(builder, inside);
		json_builder_set_member_name(builder, "protocol");
		json_builder_add_string_value(builder, "tcp");
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);

	return json_generator_to_data(generator, NULL);
}

static gboolean
check_port(
	ClawtConfig  *config,
	const gchar  *key,
	GError      **error
){
	gint64 port = clawt_config_get_int(config, key);

	if (port >= 1 && port <= 65535)
		return TRUE;

	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
	            "%s is %" G_GINT64_FORMAT "; a port is 1 to 65535", key, port);
	return FALSE;
}

static gboolean
anythingmcp_spec(
	ClawtConfig  *config,
	const gchar  *dir,
	GPtrArray    *out,
	GError      **error
){
	g_autofree gchar *image = clawt_broker_get_image(config);
	g_autofree gchar *db_image = database_image(config);
	g_autofree gchar *password = NULL;
	g_autofree gchar *jwt = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *db_dir = g_build_filename(dir, "db", NULL);
	g_autofree gchar *run_dir = g_build_filename(dir, "run", NULL);
	g_autofree gchar *api = NULL;
	g_autofree gchar *dashboard = NULL;
	g_autofree gchar *database_url = NULL;
	const gchar *bind = clawt_config_get_string(config, "broker.bind");
	ClawtBrokerContainerSpec *db;
	ClawtBrokerContainerSpec *app;
	/*
	 * The image's own command, named: a container computer runs `sleep
	 * infinity` unless told otherwise, because an agent's machine exists
	 * to be exec'd into.  Left to the default, the gateway's container
	 * came up "running" and served nothing.
	 */
	static const gchar * const app_command[] = { "./start.sh", NULL };
	static const gchar * const db_command[] = {
		"postgres",
		"-c", "listen_addresses=",
		"-c", "unix_socket_directories=/var/run/postgresql," BROKER_SOCKET_DIR,
		NULL
	};

	if (!clawt_broker_check_image(db_image, error) ||
	    !check_port(config, "broker.dashboard_port", error))
		return FALSE;

	password = read_secret(dir, "database-password", error);
	jwt = password != NULL ? read_secret(dir, "jwt-secret", error) : NULL;
	key = jwt != NULL ? read_secret(dir, "encryption-key", error) : NULL;

	if (key == NULL)
		return FALSE;

	/*
	 * The database: no published port and no TCP listener at all.  Its
	 * own entrypoint's temporary server and psql use the image's socket
	 * directory; the gateway uses the second one, which is the bind the
	 * two containers share.
	 */
	db = spec_new("clawt-broker-anythingmcp-db", db_image);
	db->command = json_argv(db_command);
	spec_env(db, "POSTGRES_USER", "amcp");
	spec_env(db, "POSTGRES_PASSWORD", password);
	spec_env(db, "POSTGRES_DB", "anythingmcp");
	spec_env(db, "PGDATA", "/var/lib/postgresql/data/pgdata");
	spec_mount(db, db_dir, "/var/lib/postgresql/data", CLAWT_RELABEL_PRIVATE);
	spec_mount(db, run_dir, BROKER_SOCKET_DIR, CLAWT_RELABEL_SHARED);
	g_ptr_array_add(out, db);

	api = base_url(config, "broker.port");
	dashboard = clawt_broker_dashboard_url(config);
	database_url = g_strdup_printf(
		"postgresql://amcp:%s@localhost/anythingmcp?host=" BROKER_SOCKET_DIR,
		password);

	app = spec_new("clawt-broker-anythingmcp", image);
	app->command = json_argv(app_command);
	spec_env(app, "NODE_ENV", "production");
	spec_env(app, "PORT", "4000");
	spec_env(app, "DATABASE_URL", database_url);
	spec_env(app, "JWT_SECRET", jwt);
	spec_env(app, "ENCRYPTION_KEY", key);
	spec_env(app, "NEXT_PUBLIC_API_URL", api);
	spec_env(app, "SERVER_URL", api);
	spec_env(app, "CORS_ORIGIN", dashboard);
	spec_env(app, "FRONTEND_URL", dashboard);

	/*
	 * Per-user keys, each made for one gateway server, and OAuth for a
	 * person's own clients -- and no instance-wide token.  Its static
	 * MCP_BEARER_TOKEN reaches every server of every workspace, which is
	 * exactly the credential a per-agent server exists not to need.
	 */
	spec_env(app, "MCP_AUTH_MODE", "both");
	spec_env(app, "MCP_ALLOW_ANONYMOUS", "false");
	spec_env(app, "ALLOW_OPEN_REGISTRATION", "false");
	app->ports = json_ports(bind, clawt_config_get_int(config, "broker.port"),
	                        4000,
	                        clawt_config_get_int(config,
	                                             "broker.dashboard_port"),
	                        3000);
	spec_mount(app, run_dir, BROKER_SOCKET_DIR, CLAWT_RELABEL_SHARED);
	g_ptr_array_add(out, app);

	return TRUE;
}

static gboolean
jentic_spec(
	ClawtConfig  *config,
	const gchar  *dir,
	GPtrArray    *out,
	GError      **error
){
	g_autofree gchar *image = clawt_broker_get_image(config);
	g_autofree gchar *key = read_secret(dir, "jentic-encryption-key", error);
	g_autofree gchar *data = g_build_filename(dir, "data", NULL);
	g_autofree gchar *url = NULL;
	ClawtBrokerContainerSpec *app;
	/*
	 * The control plane and the broker as two processes of one
	 * container, so the control plane reaches the broker on its own
	 * loopback as Jentic's compose file arranges with a shared network.
	 * Either one ending ends the container, and the restart policy
	 * starts both again.  The trap is because PID 1 ignores a SIGTERM it
	 * has no handler for, and a stop would otherwise wait for the kill.
	 */
	static const gchar * const command[] = {
		"bash", "-c",
		"trap 'kill -TERM $(jobs -p) 2>/dev/null; wait; exit 0' TERM INT; "
		"python -m jentic_one.migrations.run || exit 1; "
		"JENTIC__APPS=broker JENTIC__SERVER__PORT=8100 "
		"python -m jentic_one & "
		"python -m jentic_one & "
		"wait -n; exit 1",
		NULL
	};

	if (key == NULL)
		return FALSE;

	url = clawt_broker_dashboard_url(config);

	app = spec_new("clawt-broker-jentic", image);
	app->command = json_argv(command);
	spec_env(app, "JENTIC__AUTH__CANONICAL_BASE_URL", url);
	spec_env(app, "JENTIC__SERVER__MCP__ENABLED", "true");
	spec_env(app, "JENTIC__DATABASES__REGISTRY__BACKEND", "sqlite");
	spec_env(app, "JENTIC__DATABASES__REGISTRY__PATH", "/data/registry.db");
	spec_env(app, "JENTIC__DATABASES__CONTROL__BACKEND", "sqlite");
	spec_env(app, "JENTIC__DATABASES__CONTROL__PATH", "/data/control.db");
	spec_env(app, "JENTIC__DATABASES__ADMIN__BACKEND", "sqlite");
	spec_env(app, "JENTIC__DATABASES__ADMIN__PATH", "/data/admin.db");
	spec_env(app, "JENTIC__CREDENTIALS__ENCRYPTION__ENTRIES__0__ID", "v1");
	spec_env(app, "JENTIC__CREDENTIALS__ENCRYPTION__ENTRIES__0__MATERIAL_ENV",
	         "JENTIC_ENCRYPTION_KEY");
	spec_env(app, "JENTIC_ENCRYPTION_KEY", key);

	/*
	 * Its documented opt-out, always: the quickstart turns telemetry on.
	 * The release check and the OTLP exporters reach out too -- GitHub,
	 * and a collector on localhost:4317 that is not there, which fills
	 * the log with retries -- so they go with it.
	 */
	spec_env(app, "JENTIC__TELEMETRY__ENABLED", "false");
	spec_env(app, "JENTIC__RELEASE_CHECK__ENABLED", "false");
	spec_env(app, "JENTIC__OBSERVABILITY__METRICS__EXPORTER", "none");
	spec_env(app, "JENTIC__OBSERVABILITY__TRACING__EXPORTER", "none");
	app->ports = json_ports(clawt_config_get_string(config, "broker.bind"),
	                        clawt_config_get_int(config, "broker.port"), 8000,
	                        0, 0);
	spec_mount(app, data, "/data", CLAWT_RELABEL_PRIVATE);
	g_ptr_array_add(out, app);

	return TRUE;
}

GPtrArray *
clawt_broker_build_spec(
	ClawtConfig  *config,
	const gchar  *state_dir,
	GError      **error
){
	g_autoptr(GPtrArray) out = NULL;
	g_autofree gchar *image = NULL;
	g_autofree gchar *dir = NULL;
	gboolean built = FALSE;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
	g_return_val_if_fail(state_dir != NULL, NULL);

	image = clawt_broker_get_image(config);

	if (!clawt_broker_check_image(image, error) ||
	    !check_port(config, "broker.port", error))
		return NULL;

	dir = clawt_broker_dir(state_dir);
	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_broker_container_spec_free);

	switch (clawt_broker_get_type_value(config)) {
	case CLAWT_BROKER_ANYTHINGMCP:
		built = anythingmcp_spec(config, dir, out, error);
		break;
	case CLAWT_BROKER_JENTIC:
		built = jentic_spec(config, dir, out, error);
		break;
	}

	if (!built)
		return NULL;

	return g_steal_pointer(&out);
}

/* ── What the relay says ─────────────────────────────────────────── */

static JsonObject *
parse_object(
	JsonParser  *parser,
	const gchar *text
){
	JsonNode *root;

	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	return json_node_get_object(root);
}

static gchar *
render_object(
	JsonObject *object
){
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonGenerator) generator = json_generator_new();

	json_node_set_object(node, object);
	json_generator_set_root(generator, node);

	return json_generator_to_data(generator, NULL);
}

/*
 * One line of somebody else's words, bounded.  A gateway's reason is
 * worth repeating -- "Tool nope not found" is the answer -- but it is the
 * gateway's text in an agent's transcript, so it is cut short and kept
 * to one line rather than relayed whole.
 */
static gchar *
one_line(
	const gchar *text
){
	g_autoptr(GString) out = NULL;
	const gchar *p;

	if (text == NULL)
		return NULL;

	out = g_string_new(NULL);

	for (p = text; *p != '\0' && out->len < BROKER_DETAIL_CHARS;
	     p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char_validated(p, -1);

		if (c == (gunichar)-1 || c == (gunichar)-2)
			break;

		if (g_unichar_iscntrl(c))
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, c);
	}

	if (*p != '\0')
		g_string_append(out, "...");

	g_strstrip(out->str);

	if (out->str[0] == '\0')
		return NULL;

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gchar *
clawt_broker_reason_from_body(
	const gchar *body
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *json = NULL;
	JsonObject *object;
	JsonNode *member;

	if (body == NULL)
		return NULL;

	/* A streamed reply: the first event's data is the message. */
	if (g_str_has_prefix(body, "event:") || g_str_has_prefix(body, "data:")) {
		const gchar *data = strstr(body, "data:");
		const gchar *end;

		if (data == NULL)
			return NULL;

		data += 5;
		end = strchr(data, '\n');
		json = (end != NULL) ? g_strndup(data, (gsize)(end - data))
		                     : g_strdup(data);
	} else {
		json = g_strdup(body);
	}

	object = parse_object(parser, g_strstrip(json));

	if (object == NULL)
		return NULL;

	if (json_object_has_member(object, "error") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "error"))) {
		JsonObject *error = json_object_get_object_member(object, "error");

		return one_line(json_object_get_string_member_with_default(
			error, "message", NULL));
	}

	member = json_object_get_member(object, "detail");

	if (member == NULL)
		member = json_object_get_member(object, "message");

	if (member != NULL && JSON_NODE_HOLDS_VALUE(member) &&
	    json_node_get_value_type(member) == G_TYPE_STRING)
		return one_line(json_node_get_string(member));

	return NULL;
}

gchar *
clawt_broker_refusal_reply(
	const gchar *line,
	const gchar *gateway,
	const gchar *connector,
	guint        status,
	const gchar *detail
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonObject) reply = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *reason = one_line(detail);
	g_autofree gchar *target = clawt_broker_target_for_connector(connector);
	g_autofree gchar *said = NULL;
	JsonObject *request;
	const gchar *method = NULL;

	g_return_val_if_fail(gateway != NULL, NULL);

	request = parse_object(parser, line);

	if (request == NULL || !json_object_has_member(request, "id"))
		return NULL;

	if (json_object_has_member(request, "method"))
		method = json_object_get_string_member_with_default(request, "method",
		                                                    NULL);

	said = (reason != NULL) ? g_strdup_printf(": %s", reason)
	                        : g_strdup("");

	/*
	 * Each starts with the gateway's name, because the fix is different
	 * for each layer: clawtilla's own refusals (a narrowed tool list, an
	 * action rule) say clawtilla, and nothing here does.
	 */
	if (status == 0)
		text = g_strdup_printf(
			"%s, the integration broker clawtilla runs, did not answer%s, "
			"so nothing ran. It may still be starting; `clawtilla broker "
			"status` says. Tell the person rather than retrying.",
			gateway, said);
	else if (status == 401)
		text = g_strdup_printf(
			"%s, the integration broker, refused the key clawtilla holds "
			"for this connector (HTTP 401)%s, so nothing ran. Do not retry. "
			"Ask the person to make a key for this agent's gateway server "
			"in %s's dashboard and store it with `clawtilla broker key "
			"%s`.", gateway, said, gateway,
			target != NULL ? target : "<agent>");
	else if (status == 403)
		text = g_strdup_printf(
			"%s, the integration broker, refused this (HTTP 403)%s, so "
			"nothing ran. What this agent may use is set on its gateway "
			"server in %s, not by clawtilla; ask the person if you need "
			"it.", gateway, said, gateway);
	else
		text = g_strdup_printf(
			"%s, the integration broker, failed this request (HTTP %u)%s, "
			"so nothing ran.", gateway, status, said);

	reply = json_object_new();
	json_object_set_string_member(reply, "jsonrpc", "2.0");
	json_object_set_member(reply, "id",
	                       json_node_copy(json_object_get_member(request,
	                                                             "id")));

	if (g_strcmp0(method, "tools/call") == 0) {
		JsonObject *result = json_object_new();
		JsonArray *content = json_array_new();
		JsonObject *item = json_object_new();

		json_object_set_string_member(item, "type", "text");
		json_object_set_string_member(item, "text", text);
		json_array_add_object_element(content, item);
		json_object_set_array_member(result, "content", content);
		json_object_set_boolean_member(result, "isError", TRUE);
		json_object_set_object_member(reply, "result", result);
	} else {
		JsonObject *error = json_object_new();

		/* -32001: a server-defined error, which is what a refusal is. */
		json_object_set_int_member(error, "code", -32001);
		json_object_set_string_member(error, "message", text);
		json_object_set_object_member(reply, "error", error);
	}

	return render_object(reply);
}

gchar *
clawt_broker_annotate_reply(
	const gchar *line,
	const gchar *gateway
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *prefix = NULL;
	JsonObject *message;

	g_return_val_if_fail(gateway != NULL, NULL);

	message = parse_object(parser, line);

	if (message == NULL)
		return NULL;

	prefix = g_strdup_printf("%s answered: ", gateway);

	if (json_object_has_member(message, "error") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(message, "error"))) {
		JsonObject *error = json_object_get_object_member(message, "error");
		const gchar *text = json_object_get_string_member_with_default(
			error, "message", NULL);
		g_autofree gchar *named = NULL;

		if (error == NULL || text == NULL || g_str_has_prefix(text, prefix))
			return NULL;

		named = g_strconcat(prefix, text, NULL);
		json_object_set_string_member(error, "message", named);

		return render_object(message);
	}

	if (json_object_has_member(message, "result") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(message, "result"))) {
		JsonObject *result = json_object_get_object_member(message, "result");
		JsonArray *content;
		JsonObject *first;
		const gchar *text;
		g_autofree gchar *named = NULL;

		if (result == NULL ||
		    !json_object_get_boolean_member_with_default(result, "isError",
		                                                 FALSE) ||
		    !json_object_has_member(result, "content") ||
		    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "content")))
			return NULL;

		content = json_object_get_array_member(result, "content");

		if (content == NULL || json_array_get_length(content) == 0)
			return NULL;

		first = json_array_get_object_element(content, 0);

		if (first == NULL)
			return NULL;

		text = json_object_get_string_member_with_default(first, "text", NULL);

		if (text == NULL || g_str_has_prefix(text, prefix))
			return NULL;

		named = g_strconcat(prefix, text, NULL);
		json_object_set_string_member(first, "text", named);

		return render_object(message);
	}

	return NULL;
}

/* ── The running broker ──────────────────────────────────────────── */

enum {
	SIGNAL_CHANGED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtBroker {
	GObject           parent_instance;

	ClawtPodBridge   *bridge;
	GMainContext     *context;
	ClawtConfig      *config;
	gchar            *state_dir;

	ClawtBrokerState  state;
	gchar            *detail;

	/* ClawtComputer*, in start order; NULL until a start or stop builds them. */
	GPtrArray        *computers;
	gchar            *connection;
	gchar            *health_url;
	ClawtRestartPolicy restart;

	/*
	 * Bumped by every start and stop, and carried by everything that
	 * answers later: a container step or a probe that finishes after the
	 * person pressed Stop is about a broker that no longer exists.
	 */
	guint             generation;
	gboolean          start_after_stop;

	/*
	 * Whether any container of ours was started since the last stop.  A
	 * start that failed on its first container left nothing running, and
	 * the daemon's shutdown has nothing to stop -- asking podman anyway
	 * reports a failure about a container that never existed.
	 */
	gboolean          up;

	GSource          *watch;
	guint             watch_interval;
	gboolean          probing;
	gint64            starting_since;
	guint             unhealthy;
	guint             restarts;
	GArray           *restart_times;   /* gint64, monotonic seconds */

	/*
	 * The last container the watch found down, and whether it was
	 * started again: broker.status's last_exit.  Kept across starts, so
	 * the reason survives the detail being replaced.
	 */
	ClawtBrokerExit  *last_exit;
	gboolean          last_exit_restarted;
};

G_DEFINE_FINAL_TYPE(ClawtBroker, clawt_broker, G_TYPE_OBJECT)

static void broker_begin_start(ClawtBroker *self);

static void
set_state(
	ClawtBroker      *self,
	ClawtBrokerState  state,
	const gchar      *detail
){
	if (self->state == state && g_strcmp0(self->detail, detail) == 0)
		return;

	self->state = state;
	g_free(self->detail);
	self->detail = g_strdup(detail);

	g_message("broker: %s%s%s",
	          clawt_enum_to_nick(CLAWT_TYPE_BROKER_STATE, (gint)state),
	          detail != NULL ? ": " : "", detail != NULL ? detail : "");

	g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static void
stop_watch(
	ClawtBroker *self
){
	if (self->watch == NULL)
		return;

	g_source_destroy(self->watch);
	g_clear_pointer(&self->watch, g_source_unref);
}

static gboolean on_watch_tick(gpointer user_data);

/*
 * Attached to the broker's own context by name.  A timer on the
 * thread-default is a timer on whichever loop happened to be pushed when
 * the last step answered, and the global default is a loop an embedded
 * daemon never runs.
 */
static void
start_watch(
	ClawtBroker *self,
	guint        seconds
){
	if (self->watch != NULL && self->watch_interval == seconds)
		return;

	stop_watch(self);
	self->watch_interval = seconds;
	self->watch = g_timeout_source_new_seconds(seconds);
	g_source_set_callback(self->watch, on_watch_tick, self, NULL);
	g_source_attach(self->watch, self->context);
}

/*
 * The containers, made fresh from the config.  A new object per start is
 * what makes a start take a changed image or port: a computer whose state
 * is absent provisions, and podomation's create replaces a container of
 * the same name that is already there.  Everything they hold is a bind
 * under <state_dir>/broker, so replacing them loses nothing.
 */
static gboolean
build_computers(
	ClawtBroker  *self,
	GError      **error
){
	g_autoptr(GPtrArray) spec = NULL;
	ClawtBrokerType type;
	const gchar *connection;
	guint i;

	if (self->config == NULL || self->state_dir == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		                    "the broker has no configuration yet");
		return FALSE;
	}

	type = clawt_broker_get_type_value(self->config);

	if (!clawt_broker_check_confinement(self->config, error))
		return FALSE;

	if (!clawt_broker_prepare_state(type, self->state_dir, error))
		return FALSE;

	spec = clawt_broker_build_spec(self->config, self->state_dir, error);

	if (spec == NULL)
		return FALSE;

	g_clear_pointer(&self->computers, g_ptr_array_unref);
	self->computers = g_ptr_array_new_with_free_func(g_object_unref);
	connection = clawt_config_get_string(self->config, "broker.connection");

	for (i = 0; i < spec->len; i++) {
		ClawtBrokerContainerSpec *one = g_ptr_array_index(spec, i);
		ClawtComputer *computer = clawt_container_computer_new(
			NULL, self->bridge, one->image);
		ClawtContainerComputer *container =
			CLAWT_CONTAINER_COMPUTER(computer);
		g_autofree gchar *env = clawt_broker_container_spec_env_json(one);
		guint m;

		clawt_container_computer_set_name(container, one->name);
		clawt_container_computer_set_connection(container, connection);
		clawt_container_computer_set_keep(container, TRUE);
		clawt_container_computer_set_env(container, env);
		clawt_container_computer_set_ports(container, one->ports);

		if (one->command != NULL)
			clawt_container_computer_set_command(container, one->command);

		for (m = 0; m < one->mounts->len; m++)
			clawt_computer_add_mount(computer,
			                         g_ptr_array_index(one->mounts, m));

		g_ptr_array_add(self->computers, computer);
	}

	g_free(self->connection);
	self->connection = g_strdup(clawt_container_computer_get_connection(
		CLAWT_CONTAINER_COMPUTER(g_ptr_array_index(self->computers, 0))));
	g_free(self->health_url);
	self->health_url = clawt_broker_health_url(self->config);
	self->restart = (ClawtRestartPolicy)clawt_config_get_enum(
		self->config, "broker.restart");

	return TRUE;
}

/* ── The steps, one container at a time ── */

typedef struct {
	ClawtBroker            *self;
	guint                   generation;
	guint                   index;
	ClawtComputerLifecycle  op;
} Step;

static void
step_free(
	Step *step
){
	g_clear_object(&step->self);
	g_free(step);
}

static void run_step(Step *step);

static ClawtComputer *
step_computer(
	Step *step
){
	ClawtBroker *self = step->self;
	guint n = self->computers->len;

	/* Started in order, the database first; stopped the other way round. */
	if (step->op == CLAWT_COMPUTER_LIFECYCLE_STOP)
		return g_ptr_array_index(self->computers, n - 1 - step->index);

	return g_ptr_array_index(self->computers, step->index);
}

static void
on_step_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Step *step = user_data;
	ClawtBroker *self = step->self;
	ClawtComputer *computer = CLAWT_COMPUTER(source);
	g_autoptr(GError) error = NULL;
	gboolean ok = clawt_computer_lifecycle_finish(computer, result, &error);

	if (step->generation != self->generation) {
		step_free(step);
		return;
	}

	if (ok && step->op == CLAWT_COMPUTER_LIFECYCLE_START)
		self->up = TRUE;

	if (!ok && step->op == CLAWT_COMPUTER_LIFECYCLE_START) {
		g_autofree gchar *detail = g_strdup_printf(
			"%s did not start: %s",
			clawt_container_computer_get_name(
				CLAWT_CONTAINER_COMPUTER(computer)),
			error != NULL ? error->message : "no reason given");

		set_state(self, CLAWT_BROKER_STATE_ERROR, detail);
		step_free(step);
		return;
	}

	/*
	 * A container that would not stop is said, and the rest are still
	 * stopped: leaving the database up because the gateway was already
	 * gone is not what somebody pressing Stop asked for.
	 */
	if (!ok)
		g_warning("broker: %s did not stop: %s",
		          clawt_container_computer_get_name(
		              CLAWT_CONTAINER_COMPUTER(computer)),
		          error != NULL ? error->message : "no reason given");

	step->index++;

	if (step->index < self->computers->len) {
		run_step(step);
		return;
	}

	if (step->op == CLAWT_COMPUTER_LIFECYCLE_START) {
		g_autofree gchar *detail = g_strdup_printf(
			"waiting for %s to answer at %s",
			clawt_broker_type_label(
				clawt_broker_get_type_value(self->config)),
			self->health_url);

		self->unhealthy = 0;
		set_state(self, CLAWT_BROKER_STATE_STARTING, detail);
		start_watch(self, BROKER_WATCH_STARTING_SECONDS);
		step_free(step);
		return;
	}

	self->up = FALSE;
	set_state(self,
	          clawt_config_get_boolean(self->config, "broker.enabled")
	          ? CLAWT_BROKER_STATE_STOPPED : CLAWT_BROKER_STATE_DISABLED,
	          ok ? NULL : (error != NULL ? error->message : NULL));
	step_free(step);

	if (self->start_after_stop) {
		self->start_after_stop = FALSE;
		broker_begin_start(self);
	}
}

static void
run_step(
	Step *step
){
	ClawtComputer *computer = step_computer(step);

	if (step->op == CLAWT_COMPUTER_LIFECYCLE_START) {
		g_autofree gchar *detail = g_strdup_printf(
			"starting %s (pulling %s first if it is not here yet)",
			clawt_container_computer_get_name(
				CLAWT_CONTAINER_COMPUTER(computer)),
			clawt_computer_get_state(computer) ==
			CLAWT_COMPUTER_STATE_ABSENT
			? "its image" : "nothing");

		set_state(step->self, CLAWT_BROKER_STATE_STARTING, detail);
	}

	clawt_computer_lifecycle_async(computer, step->op, step->self->context,
	                               NULL, on_step_done, step);
}

static void
begin_chain(
	ClawtBroker            *self,
	ClawtComputerLifecycle  op
){
	Step *step;

	if (self->computers == NULL || self->computers->len == 0)
		return;

	step = g_new0(Step, 1);
	step->self = g_object_ref(self);
	step->generation = self->generation;
	step->op = op;
	run_step(step);
}

static void
broker_begin_start(
	ClawtBroker *self
){
	g_autoptr(GError) error = NULL;

	self->generation++;
	stop_watch(self);
	self->starting_since = g_get_monotonic_time() / G_USEC_PER_SEC;

	if (!build_computers(self, &error)) {
		set_state(self, CLAWT_BROKER_STATE_ERROR, error->message);
		return;
	}

	set_state(self, CLAWT_BROKER_STATE_STARTING, "preparing");
	begin_chain(self, CLAWT_COMPUTER_LIFECYCLE_START);
}

void
clawt_broker_start(
	ClawtBroker *self
){
	g_return_if_fail(CLAWT_IS_BROKER(self));

	if (self->state == CLAWT_BROKER_STATE_STARTING ||
	    self->state == CLAWT_BROKER_STATE_RUNNING)
		return;

	/* A start pressed while it stops waits for the stop. */
	if (self->state == CLAWT_BROKER_STATE_STOPPING) {
		self->start_after_stop = TRUE;
		return;
	}

	broker_begin_start(self);
}

void
clawt_broker_stop(
	ClawtBroker *self
){
	g_return_if_fail(CLAWT_IS_BROKER(self));

	self->generation++;
	self->start_after_stop = FALSE;
	stop_watch(self);

	/*
	 * Nothing this daemon started is nothing to stop.  Building the
	 * containers just to stop them would make <state_dir>/broker and its
	 * secrets for a broker nobody turned on, and ask podman about it.  A
	 * gateway an earlier daemon left behind is replaced, by name, at the
	 * next start.
	 */
	if (self->computers == NULL) {
		set_state(self,
		          (self->config != NULL &&
		           clawt_config_get_boolean(self->config, "broker.enabled"))
		          ? CLAWT_BROKER_STATE_STOPPED
		          : CLAWT_BROKER_STATE_DISABLED, NULL);
		return;
	}

	set_state(self, CLAWT_BROKER_STATE_STOPPING, NULL);
	begin_chain(self, CLAWT_COMPUTER_LIFECYCLE_STOP);
}

void
clawt_broker_restart(
	ClawtBroker *self
){
	g_return_if_fail(CLAWT_IS_BROKER(self));

	clawt_broker_stop(self);

	if (self->state == CLAWT_BROKER_STATE_STOPPING)
		self->start_after_stop = TRUE;
	else
		broker_begin_start(self);
}

void
clawt_broker_stop_sync(
	ClawtBroker *self
){
	guint i;

	g_return_if_fail(CLAWT_IS_BROKER(self));

	self->generation++;
	self->start_after_stop = FALSE;
	stop_watch(self);

	if (self->computers == NULL || !self->up)
		return;

	for (i = self->computers->len; i > 0; i--) {
		ClawtComputer *computer = g_ptr_array_index(self->computers, i - 1);
		g_autoptr(GError) error = NULL;

		if (!clawt_computer_stop(computer, &error))
			g_warning("broker: %s did not stop: %s",
			          clawt_container_computer_get_name(
			              CLAWT_CONTAINER_COMPUTER(computer)),
			          error != NULL ? error->message : "no reason given");
	}

	self->up = FALSE;
	set_state(self, CLAWT_BROKER_STATE_STOPPED, NULL);
}

/* ── The watch ── */

typedef struct {
	ClawtPodBridge *bridge;
	gchar          *connection;
	gchar          *health_url;
	GStrv           ids;     /* each container's podman id, in order */
	GStrv           names;

	/* Answers. */
	gboolean        answered;
	gchar          *answer_error;
	ClawtBrokerExit *missing;
	gboolean        healthy;
} Probe;

static void
probe_free(
	Probe *probe
){
	g_clear_object(&probe->bridge);
	g_free(probe->connection);
	g_free(probe->health_url);
	g_strfreev(probe->ids);
	g_strfreev(probe->names);
	g_free(probe->answer_error);
	clawt_broker_exit_free(probe->missing);
	g_free(probe);
}

/*
 * A GET of the health URL from a worker thread.  The session is made
 * here, under a context of its own pushed for the duration: libsoup binds
 * a session to the context that was thread-default at its creation, and
 * a worker has none, so without this it would reach for the global
 * default -- the loop a GTK client is running.
 */
static gboolean
probe_health(
	const gchar *url
){
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) body = NULL;
	gboolean healthy = FALSE;

	g_main_context_push_thread_default(context);

	session = soup_session_new_with_options("timeout", 5, NULL);
	message = clawt_http_message_new("GET", url, CLAWT_HTTP_NO_CREDENTIALS);

	if (message != NULL) {
		body = clawt_http_send_and_read(session, message, NULL, NULL);
		healthy = body != NULL &&
		          SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(message));
	}

	g_clear_object(&message);
	g_clear_object(&session);
	g_main_context_pop_thread_default(context);

	return healthy;
}

/*
 * A container's podman state and how it last ended, from the module's
 * inspect.  %TRUE with @ended's status set (possibly empty) when inspect
 * answered; %FALSE with @error set when it did not.
 *
 * The module returns every value as a string.  `exit_code` and
 * `oom_killed` arrived in podomation 1.0.3; a module without them leaves
 * exit_known off, which on-failure reads as "did not say".
 */
static gboolean
inspect_exit(
	ClawtPodBridge   *bridge,
	const gchar      *connection,
	const gchar      *id,
	ClawtBrokerExit  *ended,
	GError          **error
){
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(GHashTable) result = NULL;
	const gchar *value;
	const gchar *code;
	const gchar *oom;

	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(params, g_strdup("target"), g_strdup(id));
	result = clawt_pod_bridge_call_for(bridge, "container", connection,
	                                   "inspect", params, error);

	if (result == NULL)
		return FALSE;

	value = g_hash_table_lookup(result, "status");
	g_free(ended->status);
	ended->status = g_strdup(value != NULL ? value : "");

	/* Both or neither: half an answer is not an answer. */
	code = g_hash_table_lookup(result, "exit_code");
	oom = g_hash_table_lookup(result, "oom_killed");

	if (code != NULL && oom != NULL) {
		gint64 parsed = 0;

		if (g_ascii_string_to_signed(code, 10, G_MININT, G_MAXINT, &parsed,
		                             NULL)) {
			ended->exit_known = TRUE;
			ended->exit_code = (gint)parsed;
			ended->oom_killed = g_strcmp0(oom, "true") == 0;
		}
	}

	return TRUE;
}

void
clawt_broker_exit_free(
	ClawtBrokerExit *self
){
	if (self == NULL)
		return;

	g_free(self->name);
	g_free(self->status);
	g_free(self);
}

/* A down container named @name, with nothing known about it yet. */
static ClawtBrokerExit *
broker_exit_new(
	const gchar *name
){
	ClawtBrokerExit *ended = g_new0(ClawtBrokerExit, 1);

	ended->name = g_strdup(name);
	ended->status = g_strdup("");
	return ended;
}

gboolean
clawt_broker_exit_is_failure(
	const ClawtBrokerExit *ended
){
	g_return_val_if_fail(ended != NULL, FALSE);

	/*
	 * Only an exit the module reported can be judged.  podman's own
	 * signal exits are already non-zero here (128 + the signal), so a
	 * crash by SIGSEGV is 139 and counts; an OOM kill is 137 too, but
	 * OOMKilled is the reliable signal, so it is asked for by name.
	 */
	if (!ended->exit_known || ended->gone || ended->never_made)
		return FALSE;

	return ended->oom_killed || ended->exit_code != 0;
}

gchar *
clawt_broker_exit_describe(
	const ClawtBrokerExit *ended
){
	g_return_val_if_fail(ended != NULL, NULL);

	if (ended->never_made)
		return g_strdup("was never made");

	if (ended->gone)
		return g_strdup("is gone from podman");

	if (!ended->exit_known)
		return g_strdup_printf("stopped (podman says %s, and the container "
		                       "module does not say how it ended)",
		                       ended->status != NULL && *ended->status != '\0'
		                       ? ended->status : "nothing");

	if (ended->oom_killed)
		return g_strdup_printf("was killed for running out of memory "
		                       "(exit code %d)", ended->exit_code);

	if (ended->exit_code != 0)
		return g_strdup_printf("exited with code %d", ended->exit_code);

	/* Made, never started: podman reports no exit for it, which is 0. */
	if (g_strcmp0(ended->status, "created") == 0 ||
	    g_strcmp0(ended->status, "configured") == 0 ||
	    g_strcmp0(ended->status, "initialized") == 0)
		return g_strdup_printf("never ran (podman says %s)", ended->status);

	return g_strdup("exited cleanly (exit code 0)");
}

gboolean
clawt_broker_should_restart(
	ClawtRestartPolicy     policy,
	const ClawtBrokerExit *ended,
	gchar                **reason
){
	g_autofree gchar *how = NULL;

	g_return_val_if_fail(ended != NULL, FALSE);

	if (reason != NULL)
		*reason = NULL;

	/* always: whatever the exit, the watch starts it again. */
	if (policy == CLAWT_RESTART_ALWAYS)
		return TRUE;

	/* on-failure: a non-zero exit or an OOM kill, and nothing else. */
	if (policy == CLAWT_RESTART_ON_FAILURE &&
	    clawt_broker_exit_is_failure(ended))
		return TRUE;

	if (reason == NULL)
		return FALSE;

	how = clawt_broker_exit_describe(ended);

	if (policy == CLAWT_RESTART_NEVER) {
		*reason = g_strdup_printf("%s %s, and broker.restart is never, so "
		                          "it was not restarted; `clawtilla broker "
		                          "logs` says why", ended->name, how);
		return FALSE;
	}

	/*
	 * on-failure and not a failure.  The sentence says which of the
	 * three it was, because "not restarted" alone reads as a bug: a
	 * clean exit, a container somebody removed, or a module too old to
	 * say.
	 */
	if (!ended->exit_known && !ended->gone && !ended->never_made)
		*reason = g_strdup_printf(
			"%s %s, and broker.restart is on-failure, which cannot tell a "
			"failure from a clean exit without podomation 1.0.3 or later, "
			"so it was not restarted; `clawtilla broker start` starts it "
			"again", ended->name, how);
	else
		*reason = g_strdup_printf(
			"%s %s, and broker.restart is on-failure, which restarts only "
			"a container that exited non-zero or was killed for memory, "
			"so it was not restarted; `clawtilla broker start` starts it "
			"again", ended->name, how);

	return FALSE;
}

/*
 * Whether podman answers at all: its container list.  Asked only after
 * an inspect fails, to tell "that container is gone" (podman answers
 * this) from "podman is not there" (it does not).
 */
static gboolean
podman_answers(
	ClawtPodBridge  *bridge,
	const gchar     *connection,
	GError         **error
){
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(GHashTable) result = NULL;

	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	result = clawt_pod_bridge_call_for(bridge, "container", connection,
	                                   "list", params, error);

	return result != NULL;
}

gboolean
clawt_broker_container_is_up(
	const gchar *status
){
	/*
	 * Paused and stopping are not up in any useful sense, but they are
	 * not a container that ended either: restarting one would fight
	 * whoever paused or is stopping it.  The health check sees a paused
	 * gateway stop answering, which is the right report for it.
	 */
	return g_strcmp0(status, "running") == 0 ||
	       g_strcmp0(status, "paused") == 0 ||
	       g_strcmp0(status, "stopping") == 0;
}

gboolean
clawt_broker_find_stopped(
	ClawtPodBridge      *bridge,
	const gchar         *connection,
	const gchar * const *ids,
	const gchar * const *names,
	ClawtBrokerExit    **stopped,
	GError             **error
){
	guint i;

	g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(bridge), FALSE);
	g_return_val_if_fail(stopped != NULL, FALSE);

	*stopped = NULL;

	if (!clawt_pod_bridge_load_module_for(bridge, "container", connection,
	                                      error))
		return FALSE;

	for (i = 0; ids != NULL && ids[i] != NULL; i++) {
		g_autoptr(GError) local = NULL;
		g_autoptr(ClawtBrokerExit) ended = NULL;
		const gchar *name = (names != NULL && names[i] != NULL)
		                    ? names[i] : ids[i];

		ended = broker_exit_new(name);

		/* Never made, so certainly not running. */
		if (*ids[i] == '\0') {
			ended->never_made = TRUE;
			*stopped = g_steal_pointer(&ended);
			return TRUE;
		}

		/*
		 * Inspect, not the running list.  The list used to be the only
		 * place the module said whether a container ran, because its
		 * inspect left the status empty; podomation 1.0.2 fills it, and
		 * 1.0.3 adds how a stopped one ended.
		 */
		if (!inspect_exit(bridge, connection, ids[i], ended, &local)) {
			g_autoptr(GError) list_error = NULL;

			/*
			 * podman answering its list while refusing this inspect
			 * means the container is gone (a 404); podman not
			 * answering at all is not the gateway stopping.
			 */
			if (podman_answers(bridge, connection, &list_error)) {
				ended->gone = TRUE;
				*stopped = g_steal_pointer(&ended);
				return TRUE;
			}

			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}

		/* "It did not say" is not "it said no". */
		if (*ended->status == '\0' || g_strcmp0(ended->status, "unknown") == 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
			            "podman's inspect of %s gave no state%s", name,
			            *ended->status == '\0'
			            ? " (the container module is older than "
			              "podomation 1.0.2)"
			            : "");
			return FALSE;
		}

		if (!clawt_broker_container_is_up(ended->status)) {
			*stopped = g_steal_pointer(&ended);
			return TRUE;
		}
	}

	return TRUE;
}

static void
probe_worker(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Probe *probe = task_data;
	g_autoptr(GError) error = NULL;

	if (!clawt_broker_find_stopped(probe->bridge, probe->connection,
	                               (const gchar * const *)probe->ids,
	                               (const gchar * const *)probe->names,
	                               &probe->missing, &error)) {
		probe->answer_error = g_strdup(error != NULL ? error->message
		                                             : "no reason given");
		g_task_return_boolean(task, TRUE);
		return;
	}

	probe->answered = TRUE;

	if (probe->missing == NULL)
		probe->healthy = probe_health(probe->health_url);

	g_task_return_boolean(task, TRUE);
}

/*
 * Remembers how a container last ended and what the watch did about it,
 * for broker.status: the detail says it in a sentence while the broker
 * is failed, and last_exit keeps it after a restart has cleared that.
 */
static void
note_exit(
	ClawtBroker           *self,
	const ClawtBrokerExit *ended,
	gboolean               restarted
){
	ClawtBrokerExit *copy = g_new0(ClawtBrokerExit, 1);

	*copy = *ended;
	copy->name = g_strdup(ended->name);
	copy->status = g_strdup(ended->status);

	clawt_broker_exit_free(self->last_exit);
	self->last_exit = copy;
	self->last_exit_restarted = restarted;
}

static void
handle_exit(
	ClawtBroker           *self,
	const ClawtBrokerExit *ended
){
	gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
	g_autofree gchar *detail = NULL;
	g_autofree gchar *how = NULL;
	guint i;

	/*
	 * The policy first: never and on-failure-after-a-clean-exit leave it
	 * down, and say why in the detail.  The state is failed rather than
	 * stopped even for a clean exit: the broker is not serving, its other
	 * containers may still run, and a stopped broker is one the daemon
	 * starts again at the next config reload -- which would make
	 * on-failure mean always one edit later.
	 */
	if (!clawt_broker_should_restart(self->restart, ended, &detail)) {
		note_exit(self, ended, FALSE);
		stop_watch(self);
		set_state(self, CLAWT_BROKER_STATE_ERROR, detail);
		return;
	}

	how = clawt_broker_exit_describe(ended);

	/* Only the restarts inside the window count towards giving up. */
	for (i = self->restart_times->len; i > 0; i--) {
		if (now - g_array_index(self->restart_times, gint64, i - 1) >
		    BROKER_RESTART_WINDOW_SECONDS)
			g_array_remove_index(self->restart_times, i - 1);
	}

	if (self->restart_times->len >= BROKER_RESTART_LIMIT) {
		detail = g_strdup_printf(
			"%s keeps stopping (it %s): restarted %u times in ten minutes, "
			"and not again until somebody starts it; `clawtilla broker logs` "
			"says why", ended->name, how, (guint)self->restart_times->len);
		note_exit(self, ended, FALSE);
		stop_watch(self);
		set_state(self, CLAWT_BROKER_STATE_ERROR, detail);
		return;
	}

	note_exit(self, ended, TRUE);
	g_array_append_val(self->restart_times, now);
	self->restarts++;
	g_message("broker: %s %s; broker.restart is %s, so starting it again (%u)",
	          ended->name, how,
	          clawt_enum_to_nick(CLAWT_TYPE_RESTART_POLICY,
	                             (gint)self->restart),
	          self->restarts);
	broker_begin_start(self);
}

void
clawt_broker_test_handle_exit(
	ClawtBroker           *self,
	const ClawtBrokerExit *ended
){
	g_return_if_fail(CLAWT_IS_BROKER(self));
	g_return_if_fail(ended != NULL);

	handle_exit(self, ended);
}

static void
on_probed(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	ClawtBroker *self = CLAWT_BROKER(source);
	Probe *probe = g_task_get_task_data(G_TASK(result));
	guint generation = GPOINTER_TO_UINT(user_data);
	gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;

	self->probing = FALSE;

	if (generation != self->generation)
		return;

	/*
	 * podman not answering is not the gateway stopping: "it did not say"
	 * is not "it said no", and restarting on it would replace a working
	 * gateway because a socket was busy.
	 */
	if (!probe->answered) {
		g_warning("broker: podman did not answer the watch: %s",
		          probe->answer_error != NULL ? probe->answer_error : "?");
		return;
	}

	if (probe->missing != NULL) {
		handle_exit(self, probe->missing);
		return;
	}

	if (probe->healthy) {
		self->unhealthy = 0;
		set_state(self, CLAWT_BROKER_STATE_RUNNING, NULL);
		start_watch(self, BROKER_WATCH_RUNNING_SECONDS);
		return;
	}

	if (self->state == CLAWT_BROKER_STATE_STARTING) {
		if (now - self->starting_since > BROKER_START_DEADLINE_SECONDS) {
			g_autofree gchar *detail = g_strdup_printf(
				"its containers run, and %s has not answered at %s in ten "
				"minutes; `clawtilla broker logs` says why",
				clawt_broker_type_label(
					clawt_broker_get_type_value(self->config)),
				self->health_url);

			set_state(self, CLAWT_BROKER_STATE_ERROR, detail);
			start_watch(self, BROKER_WATCH_RUNNING_SECONDS);
		}

		return;
	}

	if (++self->unhealthy >= BROKER_UNHEALTHY_LIMIT) {
		g_autofree gchar *detail = g_strdup_printf(
			"its containers run, and it has stopped answering at %s",
			self->health_url);

		set_state(self, CLAWT_BROKER_STATE_ERROR, detail);
	}
}

static gboolean
on_watch_tick(
	gpointer user_data
){
	ClawtBroker *self = user_data;
	Probe *probe;
	GTask *task;
	GPtrArray *ids;
	GPtrArray *names;
	guint i;

	if (self->probing || self->computers == NULL)
		return G_SOURCE_CONTINUE;

	probe = g_new0(Probe, 1);
	probe->bridge = g_object_ref(self->bridge);
	probe->connection = g_strdup(self->connection);
	probe->health_url = g_strdup(self->health_url);

	ids = g_ptr_array_new();
	names = g_ptr_array_new();

	for (i = 0; i < self->computers->len; i++) {
		ClawtContainerComputer *container = CLAWT_CONTAINER_COMPUTER(
			g_ptr_array_index(self->computers, i));
		const gchar *id = clawt_container_computer_get_container_id(container);

		g_ptr_array_add(ids, g_strdup(id != NULL ? id : ""));
		g_ptr_array_add(names,
		                g_strdup(clawt_container_computer_get_name(container)));
	}

	g_ptr_array_add(ids, NULL);
	g_ptr_array_add(names, NULL);
	probe->ids = (GStrv)g_ptr_array_free(ids, FALSE);
	probe->names = (GStrv)g_ptr_array_free(names, FALSE);

	self->probing = TRUE;

	g_main_context_push_thread_default(self->context);
	task = g_task_new(self, NULL, on_probed,
	                  GUINT_TO_POINTER(self->generation));
	g_main_context_pop_thread_default(self->context);

	g_task_set_task_data(task, probe, (GDestroyNotify)probe_free);
	g_task_run_in_thread(task, probe_worker);
	g_object_unref(task);

	return G_SOURCE_CONTINUE;
}

/* ── Logs ── */

typedef struct {
	ClawtPodBridge *bridge;
	gchar          *connection;
	GStrv           names;
} LogsJob;

static void
logs_job_free(
	LogsJob *job
){
	g_clear_object(&job->bridge);
	g_free(job->connection);
	g_strfreev(job->names);
	g_free(job);
}

gchar *
clawt_broker_read_logs(
	ClawtPodBridge      *bridge,
	const gchar         *connection,
	const gchar * const *names,
	const gchar         *tail,
	GError             **error
){
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;

	g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(bridge), NULL);

	if (!clawt_pod_bridge_load_module_for(bridge, "container", connection,
	                                      error))
		return NULL;

	for (i = 0; names != NULL && names[i] != NULL; i++) {
		g_autoptr(GHashTable) params = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, g_free);
		g_autoptr(GHashTable) result = NULL;
		g_autoptr(GError) local = NULL;
		const gchar *text_out;
		const gchar *text_err;
		gboolean has_out;
		gboolean has_err;

		g_hash_table_insert(params, g_strdup("target"), g_strdup(names[i]));
		g_hash_table_insert(params, g_strdup("tail"),
		                    g_strdup(tail != NULL ? tail : "100"));
		result = clawt_pod_bridge_call_for(bridge, "container", connection,
		                                   "logs", params, &local);

		g_string_append_printf(out, "== %s ==\n", names[i]);

		if (result == NULL) {
			g_string_append_printf(out, "(no logs: %s)\n",
			                       local != NULL ? local->message
			                                     : "no reason given");
			continue;
		}

		/*
		 * podomation splits podman's log stream into its two streams,
		 * the way an exec's output is split, and the order between a
		 * stdout line and a stderr line is not kept.  With both, each
		 * gets a heading so nobody reads one after the other as the
		 * order things happened in; with one, it is the whole log.
		 */
		text_out = g_hash_table_lookup(result, "stdout");
		text_err = g_hash_table_lookup(result, "stderr");
		has_out = text_out != NULL && *text_out != '\0';
		has_err = text_err != NULL && *text_err != '\0';

		if (!has_out && !has_err) {
			g_string_append(out, "(no lines yet)\n");
			continue;
		}

		if (has_out) {
			if (has_err)
				g_string_append(out, "-- stdout --\n");
			g_string_append(out, text_out);
			if (!g_str_has_suffix(text_out, "\n"))
				g_string_append_c(out, '\n');
		}

		if (has_err) {
			if (has_out)
				g_string_append(out, "-- stderr --\n");
			g_string_append(out, text_err);
			if (!g_str_has_suffix(text_err, "\n"))
				g_string_append_c(out, '\n');
		}
	}

	/*
	 * A gateway logs what it is handed, and some of what it is handed
	 * is credentials.  Best effort, like every other redaction here:
	 * an opaque token on its own is not recognisable.
	 */
	return clawt_redact_secrets(out->str);
}

static void
logs_worker(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	LogsJob *job = task_data;
	g_autoptr(GError) error = NULL;
	gchar *text;

	text = clawt_broker_read_logs(job->bridge, job->connection,
	                              (const gchar * const *)job->names, NULL,
	                              &error);

	if (text == NULL) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}

	g_task_return_pointer(task, text, g_free);
}

void
clawt_broker_logs_async(
	ClawtBroker         *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GPtrArray *names;
	LogsJob *job;
	GTask *task;
	guint i;

	g_return_if_fail(CLAWT_IS_BROKER(self));

	g_main_context_push_thread_default(self->context);
	task = g_task_new(self, cancellable, callback, user_data);
	g_main_context_pop_thread_default(self->context);

	/* Nothing started here has nothing to show, and nothing is made for it. */
	if (self->computers == NULL) {
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
		                        "this daemon has not started the broker, so "
		                        "it has no containers to read logs from");
		g_object_unref(task);
		return;
	}

	job = g_new0(LogsJob, 1);
	job->bridge = g_object_ref(self->bridge);
	job->connection = g_strdup(self->connection);
	names = g_ptr_array_new();

	for (i = 0; i < self->computers->len; i++)
		g_ptr_array_add(names, g_strdup(clawt_container_computer_get_name(
			CLAWT_CONTAINER_COMPUTER(
				g_ptr_array_index(self->computers, i)))));

	g_ptr_array_add(names, NULL);
	job->names = (GStrv)g_ptr_array_free(names, FALSE);

	g_task_set_task_data(task, job, (GDestroyNotify)logs_job_free);
	g_task_run_in_thread(task, logs_worker);
	g_object_unref(task);
}

gchar *
clawt_broker_logs_finish(
	ClawtBroker   *self,
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Status ── */

void
clawt_broker_build_status(
	ClawtBroker *self,
	ClawtConfig *config,
	JsonBuilder *builder
){
	g_autoptr(GPtrArray) warnings = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) assignments = NULL;
	g_autoptr(GError) refusal = NULL;
	g_autofree gchar *image = NULL;
	g_autofree gchar *dashboard = NULL;
	g_autofree gchar *mcp = NULL;
	gboolean enabled;
	ClawtBrokerType type;
	ClawtBrokerState state;
	guint i;

	g_return_if_fail(CLAWT_IS_CONFIG(config));
	g_return_if_fail(JSON_IS_BUILDER(builder));

	enabled = clawt_config_get_boolean(config, "broker.enabled");
	type = clawt_broker_get_type_value(config);
	state = (self != NULL) ? self->state : CLAWT_BROKER_STATE_STOPPED;

	/* Off and not running is off; off and still running is still running. */
	if (!enabled && (state == CLAWT_BROKER_STATE_STOPPED ||
	                 state == CLAWT_BROKER_STATE_DISABLED))
		state = CLAWT_BROKER_STATE_DISABLED;
	else if (enabled && state == CLAWT_BROKER_STATE_DISABLED)
		state = CLAWT_BROKER_STATE_STOPPED;

	image = clawt_broker_get_image(config);
	dashboard = clawt_broker_dashboard_url(config);
	mcp = clawt_broker_mcp_url(config, NULL);

	json_builder_set_member_name(builder, "enabled");
	json_builder_add_boolean_value(builder, enabled);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder,
	                              clawt_enum_to_nick(CLAWT_TYPE_BROKER_TYPE,
	                                                 (gint)type));
	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, clawt_broker_type_label(type));
	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder,
	                              clawt_enum_to_nick(CLAWT_TYPE_BROKER_STATE,
	                                                 (gint)state));
	json_builder_set_member_name(builder, "state_label");
	json_builder_add_string_value(builder, clawt_broker_state_label(state));

	if (self != NULL && self->detail != NULL) {
		json_builder_set_member_name(builder, "detail");
		json_builder_add_string_value(builder, self->detail);
	}

	json_builder_set_member_name(builder, "image");
	json_builder_add_string_value(builder, image);
	json_builder_set_member_name(builder, "dashboard_url");
	json_builder_add_string_value(builder, dashboard);
	json_builder_set_member_name(builder, "mcp_url");
	json_builder_add_string_value(builder, mcp);
	json_builder_set_member_name(builder, "restarts");
	json_builder_add_int_value(builder, self != NULL ? self->restarts : 0);

	/*
	 * How the last container the watch found down ended, and whether
	 * broker.restart started it again.  exit_code and oom_killed only when
	 * the container module reported them.
	 */
	if (self != NULL && self->last_exit != NULL) {
		g_autofree gchar *how = clawt_broker_exit_describe(self->last_exit);

		json_builder_set_member_name(builder, "last_exit");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "container");
		json_builder_add_string_value(builder, self->last_exit->name);
		json_builder_set_member_name(builder, "status");
		json_builder_add_string_value(builder, self->last_exit->status);
		json_builder_set_member_name(builder, "how");
		json_builder_add_string_value(builder, how);

		if (self->last_exit->exit_known) {
			json_builder_set_member_name(builder, "exit_code");
			json_builder_add_int_value(builder, self->last_exit->exit_code);
			json_builder_set_member_name(builder, "oom_killed");
			json_builder_add_boolean_value(builder,
			                               self->last_exit->oom_killed);
		}

		json_builder_set_member_name(builder, "failure");
		json_builder_add_boolean_value(
			builder, clawt_broker_exit_is_failure(self->last_exit));
		json_builder_set_member_name(builder, "restarted");
		json_builder_add_boolean_value(builder, self->last_exit_restarted);
		json_builder_end_object(builder);
	}

	if (enabled && !clawt_broker_check_confinement(config, &refusal)) {
		json_builder_set_member_name(builder, "refusal");
		json_builder_add_string_value(builder, refusal->message);
	}

	/*
	 * Who reaches which gateway server, and whether the key is there --
	 * whether, never what: the file is tested for, not read.
	 */
	assignments = clawt_broker_assignments(config, warnings);
	json_builder_set_member_name(builder, "assignments");
	json_builder_begin_array(builder);

	for (i = 0; i < assignments->len; i++) {
		ClawtBrokerAssignment *assignment = g_ptr_array_index(assignments, i);
		guint a;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "target");
		json_builder_add_string_value(builder, assignment->target);
		json_builder_set_member_name(builder, "server");
		json_builder_add_string_value(builder, assignment->server);
		json_builder_set_member_name(builder, "connector");
		json_builder_add_string_value(builder, assignment->connector);
		json_builder_set_member_name(builder, "key_stored");
		json_builder_add_boolean_value(
			builder, g_file_test(assignment->key_file, G_FILE_TEST_EXISTS));
		json_builder_set_member_name(builder, "agents");
		json_builder_begin_array(builder);

		for (a = 0; assignment->agents[a] != NULL; a++)
			json_builder_add_string_value(builder, assignment->agents[a]);

		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "warnings");
	json_builder_begin_array(builder);

	for (i = 0; i < warnings->len; i++)
		json_builder_add_string_value(builder, g_ptr_array_index(warnings, i));

	json_builder_end_array(builder);
}

/* ── Accessors and the object itself ── */

ClawtBroker *
clawt_broker_new(
	ClawtPodBridge *bridge,
	GMainContext   *context
){
	ClawtBroker *self;

	g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(bridge), NULL);

	self = g_object_new(CLAWT_TYPE_BROKER, NULL);
	self->bridge = g_object_ref(bridge);
	self->context = (context != NULL) ? g_main_context_ref(context)
	                                  : g_main_context_ref_thread_default();

	return self;
}

void
clawt_broker_set_config(
	ClawtBroker *self,
	ClawtConfig *config,
	const gchar *state_dir
){
	g_return_if_fail(CLAWT_IS_BROKER(self));
	g_return_if_fail(CLAWT_IS_CONFIG(config));

	g_set_object(&self->config, config);
	g_free(self->state_dir);
	self->state_dir = g_strdup(state_dir);

	if (self->state == CLAWT_BROKER_STATE_STOPPED &&
	    !clawt_config_get_boolean(config, "broker.enabled"))
		set_state(self, CLAWT_BROKER_STATE_DISABLED, NULL);
	else if (self->state == CLAWT_BROKER_STATE_DISABLED &&
	         clawt_config_get_boolean(config, "broker.enabled"))
		set_state(self, CLAWT_BROKER_STATE_STOPPED, NULL);
}

ClawtBrokerState
clawt_broker_get_state(
	ClawtBroker *self
){
	g_return_val_if_fail(CLAWT_IS_BROKER(self), CLAWT_BROKER_STATE_ERROR);

	return self->state;
}

const gchar *
clawt_broker_get_detail(
	ClawtBroker *self
){
	g_return_val_if_fail(CLAWT_IS_BROKER(self), NULL);

	return self->detail;
}

guint
clawt_broker_get_restarts(
	ClawtBroker *self
){
	g_return_val_if_fail(CLAWT_IS_BROKER(self), 0);

	return self->restarts;
}

static void
clawt_broker_dispose(
	GObject *object
){
	ClawtBroker *self = CLAWT_BROKER(object);

	stop_watch(self);
	g_clear_pointer(&self->computers, g_ptr_array_unref);
	g_clear_object(&self->bridge);
	g_clear_object(&self->config);

	G_OBJECT_CLASS(clawt_broker_parent_class)->dispose(object);
}

static void
clawt_broker_finalize(
	GObject *object
){
	ClawtBroker *self = CLAWT_BROKER(object);

	g_clear_pointer(&self->context, g_main_context_unref);
	g_free(self->state_dir);
	g_free(self->detail);
	g_free(self->connection);
	g_free(self->health_url);
	g_array_unref(self->restart_times);
	clawt_broker_exit_free(self->last_exit);

	G_OBJECT_CLASS(clawt_broker_parent_class)->finalize(object);
}

static void
clawt_broker_class_init(
	ClawtBrokerClass *klass
){
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = clawt_broker_dispose;
	object_class->finalize = clawt_broker_finalize;

	/**
	 * ClawtBroker::changed:
	 * @self: the broker
	 *
	 * Its state or its detail moved.
	 */
	signals[SIGNAL_CHANGED] = g_signal_new("changed",
	                                       G_TYPE_FROM_CLASS(klass),
	                                       G_SIGNAL_RUN_LAST, 0, NULL, NULL,
	                                       NULL, G_TYPE_NONE, 0);
}

static void
clawt_broker_init(
	ClawtBroker *self
){
	self->state = CLAWT_BROKER_STATE_DISABLED;
	self->restart_times = g_array_new(FALSE, FALSE, sizeof(gint64));
}
