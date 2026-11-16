/*
 * test-broker.c - The integration broker, as data
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no podman, no gateway, no network.  The containers are
 * asserted as data -- image, command, environment, ports, binds under the
 * fixture's state directory -- and so are the connectors the config
 * loader makes, the refusals the relay writes and the status a client
 * reads.  The daemon is driven through clawt_daemon_handle_request() with
 * no socket; where a test needs the broker switched on, `broker.connection`
 * names a socket that does not exist inside the fixture, so the start
 * fails where it would have reached podman rather than reaching the
 * machine's own.  A real start is test-broker-integration.c, behind
 * CLAWT_TEST_INTEGRATION=1.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"
#include "integration/clawt-broker-private.h"
#include "integration/clawt-connector-relay.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
	gchar       *dir;
	gchar       *state;
	ClawtConfig *config;
} Fixture;

static void
fixture_load(
	Fixture     *fixture,
	const gchar *broker,
	const gchar *rest
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-broker-XXXXXX", NULL);
	fixture->state = g_build_filename(fixture->dir, "state", NULL);
	yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s\"\n"
		"defaults:\n"
		"  workspace_root: \"%s/agents\"\n"
		"broker:\n"
		"%s"
		"%s",
		fixture->state, fixture->dir, broker, rest);

	fixture->config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->config);
}

static void
fixture_free(
	Fixture *fixture
){
	g_clear_object(&fixture->config);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
	g_free(fixture->state);
}

static gchar *
slurp(
	const gchar *dir,
	const gchar *name
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));

	return g_strstrip(text);
}

static guint
mode_of(
	const gchar *path
){
	GStatBuf st;

	g_assert_cmpint(g_stat(path, &st), ==, 0);

	return (guint)(st.st_mode & 0777);
}

static ClawtBrokerContainerSpec *
spec_named(
	GPtrArray   *spec,
	const gchar *name
){
	guint i;

	for (i = 0; i < spec->len; i++) {
		ClawtBrokerContainerSpec *one = g_ptr_array_index(spec, i);

		if (g_strcmp0(one->name, name) == 0)
			return one;
	}

	return NULL;
}

/* The mount whose target is @target, or NULL. */
static ClawtMount *
mount_at(
	ClawtBrokerContainerSpec *spec,
	const gchar              *target
){
	guint i;

	for (i = 0; i < spec->mounts->len; i++) {
		ClawtMount *mount = g_ptr_array_index(spec->mounts, i);

		if (g_strcmp0(clawt_mount_get_target(mount), target) == 0)
			return mount;
	}

	return NULL;
}

/* ── The image is pinned ─────────────────────────────────────────── */

static void
test_image_must_be_pinned(
	void
){
	g_autoptr(GError) error = NULL;

	g_assert_true(clawt_broker_check_image(CLAWT_BROKER_ANYTHINGMCP_IMAGE,
	                                       NULL));
	g_assert_true(clawt_broker_check_image(CLAWT_BROKER_JENTIC_IMAGE, NULL));
	g_assert_true(clawt_broker_check_image(CLAWT_BROKER_DATABASE_IMAGE,
	                                       NULL));
	g_assert_true(clawt_broker_check_image(
		"localhost:5000/anythingmcp:v0.54.0-arm64", NULL));
	g_assert_true(clawt_broker_check_image(
		"docker.io/helpcodeai/anythingmcp@sha256:61c67981c13da0c4da9d7672"
		"45a455df0df87632e1499e73ba773a372157bea8", NULL));

	/* A registry's port is not a tag. */
	g_assert_false(clawt_broker_check_image("localhost:5000/anythingmcp",
	                                        &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	g_clear_error(&error);

	g_assert_false(clawt_broker_check_image("helpcodeai/anythingmcp:latest",
	                                        &error));
	g_assert_nonnull(strstr(error->message, "latest"));
	g_clear_error(&error);

	g_assert_false(clawt_broker_check_image("", NULL));
	g_assert_false(clawt_broker_check_image(NULL, NULL));
}

/* ── The containers, as data ─────────────────────────────────────── */

static void
test_anythingmcp_spec(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) spec = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *db_dir = NULL;
	g_autofree gchar *run_dir = NULL;
	g_autofree gchar *password = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *jwt = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *env = NULL;
	ClawtBrokerContainerSpec *db;
	ClawtBrokerContainerSpec *app;
	ClawtMount *mount;

	fixture_load(&fixture,
	             "  enabled: true\n"
	             "  port: 47410\n"
	             "  dashboard_port: 47310\n",
	             "");

	g_assert_true(clawt_broker_prepare_state(CLAWT_BROKER_ANYTHINGMCP,
	                                         fixture.state, &error));
	g_assert_no_error(error);

	spec = clawt_broker_build_spec(fixture.config, fixture.state, &error);
	g_assert_no_error(error);
	g_assert_nonnull(spec);
	g_assert_cmpuint(spec->len, ==, 2);

	dir = clawt_broker_dir(fixture.state);
	db_dir = g_build_filename(dir, "db", NULL);
	run_dir = g_build_filename(dir, "run", NULL);
	password = slurp(dir, "database-password");
	key = slurp(dir, "encryption-key");
	jwt = slurp(dir, "jwt-secret");

	/* State: 0700 at the top, 0600 secrets, the key exactly 32 chars. */
	g_assert_cmpuint(mode_of(dir), ==, 0700);
	g_assert_cmpuint(strlen(key), ==, 32);
	g_assert_cmpuint(strlen(password), ==, 64);
	{
		g_autofree gchar *path = g_build_filename(dir, "encryption-key",
		                                          NULL);

		g_assert_cmpuint(mode_of(path), ==, 0600);
	}

	/* The database starts first and publishes nothing. */
	db = g_ptr_array_index(spec, 0);
	g_assert_cmpstr(db->name, ==, "clawt-broker-anythingmcp-db");
	g_assert_cmpstr(db->image, ==, CLAWT_BROKER_DATABASE_IMAGE);
	g_assert_null(db->ports);
	g_assert_nonnull(strstr(db->command, "listen_addresses="));
	g_assert_nonnull(strstr(db->command, "/clawt-run"));
	g_assert_cmpstr(g_hash_table_lookup(db->env, "POSTGRES_PASSWORD"), ==,
	                password);
	g_assert_cmpstr(g_hash_table_lookup(db->env, "PGDATA"), ==,
	                "/var/lib/postgresql/data/pgdata");

	/*
	 * Bound at the data volume, not at PGDATA: the entrypoint chowns
	 * PGDATA, and a bind source this user no longer owns cannot be
	 * relabelled at the next start.
	 */
	mount = mount_at(db, "/var/lib/postgresql/data");
	g_assert_nonnull(mount);
	g_assert_cmpstr(clawt_mount_get_source(mount), ==, db_dir);
	g_assert_cmpint(clawt_mount_get_relabel(mount), ==,
	                CLAWT_RELABEL_PRIVATE);
	mount = mount_at(db, "/clawt-run");
	g_assert_nonnull(mount);
	g_assert_cmpstr(clawt_mount_get_source(mount), ==, run_dir);
	g_assert_cmpint(clawt_mount_get_relabel(mount), ==,
	                CLAWT_RELABEL_SHARED);

	/* The gateway, reaching the database by its socket. */
	app = spec_named(spec, "clawt-broker-anythingmcp");
	g_assert_nonnull(app);
	g_assert_true(app == g_ptr_array_index(spec, 1));
	g_assert_cmpstr(app->image, ==, CLAWT_BROKER_ANYTHINGMCP_IMAGE);

	/* Named, or a container computer's `sleep infinity` runs instead. */
	g_assert_cmpstr(app->command, ==, "[\"./start.sh\"]");
	url = g_strdup_printf(
		"postgresql://amcp:%s@localhost/anythingmcp?host=/clawt-run",
		password);
	g_assert_cmpstr(g_hash_table_lookup(app->env, "DATABASE_URL"), ==, url);
	g_assert_cmpstr(g_hash_table_lookup(app->env, "ENCRYPTION_KEY"), ==, key);
	g_assert_cmpstr(g_hash_table_lookup(app->env, "JWT_SECRET"), ==, jwt);
	g_assert_cmpstr(g_hash_table_lookup(app->env, "MCP_ALLOW_ANONYMOUS"), ==,
	                "false");
	g_assert_cmpstr(g_hash_table_lookup(app->env, "SERVER_URL"), ==,
	                "http://127.0.0.1:47410");
	g_assert_cmpstr(g_hash_table_lookup(app->env, "FRONTEND_URL"), ==,
	                "http://127.0.0.1:47310");

	/* No instance-wide token reaching every server: per-server keys only. */
	g_assert_false(g_hash_table_contains(app->env, "MCP_BEARER_TOKEN"));
	g_assert_false(g_hash_table_contains(app->env, "MCP_API_KEY"));

	/* Published on the bind only, never podman's default of everywhere. */
	g_assert_nonnull(strstr(app->ports, "\"host_ip\":\"127.0.0.1\""));
	g_assert_nonnull(strstr(app->ports, "\"host_port\":47410"));
	g_assert_nonnull(strstr(app->ports, "\"container_port\":4000"));
	g_assert_nonnull(strstr(app->ports, "\"host_port\":47310"));
	g_assert_nonnull(strstr(app->ports, "\"container_port\":3000"));
	mount = mount_at(app, "/clawt-run");
	g_assert_nonnull(mount);
	g_assert_cmpstr(clawt_mount_get_source(mount), ==, run_dir);

	/* Every bind is under the fixture's state directory. */
	g_assert_true(g_str_has_prefix(db_dir, fixture.state));

	env = clawt_broker_container_spec_env_json(app);
	g_assert_nonnull(strstr(env, "\"NODE_ENV\":\"production\""));

	fixture_free(&fixture);
}

static void
test_prepare_keeps_secrets(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	fixture_load(&fixture, "  enabled: true\n", "");

	g_assert_true(clawt_broker_prepare_state(CLAWT_BROKER_ANYTHINGMCP,
	                                         fixture.state, &error));
	dir = clawt_broker_dir(fixture.state);
	first = slurp(dir, "encryption-key");

	/* A new key would make every stored SaaS credential unreadable. */
	g_assert_true(clawt_broker_prepare_state(CLAWT_BROKER_ANYTHINGMCP,
	                                         fixture.state, &error));
	second = slurp(dir, "encryption-key");
	g_assert_cmpstr(first, ==, second);

	fixture_free(&fixture);
}

static void
test_jentic_spec(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) spec = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *data = NULL;
	g_autofree gchar *key = NULL;
	ClawtBrokerContainerSpec *app;
	ClawtMount *mount;
	gsize decoded = 0;
	g_autofree guchar *raw = NULL;

	fixture_load(&fixture,
	             "  enabled: true\n"
	             "  type: jentic\n"
	             "  port: 47810\n",
	             "");

	g_assert_true(clawt_broker_prepare_state(CLAWT_BROKER_JENTIC,
	                                         fixture.state, &error));
	spec = clawt_broker_build_spec(fixture.config, fixture.state, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(spec->len, ==, 1);

	dir = clawt_broker_dir(fixture.state);
	data = g_build_filename(dir, "data", NULL);
	key = slurp(dir, "jentic-encryption-key");
	raw = g_base64_decode(key, &decoded);
	g_assert_cmpuint(decoded, ==, 32);

	app = g_ptr_array_index(spec, 0);
	g_assert_cmpstr(app->name, ==, "clawt-broker-jentic");
	g_assert_cmpstr(app->image, ==, CLAWT_BROKER_JENTIC_IMAGE);

	/* The documented opt-out, and the other two things that reach out. */
	g_assert_cmpstr(g_hash_table_lookup(app->env,
	                                    "JENTIC__TELEMETRY__ENABLED"), ==,
	                "false");
	g_assert_cmpstr(g_hash_table_lookup(app->env,
	                                    "JENTIC__RELEASE_CHECK__ENABLED"), ==,
	                "false");
	g_assert_cmpstr(g_hash_table_lookup(
		app->env, "JENTIC__OBSERVABILITY__TRACING__EXPORTER"), ==, "none");
	g_assert_cmpstr(g_hash_table_lookup(app->env, "JENTIC_ENCRYPTION_KEY"),
	                ==, key);
	g_assert_cmpstr(g_hash_table_lookup(app->env,
	                                    "JENTIC__SERVER__MCP__ENABLED"), ==,
	                "true");

	/* Control plane and broker, in one container sharing a loopback. */
	g_assert_nonnull(strstr(app->command, "jentic_one.migrations.run"));
	g_assert_nonnull(strstr(app->command, "JENTIC__APPS=broker"));
	g_assert_nonnull(strstr(app->command, "wait -n"));
	g_assert_nonnull(strstr(app->ports, "\"host_port\":47810"));
	g_assert_nonnull(strstr(app->ports, "\"container_port\":8000"));
	g_assert_null(strstr(app->ports, "8100"));

	mount = mount_at(app, "/data");
	g_assert_nonnull(mount);
	g_assert_cmpstr(clawt_mount_get_source(mount), ==, data);

	fixture_free(&fixture);
}

static void
test_bad_port_refused(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) spec = NULL;
	g_autoptr(GError) error = NULL;

	fixture_load(&fixture, "  enabled: true\n  port: 70000\n", "");
	g_assert_true(clawt_broker_prepare_state(CLAWT_BROKER_ANYTHINGMCP,
	                                         fixture.state, NULL));

	spec = clawt_broker_build_spec(fixture.config, fixture.state, &error);
	g_assert_null(spec);
	g_assert_nonnull(strstr(error->message, "broker.port"));

	fixture_free(&fixture);
}

/* ── Who reaches it ──────────────────────────────────────────────── */

#define FLEET \
	"teams:\n" \
	"  - id: research\n" \
	"    broker_server: team-srv\n" \
	"agents:\n" \
	"  - id: scout\n" \
	"    broker_server: scout-srv\n" \
	"    team: research\n" \
	"  - id: kudu\n" \
	"    team: research\n" \
	"  - id: gnu\n" \
	"    team: research\n" \
	"  - id: hermit\n" \
	"    broker_server: hermit-srv\n" \
	"    runtime:\n" \
	"      confine: bwrap\n" \
	"      network: gated\n" \
	"  - id: loner\n"

static void
test_assignments_and_connectors(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) warnings = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) assignments = NULL;
	g_autoptr(ClawtBrokerAssignment) mine = NULL;
	g_autoptr(GPtrArray) bindings = NULL;
	g_autofree gchar *saved = NULL;
	ClawtBrokerAssignment *one;
	ClawtIntegrationConfig *connector;
	g_auto(GStrv) agents = NULL;
	guint i;
	gboolean found = FALSE;

	fixture_load(&fixture, "  enabled: true\n  port: 47400\n", FLEET);

	assignments = clawt_broker_assignments(fixture.config, warnings);
	g_assert_cmpuint(assignments->len, ==, 2);

	/* An agent's own server wins over its team's, and comes first. */
	one = g_ptr_array_index(assignments, 0);
	g_assert_cmpstr(one->target, ==, "scout");
	g_assert_cmpstr(one->server, ==, "scout-srv");
	g_assert_cmpstr(one->connector, ==, "broker-agent-scout");
	g_assert_cmpstr(one->agents[0], ==, "scout");
	g_assert_null(one->agents[1]);
	g_assert_true(g_str_has_prefix(one->key_file, fixture.state));
	g_assert_true(g_str_has_suffix(one->key_file, "/broker/keys/"
	                                              "agent-scout.json"));

	/* Two agents through their team's one server -- two keys, one test. */
	one = g_ptr_array_index(assignments, 1);
	g_assert_cmpstr(one->target, ==, "team:research");
	g_assert_cmpstr(one->connector, ==, "broker-team-research");
	g_assert_cmpstr(one->agents[0], ==, "kudu");
	g_assert_cmpstr(one->agents[1], ==, "gnu");
	g_assert_null(one->agents[2]);

	/* A gated agent cannot reach loopback, so it is told, not given it. */
	for (i = 0; i < warnings->len; i++)
		if (strstr(g_ptr_array_index(warnings, i), "'hermit'") != NULL)
			found = TRUE;
	g_assert_true(found);

	mine = clawt_broker_assignment_for_agent(fixture.config, "gnu");
	g_assert_nonnull(mine);
	g_assert_cmpstr(mine->target, ==, "team:research");
	g_assert_null(clawt_broker_assignment_for_agent(fixture.config,
	                                                "hermit"));
	g_assert_null(clawt_broker_assignment_for_agent(fixture.config,
	                                                "loner"));

	/* The loader made the connector, and it is a broker's. */
	connector = clawt_config_get_integration(fixture.config,
	                                         "broker-agent-scout");
	g_assert_nonnull(connector);
	g_assert_true(clawt_integration_config_is_managed(connector));
	g_assert_cmpstr(clawt_integration_config_get_type_id(connector), ==,
	                "connector");
	g_assert_cmpstr(clawt_integration_config_get_string(connector, NULL,
	                                                    "provider"), ==,
	                "anythingmcp");
	g_assert_cmpstr(clawt_integration_config_get_string(connector, NULL,
	                                                    "url"), ==,
	                "http://127.0.0.1:47400/mcp/scout-srv");
	agents = clawt_integration_config_get_agents(connector);
	g_assert_cmpstr(agents[0], ==, "scout");
	g_assert_null(agents[1]);

	/* Through the one resolver, to the agents it names and no others. */
	bindings = clawt_integration_resolve_for_agent(
		fixture.config, clawt_config_get_agent(fixture.config, "kudu"));
	found = FALSE;
	for (i = 0; i < bindings->len; i++) {
		const gchar *name = clawt_integration_binding_get_name(
			g_ptr_array_index(bindings, i));

		g_assert_cmpstr(name, !=, "broker-agent-scout");
		if (g_strcmp0(name, "broker-team-research") == 0)
			found = TRUE;
	}
	g_assert_true(found);

	/* Derived, so never written back. */
	saved = clawt_config_to_string(fixture.config);
	g_assert_null(strstr(saved, "broker-agent-scout"));

	fixture_free(&fixture);
}

static void
test_off_makes_no_connector(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) assignments = NULL;

	fixture_load(&fixture, "  enabled: false\n", FLEET);

	assignments = clawt_broker_assignments(fixture.config, NULL);
	g_assert_cmpuint(assignments->len, ==, 0);
	g_assert_null(clawt_config_get_integration(fixture.config,
	                                           "broker-agent-scout"));

	fixture_free(&fixture);
}

static void
test_jentic_needs_confinement(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) warnings = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) assignments = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *jentic_url = NULL;
	ClawtBrokerAssignment *one;

	fixture_load(&fixture,
	             "  enabled: true\n  type: jentic\n  port: 47800\n",
	             "agents:\n"
	             "  - id: boxed\n"
	             "    broker_server: jentic-agent-1\n"
	             "    runtime:\n"
	             "      confine: bwrap\n"
	             "  - id: loose\n"
	             "    broker_server: jentic-agent-2\n");

	/* The unconfined agent is not given it... */
	assignments = clawt_broker_assignments(fixture.config, warnings);
	g_assert_cmpuint(assignments->len, ==, 1);
	one = g_ptr_array_index(assignments, 0);
	g_assert_cmpstr(one->target, ==, "boxed");
	g_assert_cmpuint(warnings->len, ==, 1);
	g_assert_nonnull(strstr(g_ptr_array_index(warnings, 0), "bwrap"));

	/* ...and the broker itself is not started while it names a server. */
	g_assert_false(clawt_broker_check_confinement(fixture.config, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
	g_assert_nonnull(strstr(error->message, "loose"));
	g_assert_null(strstr(error->message, "boxed"));

	/* Refused where it arrives, not saved and refused at the next start. */
	g_clear_error(&error);
	g_assert_false(clawt_broker_check_setting(fixture.config, "loose", NULL,
	                                          "broker_server", "x", &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
	g_clear_error(&error);
	g_assert_false(clawt_broker_check_setting(fixture.config, "boxed", NULL,
	                                          "runtime.confine", "none",
	                                          &error));
	g_clear_error(&error);
	g_assert_true(clawt_broker_check_setting(fixture.config, "boxed", NULL,
	                                         "broker_server", "y", NULL));
	g_assert_true(clawt_broker_check_setting(fixture.config, "loose", NULL,
	                                         "broker_server", "", NULL));

	/* One endpoint: the key says which Jentic agent is calling. */
	jentic_url = clawt_broker_mcp_url(fixture.config, one->server);
	g_assert_cmpstr(jentic_url, ==, "http://127.0.0.1:47800/mcp");

	fixture_free(&fixture);
}

static void
test_names_and_paths(
	void
){
	g_autofree gchar *agent = clawt_broker_connector_name("scout");
	g_autofree gchar *team = clawt_broker_connector_name("team:research");
	g_autofree gchar *back_agent =
		clawt_broker_target_for_connector("broker-agent-scout");
	g_autofree gchar *back_team =
		clawt_broker_target_for_connector("broker-team-research");
	g_autofree gchar *key = clawt_broker_key_path("/s", "team:research");
	g_autofree gchar *wild = clawt_broker_dial_host("0.0.0.0");
	g_autofree gchar *six = clawt_broker_dial_host("::1");
	g_autofree gchar *any6 = clawt_broker_dial_host("::");

	g_assert_cmpstr(agent, ==, "broker-agent-scout");
	g_assert_cmpstr(team, ==, "broker-team-research");
	g_assert_cmpstr(back_agent, ==, "scout");
	g_assert_cmpstr(back_team, ==, "team:research");
	g_assert_null(clawt_broker_target_for_connector("github"));
	g_assert_cmpstr(key, ==, "/s/broker/keys/team-research.json");
	g_assert_cmpstr(wild, ==, "127.0.0.1");
	g_assert_cmpstr(six, ==, "[::1]");
	g_assert_cmpstr(any6, ==, "[::1]");

	g_assert_false(clawt_broker_target_is_valid("../etc"));
	g_assert_false(clawt_broker_target_is_valid("team:"));
	g_assert_false(clawt_broker_target_is_valid(NULL));
	g_assert_true(clawt_broker_target_is_valid("team:research"));
}

static void
test_server_id_is_checked(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) warnings = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) assignments = NULL;

	/* A server id lands in a URL path; one with a slash would move it. */
	fixture_load(&fixture, "  enabled: true\n",
	             "agents:\n"
	             "  - id: scout\n"
	             "    broker_server: \"../api/admin\"\n");

	assignments = clawt_broker_assignments(fixture.config, warnings);
	g_assert_cmpuint(assignments->len, ==, 0);
	g_assert_cmpuint(warnings->len, ==, 1);

	fixture_free(&fixture);
}

/* ── What the relay says ─────────────────────────────────────────── */

static JsonObject *
parse(
	JsonParser  *parser,
	const gchar *text
){
	g_assert_nonnull(text);
	g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));

	return json_node_get_object(json_parser_get_root(parser));
}

static const gchar *
result_text(
	JsonObject *reply
){
	JsonObject *result = json_object_get_object_member(reply, "result");
	JsonArray *content = json_object_get_array_member(result, "content");

	g_assert_true(json_object_get_boolean_member(result, "isError"));

	return json_object_get_string_member(
		json_array_get_object_element(content, 0), "text");
}

static void
test_refusals_name_the_gateway(
	void
){
	static const gchar *call =
		"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"orders_list\"}}";
	static const gchar *list =
		"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/list\"}";
	g_autoptr(JsonParser) p1 = json_parser_new();
	g_autoptr(JsonParser) p2 = json_parser_new();
	g_autoptr(JsonParser) p3 = json_parser_new();
	g_autoptr(JsonParser) p4 = json_parser_new();
	g_autofree gchar *forbidden = NULL;
	g_autofree gchar *unauthorized = NULL;
	g_autofree gchar *down = NULL;
	g_autofree gchar *listing = NULL;
	JsonObject *reply;
	const gchar *text;

	forbidden = clawt_broker_refusal_reply(call, "AnythingMCP",
	                                       "broker-agent-scout", 403,
	                                       "MCP server not found");
	reply = parse(p1, forbidden);
	g_assert_cmpint(json_object_get_int_member(reply, "id"), ==, 7);
	text = result_text(reply);
	g_assert_true(g_str_has_prefix(text, "AnythingMCP"));
	g_assert_nonnull(strstr(text, "HTTP 403"));
	g_assert_nonnull(strstr(text, "MCP server not found"));
	g_assert_nonnull(strstr(text, "not by clawtilla"));

	/* A missing key says which command stores it. */
	unauthorized = clawt_broker_refusal_reply(call, "Jentic One",
	                                          "broker-team-research", 401,
	                                          NULL);
	text = result_text(parse(p2, unauthorized));
	g_assert_nonnull(strstr(text, "Jentic One"));
	g_assert_nonnull(strstr(text, "clawtilla broker key team:research"));

	/* A gateway that does not answer still gets an answer, not a hang. */
	down = clawt_broker_refusal_reply(call, "AnythingMCP",
	                                  "broker-agent-scout", 0,
	                                  "Connection refused");
	text = result_text(parse(p3, down));
	g_assert_nonnull(strstr(text, "did not answer"));
	g_assert_nonnull(strstr(text, "clawtilla broker status"));

	/* Anything but a tool call is a JSON-RPC error. */
	listing = clawt_broker_refusal_reply(list, "AnythingMCP",
	                                     "broker-agent-scout", 500, NULL);
	reply = parse(p4, listing);
	g_assert_true(json_object_has_member(reply, "error"));
	g_assert_nonnull(strstr(json_object_get_string_member(
		json_object_get_object_member(reply, "error"), "message"),
		"HTTP 500"));

	/* A notification has nobody to answer. */
	g_assert_null(clawt_broker_refusal_reply(
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
		"AnythingMCP", "broker-agent-scout", 403, NULL));
}

static void
test_errors_are_annotated(
	void
){
	g_autoptr(JsonParser) p1 = json_parser_new();
	g_autoptr(JsonParser) p2 = json_parser_new();
	g_autofree gchar *error = NULL;
	g_autofree gchar *failed = NULL;
	g_autofree gchar *again = NULL;
	JsonObject *reply;

	error = clawt_broker_annotate_reply(
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32602,"
		"\"message\":\"Tool nope not found\"}}", "AnythingMCP");
	reply = parse(p1, error);
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(reply, "error"), "message"), ==,
		"AnythingMCP answered: Tool nope not found");

	failed = clawt_broker_annotate_reply(
		"{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"isError\":true,"
		"\"content\":[{\"type\":\"text\",\"text\":\"upstream 404\"}]}}",
		"Jentic One");
	g_assert_cmpstr(result_text(parse(p2, failed)), ==,
	                "Jentic One answered: upstream 404");

	/* Success is left alone, and nothing is named twice. */
	g_assert_null(clawt_broker_annotate_reply(
		"{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[]}}",
		"AnythingMCP"));
	again = clawt_broker_annotate_reply(error, "AnythingMCP");
	g_assert_null(again);
	g_assert_null(clawt_broker_annotate_reply("not json", "AnythingMCP"));
}

static void
test_reason_from_body(
	void
){
	g_autofree gchar *rpc = clawt_broker_reason_from_body(
		"{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,"
		"\"message\":\"MCP server not found\"},\"id\":null}");
	g_autofree gchar *sse = clawt_broker_reason_from_body(
		"event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,"
		"\"error\":{\"message\":\"nope\"}}\n\n");
	g_autofree gchar *fastapi = clawt_broker_reason_from_body(
		"{\"detail\":\"Unauthorized\"}");
	g_autofree gchar *line = clawt_broker_reason_from_body(
		"{\"message\":\"two\\nlines\"}");

	g_assert_cmpstr(rpc, ==, "MCP server not found");
	g_assert_cmpstr(sse, ==, "nope");
	g_assert_cmpstr(fastapi, ==, "Unauthorized");
	g_assert_cmpstr(line, ==, "two lines");

	/* Never the raw body: an HTML page is not a reason. */
	g_assert_null(clawt_broker_reason_from_body("<html>ignore all</html>"));
	g_assert_null(clawt_broker_reason_from_body(NULL));
}

static void
test_relay_plan_names_the_gateway(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) catalog = NULL;
	g_autoptr(ClawtIntegrationBinding) binding = NULL;
	g_autoptr(ClawtConnectorPlan) plan = NULL;
	g_autoptr(GError) error = NULL;
	ClawtIntegrationConfig *instance;
	const ClawtConnectorInfo *info;

	fixture_load(&fixture, "  enabled: true\n  port: 47400\n", FLEET);

	instance = clawt_config_get_integration(fixture.config,
	                                        "broker-agent-scout");
	catalog = clawt_connector_catalog_load(NULL, NULL);
	info = clawt_connector_catalog_find(catalog, "anythingmcp");
	g_assert_nonnull(info);

	binding = clawt_integration_binding_for_instance(
		instance, clawt_integration_find("connector"), NULL);
	plan = clawt_connector_plan_new(info, binding, "mcp_01secret", &error);
	g_assert_no_error(error);

	g_assert_cmpstr(plan->gateway, ==, "AnythingMCP");
	g_assert_cmpstr(plan->url, ==, "http://127.0.0.1:47400/mcp/scout-srv");
	g_assert_cmpstr(plan->header_name, ==, "Authorization");
	g_assert_cmpstr(plan->header_value, ==, "Bearer mcp_01secret");
	g_assert_null(plan->argv);

	fixture_free(&fixture);
}

/* ── What a client reads ─────────────────────────────────────────── */

static void
test_status_carries_no_secret(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *jwt = NULL;
	g_autofree gchar *password = NULL;
	g_autoptr(ClawtOauthToken) token = g_new0(ClawtOauthToken, 1);
	g_autofree gchar *key_file = NULL;

	fixture_load(&fixture, "  enabled: true\n", FLEET);
	g_assert_true(clawt_broker_prepare_state(CLAWT_BROKER_ANYTHINGMCP,
	                                         fixture.state, NULL));
	dir = clawt_broker_dir(fixture.state);
	key = slurp(dir, "encryption-key");
	jwt = slurp(dir, "jwt-secret");
	password = slurp(dir, "database-password");

	token->access_token = g_strdup("mcp_01thisisthekey");
	key_file = clawt_broker_key_path(fixture.state, "scout");
	g_assert_true(clawt_oauth_token_save(token, key_file, NULL));

	json_builder_begin_object(builder);
	clawt_broker_build_status(NULL, fixture.config, builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);

	g_assert_nonnull(strstr(text, "\"state\":\"stopped\""));
	g_assert_nonnull(strstr(text, "\"dashboard_url\":\"http://127.0.0.1:47300\""));
	g_assert_nonnull(strstr(text, "\"key_stored\":true"));
	g_assert_nonnull(strstr(text, "\"key_stored\":false"));

	/* Whether, never what. */
	g_assert_null(strstr(text, key));
	g_assert_null(strstr(text, jwt));
	g_assert_null(strstr(text, password));
	g_assert_null(strstr(text, "mcp_01thisisthekey"));

	/* Every state has a word, and the word is not the nick. */
	g_assert_cmpstr(clawt_broker_state_label(CLAWT_BROKER_STATE_STARTING), ==,
	                "Starting");
	g_assert_cmpstr(clawt_broker_state_label(CLAWT_BROKER_STATE_ERROR), ==,
	                "Failed");

	fixture_free(&fixture);
}

/* ── Through the daemon, with no socket ──────────────────────────── */

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(
	DaemonFixture *fixture,
	const gchar   *broker
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-broker-daemon-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);

	/*
	 * A connection to a socket that is not there: a broker that is on
	 * fails where it would have reached podman, inside the fixture, and
	 * never reaches the machine's own.
	 */
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n  extra_dirs: []\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"connectors:\n  dir: \"%s/connectors.d\"\n"
		"broker:\n%s  connection: \"unix://%s/no-podman.sock\"\n"
		"agents:\n"
		"  - id: scout\n    broker_server: scout-srv\n"
		"    runtime:\n      autostart: false\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir, fixture->dir, broker, fixture->dir);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	clawt_daemon_start(fixture->daemon, &error);
	g_assert_no_error(error);
}

static void
daemon_teardown(
	DaemonFixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;

	g_clear_pointer(&fixture->context, g_main_context_unref);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

static JsonNode *
ask(
	DaemonFixture *fixture,
	const gchar   *kind,
	const gchar   *payload_json
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

	if (payload_json != NULL) {
		g_autoptr(JsonParser) parser = json_parser_new();

		g_assert_true(json_parser_load_from_data(parser, payload_json, -1,
		                                         NULL));
		clawt_ipc_frame_set_payload(
			frame, json_node_copy(json_parser_get_root(parser)));
	}

	return clawt_daemon_handle_request(fixture->daemon, frame);
}

static gboolean
reply_ok(
	JsonNode *reply
){
	return reply != NULL &&
	       json_object_get_boolean_member(json_node_get_object(reply), "ok");
}

static JsonObject *
payload_of(
	JsonNode *reply
){
	return json_object_get_object_member(json_node_get_object(reply),
	                                     "payload");
}

static const gchar *
error_message(
	JsonNode *reply
){
	return json_object_get_string_member(json_node_get_object(reply),
	                                     "error");
}

static void
test_daemon_off(
	void
){
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) status = NULL;
	g_autoptr(JsonNode) start = NULL;
	g_autoptr(JsonNode) stored = NULL;
	g_autoptr(JsonNode) bad = NULL;
	g_autofree gchar *path = NULL;
	const gchar *file;

	daemon_setup(&fixture, "  enabled: false\n");

	/* Off: no broker object at all, and a start says how to turn it on. */
	g_assert_null(fixture.daemon->broker);

	status = ask(&fixture, "broker.status", NULL);
	g_assert_true(reply_ok(status));
	g_assert_cmpstr(json_object_get_string_member(payload_of(status),
	                                              "state"), ==, "disabled");

	start = ask(&fixture, "broker.start", NULL);
	g_assert_false(reply_ok(start));
	g_assert_nonnull(strstr(error_message(start), "broker.enabled"));
	g_assert_null(fixture.daemon->broker);

	/* A key is stored 0600, and the path comes back -- never the key. */
	stored = ask(&fixture, "broker.key",
	             "{\"target\":\"scout\",\"key\":\"mcp_01abc\"}");
	g_assert_true(reply_ok(stored));
	file = json_object_get_string_member(payload_of(stored), "key_file");
	path = g_strdup_printf("%s/state/broker/keys/agent-scout.json",
	                       fixture.dir);
	g_assert_cmpstr(file, ==, path);
	g_assert_cmpuint(mode_of(path), ==, 0600);
	{
		g_autoptr(JsonGenerator) generator = json_generator_new();
		g_autofree gchar *text = NULL;

		json_generator_set_root(generator, stored);
		text = json_generator_to_data(generator, NULL);
		g_assert_null(strstr(text, "mcp_01abc"));
	}

	bad = ask(&fixture, "broker.key", "{\"target\":\"../x\",\"key\":\"k\"}");
	g_assert_false(reply_ok(bad));

	daemon_teardown(&fixture);
}

static void
test_daemon_on_without_podman(
	void
){
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) edit = NULL;
	g_autoptr(JsonNode) keyed = NULL;
	g_autoptr(JsonNode) listed = NULL;
	g_autoptr(JsonNode) status = NULL;
	gint64 deadline;

	daemon_setup(&fixture, "  enabled: true\n");

	/*
	 * Daemon start returned with the broker on and still starting: the
	 * podman work is on a worker thread, whose answer can only arrive
	 * through this context, which nobody has iterated yet.  A start that
	 * waited on podman would have come back failed already.
	 */
	g_assert_nonnull(fixture.daemon->broker);
	g_assert_cmpint(clawt_broker_get_state(fixture.daemon->broker), ==,
	                CLAWT_BROKER_STATE_STARTING);

	/* With a watchdog: a test that can hang is worse than one that fails. */
	deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

	while (clawt_broker_get_state(fixture.daemon->broker) !=
	       CLAWT_BROKER_STATE_ERROR &&
	       g_get_monotonic_time() < deadline)
		g_main_context_iteration(fixture.context, TRUE);

	g_assert_cmpint(clawt_broker_get_state(fixture.daemon->broker), ==,
	                CLAWT_BROKER_STATE_ERROR);
	g_assert_nonnull(clawt_broker_get_detail(fixture.daemon->broker));
	g_assert_nonnull(strstr(clawt_broker_get_detail(fixture.daemon->broker),
	                        "clawt-broker-anythingmcp-db"));

	status = ask(&fixture, "broker.status", NULL);
	g_assert_cmpstr(json_object_get_string_member(payload_of(status),
	                                              "state"), ==, "error");

	/* The broker's connector is listed like any other connector... */
	listed = ask(&fixture, "connector.list", NULL);
	g_assert_true(reply_ok(listed));
	g_assert_nonnull(strstr(json_object_get_string_member(
		json_array_get_object_element(
			json_object_get_array_member(payload_of(listed), "connectors"),
			0), "name"), "broker-agent-scout"));

	/* ...and an edit that would be saved nowhere is refused, naming why. */
	edit = ask(&fixture, "integration.update",
	           "{\"name\":\"broker-agent-scout\",\"url\":\"http://x\"}");
	g_assert_false(reply_ok(edit));
	g_assert_nonnull(strstr(error_message(edit), "broker_server"));

	keyed = ask(&fixture, "connector.key",
	            "{\"name\":\"broker-agent-scout\",\"key\":\"k\"}");
	g_assert_false(reply_ok(keyed));
	g_assert_nonnull(strstr(error_message(keyed), "clawtilla broker key"));

	daemon_teardown(&fixture);
}

/* ── Logs and the watch, against a canned podman ─────────────────── */

/*
 * A podman API socket that answers from a script: each request is one
 * connection, routed on its path.  Real podomation container module on
 * the other end, so what is asserted is what `clawtilla broker logs` and
 * the watch would see -- with no podman on the machine.
 */
typedef struct {
	gchar           *dir;
	gchar           *socket;
	gchar           *uri;
	GSocketListener *listener;
	GCancellable    *cancel;
	GThread         *thread;
} FakePodman;

/* One Docker log frame: stream id, three bytes of padding, length. */
static void
log_frame(
	GByteArray  *buf,
	guint8       kind,
	const gchar *payload
){
	guint32 len = (guint32)strlen(payload);
	guint8 header[8];

	header[0] = kind;
	header[1] = 0;
	header[2] = 0;
	header[3] = 0;
	header[4] = (guint8)((len >> 24) & 0xff);
	header[5] = (guint8)((len >> 16) & 0xff);
	header[6] = (guint8)((len >> 8) & 0xff);
	header[7] = (guint8)(len & 0xff);

	g_byte_array_append(buf, header, 8);
	g_byte_array_append(buf, (const guint8 *)payload, len);
}

/* What the canned podman says to @path. */
static GBytes *
fake_podman_answer(
	const gchar *path
){
	g_autoptr(GByteArray) body = g_byte_array_new();
	g_autofree gchar *head = NULL;
	gint status = 200;
	const gchar *json = NULL;

	if (strstr(path, "/containers/gw/logs?") != NULL) {
		log_frame(body, 1, "listening on :3000\n");
		log_frame(body, 2, "warn: Authorization: Bearer sk-ant-api03-"
		                   "abcdefghijklmnopqrstuvwxyz0123456789\n");
		log_frame(body, 1, "GET /health 200\n");
	} else if (strstr(path, "/containers/db/logs?") != NULL) {
		log_frame(body, 2, "database system is ready\n");
	} else if (strstr(path, "/containers/quiet/logs?") != NULL) {
		/* Nothing logged yet: an empty, successful body. */
	} else if (strstr(path, "/containers/run1/json") != NULL) {
		json = "{\"Id\":\"run1\",\"State\":{\"Status\":\"running\",\"Pid\":7}}";
	} else if (strstr(path, "/containers/exit1/json") != NULL) {
		json = "{\"Id\":\"exit1\",\"State\":{\"Status\":\"exited\",\"Pid\":0}}";
	} else if (strstr(path, "/containers/clean1/json") != NULL) {
		json = "{\"Id\":\"clean1\",\"State\":{\"Status\":\"exited\",\"Pid\":0,"
		       "\"ExitCode\":0,\"OOMKilled\":false}}";
	} else if (strstr(path, "/containers/fail1/json") != NULL) {
		json = "{\"Id\":\"fail1\",\"State\":{\"Status\":\"exited\",\"Pid\":0,"
		       "\"ExitCode\":3,\"OOMKilled\":false}}";
	} else if (strstr(path, "/containers/oom1/json") != NULL) {
		json = "{\"Id\":\"oom1\",\"State\":{\"Status\":\"exited\",\"Pid\":0,"
		       "\"ExitCode\":137,\"OOMKilled\":true}}";
	} else if (strstr(path, "/containers/oomzero1/json") != NULL) {
		/* OOMKilled with a zero code: the flag alone is a failure. */
		json = "{\"Id\":\"oomzero1\",\"State\":{\"Status\":\"stopped\","
		       "\"Pid\":0,\"ExitCode\":0,\"OOMKilled\":true}}";
	} else if (strstr(path, "/containers/kill1/json") != NULL) {
		json = "{\"Id\":\"kill1\",\"State\":{\"Status\":\"exited\",\"Pid\":0,"
		       "\"ExitCode\":137,\"OOMKilled\":false}}";
	} else if (strstr(path, "/containers/made1/json") != NULL) {
		json = "{\"Id\":\"made1\",\"State\":{\"Status\":\"created\",\"Pid\":0,"
		       "\"ExitCode\":0,\"OOMKilled\":false}}";
	} else if (strstr(path, "/containers/blank1/json") != NULL) {
		json = "{\"Id\":\"blank1\"}";
	} else if (strstr(path, "/containers/json") != NULL) {
		json = "[]";
	} else {
		status = 404;
		json = "{\"cause\":\"no such container\","
		       "\"message\":\"no container with name or ID found\","
		       "\"response\":404}";
	}

	if (json != NULL)
		g_byte_array_append(body, (const guint8 *)json, (guint)strlen(json));

	head = g_strdup_printf("HTTP/1.1 %d X\r\n"
	                       "Content-Length: %u\r\n"
	                       "Connection: close\r\n\r\n",
	                       status, body->len);
	g_byte_array_prepend(body, (const guint8 *)head, (guint)strlen(head));

	return g_byte_array_free_to_bytes(g_steal_pointer(&body));
}

static gpointer
fake_podman_serve(
	gpointer data
){
	FakePodman *fake = data;

	for (;;) {
		g_autoptr(GSocketConnection) conn = NULL;
		g_autoptr(GDataInputStream) in = NULL;
		g_autoptr(GBytes) answer = NULL;
		g_autofree gchar *request = NULL;
		g_auto(GStrv) words = NULL;
		gchar *line;
		gsize size;
		gconstpointer bytes;

		conn = g_socket_listener_accept(fake->listener, NULL, fake->cancel,
		                                NULL);
		if (conn == NULL)
			break;

		in = g_data_input_stream_new(
			g_io_stream_get_input_stream(G_IO_STREAM(conn)));
		g_data_input_stream_set_newline_type(in,
		                                     G_DATA_STREAM_NEWLINE_TYPE_ANY);
		g_filter_input_stream_set_close_base_stream(
			G_FILTER_INPUT_STREAM(in), FALSE);

		/* Read to the blank line: unread input resets the connection. */
		while ((line = g_data_input_stream_read_line(in, NULL, NULL,
		                                             NULL)) != NULL) {
			gboolean blank = *line == '\0';

			if (request == NULL)
				request = g_strdup(line);
			g_free(line);
			if (blank)
				break;
		}

		words = g_strsplit(request != NULL ? request : "", " ", 3);
		answer = fake_podman_answer(words[0] != NULL && words[1] != NULL
		                            ? words[1] : "");
		bytes = g_bytes_get_data(answer, &size);
		g_output_stream_write_all(
			g_io_stream_get_output_stream(G_IO_STREAM(conn)), bytes, size,
			NULL, NULL, NULL);
		g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
	}

	return NULL;
}

static FakePodman *
fake_podman_start(
	void
){
	FakePodman *fake = g_new0(FakePodman, 1);
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GError) error = NULL;

	fake->dir = g_dir_make_tmp("clawt-broker-podman-XXXXXX", NULL);
	g_assert_nonnull(fake->dir);
	fake->socket = g_build_filename(fake->dir, "podman.sock", NULL);
	fake->uri = g_strdup_printf("unix://%s", fake->socket);
	fake->cancel = g_cancellable_new();
	fake->listener = g_socket_listener_new();
	address = g_unix_socket_address_new(fake->socket);
	g_socket_listener_add_address(fake->listener, address,
	                              G_SOCKET_TYPE_STREAM,
	                              G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
	                              &error);
	g_assert_no_error(error);
	fake->thread = g_thread_new("fake-podman", fake_podman_serve, fake);

	return fake;
}

static void
fake_podman_stop(
	FakePodman *fake
){
	g_cancellable_cancel(fake->cancel);
	g_thread_join(fake->thread);
	g_socket_listener_close(fake->listener);
	g_object_unref(fake->listener);
	g_object_unref(fake->cancel);
	clawt_test_remove_tree(fake->dir);
	g_free(fake->dir);
	g_free(fake->socket);
	g_free(fake->uri);
	g_free(fake);
}

/*
 * A bridge with the container module loaded for @uri, or %NULL with the
 * test skipped where the module was never built.  The deps are built release whatever
 * build type this is, so this directory serves both trees.
 */
static ClawtPodBridge *
container_bridge(
	const gchar *uri
){
	g_autofree gchar *module_dir = g_build_filename(
		CLAWT_TEST_SRCDIR, "deps", "libreclaw", "build", "release",
		"modules", NULL);
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(module_dir);
	g_autoptr(GError) error = NULL;

	/*
	 * Loaded for the fake's socket, never the default connection: that
	 * would reach for this machine's own podman.
	 */
	if (!clawt_pod_bridge_load_module_for(bridge, "container", uri,
	                                      &error)) {
		g_test_skip(error->message);
		return NULL;
	}

	return g_steal_pointer(&bridge);
}

/*
 * `broker logs` prints the log.  podomation's logs action parsed the
 * multiplexed log stream as JSON and returned no text for any container,
 * so this used to print a heading per container and a line naming
 * `podman logs`.
 */
static void
test_logs_print_the_text(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	FakePodman *fake;
	const gchar *names[] = { "gw", "db", "quiet", "gone", NULL };

	fake = fake_podman_start();
	bridge = container_bridge(fake->uri);

	if (bridge == NULL) {
		fake_podman_stop(fake);
		return;
	}

	text = clawt_broker_read_logs(bridge, fake->uri, names, NULL, &error);
	fake_podman_stop(fake);

	g_assert_no_error(error);
	g_assert_nonnull(text);

	/* Both streams: each under its own heading. */
	g_assert_nonnull(strstr(text,
		"== gw ==\n"
		"-- stdout --\n"
		"listening on :3000\n"
		"GET /health 200\n"
		"-- stderr --\n"
		"warn: Authorization: "));

	/* A credential in the log is not shown. */
	g_assert_null(strstr(text, "abcdefghijklmnopqrstuvwxyz0123456789"));

	/* One stream: bare. */
	g_assert_nonnull(strstr(text,
		"== db ==\ndatabase system is ready\n== quiet ==\n"));

	/* Nothing yet says so; a container podman cannot read says why. */
	g_assert_nonnull(strstr(text, "== quiet ==\n(no lines yet)\n"));
	g_assert_nonnull(strstr(text, "== gone ==\n(no logs: "));
	g_assert_nonnull(strstr(text, "no container with name or ID found"));

	/* The workaround is gone with the bug. */
	g_assert_null(strstr(text, "podman logs"));
}

/* The watch reads inspect's state, and tells gone from unanswered. */
static void
test_watch_reads_inspect(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtBrokerExit) stopped = NULL;
	g_autofree gchar *nowhere = NULL;
	g_autofree gchar *empty_dir = NULL;
	FakePodman *fake;
	const gchar *up_ids[] = { "run1", "run1", NULL };
	const gchar *up_names[] = { "gateway", "database", NULL };
	const gchar *exit_ids[] = { "run1", "exit1", NULL };
	const gchar *gone_ids[] = { "run1", "gone1", NULL };
	const gchar *blank_ids[] = { "blank1", NULL };
	const gchar *unmade_ids[] = { "run1", "", NULL };

	fake = fake_podman_start();
	bridge = container_bridge(fake->uri);

	if (bridge == NULL) {
		fake_podman_stop(fake);
		return;
	}

	/* Every container running: nothing stopped. */
	g_assert_true(clawt_broker_find_stopped(bridge, fake->uri, up_ids,
	                                        up_names, &stopped, &error));
	g_assert_no_error(error);
	g_assert_null(stopped);

	/* An exited one is named. */
	g_assert_true(clawt_broker_find_stopped(bridge, fake->uri, exit_ids,
	                                        up_names, &stopped, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(stopped->name, ==, "database");
	g_clear_pointer(&stopped, clawt_broker_exit_free);

	/* A 404 while podman answers its list: the container is gone. */
	g_assert_true(clawt_broker_find_stopped(bridge, fake->uri, gone_ids,
	                                        up_names, &stopped, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(stopped->name, ==, "database");
	g_assert_true(stopped->gone);
	g_clear_pointer(&stopped, clawt_broker_exit_free);

	/* Never made: stopped, without asking. */
	g_assert_true(clawt_broker_find_stopped(bridge, fake->uri, unmade_ids,
	                                        up_names, &stopped, &error));
	g_assert_cmpstr(stopped->name, ==, "database");
	g_assert_true(stopped->never_made);
	g_clear_pointer(&stopped, clawt_broker_exit_free);

	/* No state at all is "did not say", not "stopped". */
	g_assert_false(clawt_broker_find_stopped(bridge, fake->uri, blank_ids,
	                                         up_names, &stopped, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
	g_assert_nonnull(strstr(error->message, "gave no state"));
	g_assert_null(stopped);
	g_clear_error(&error);

	fake_podman_stop(fake);

	/*
	 * No podman: not answered, and nothing is called stopped.  A socket
	 * path inside a directory of its own, so nothing on the machine can
	 * be listening there.
	 */
	empty_dir = g_dir_make_tmp("clawt-broker-nopodman-XXXXXX", NULL);
	g_assert_nonnull(empty_dir);
	nowhere = g_strdup_printf("unix://%s/no-podman.sock", empty_dir);
	g_assert_false(clawt_broker_find_stopped(bridge, nowhere, up_ids,
	                                         up_names, &stopped, &error));
	g_assert_nonnull(error);
	g_assert_null(stopped);
	clawt_test_remove_tree(empty_dir);
}

/*
 * broker.restart against what podman says, case by case.  Each container
 * is inspected through the real podomation container module on a canned
 * socket, and each policy is asked what it would do with the answer.
 * on-failure used to behave as always, because inspect did not say how
 * a container ended; it restarts a non-zero exit or an OOM kill now, and
 * nothing else.
 */
static void
test_restart_policy_per_exit(
	void
){
	static const struct {
		const gchar *id;
		gboolean     gone;
		gboolean     never_made;
		gint         exit_code;
		gboolean     oom_killed;
		gboolean     never;
		gboolean     on_failure;
		gboolean     always;
		const gchar *how;
	} cases[] = {
		/* id         gone   unmade code  oom    never  on-fail always */
		{ "clean1",   FALSE, FALSE,   0,  FALSE, FALSE, FALSE, TRUE,
		  "exited cleanly (exit code 0)" },
		{ "fail1",    FALSE, FALSE,   3,  FALSE, FALSE, TRUE,  TRUE,
		  "exited with code 3" },
		{ "oom1",     FALSE, FALSE, 137,  TRUE,  FALSE, TRUE,  TRUE,
		  "was killed for running out of memory (exit code 137)" },
		{ "oomzero1", FALSE, FALSE,   0,  TRUE,  FALSE, TRUE,  TRUE,
		  "was killed for running out of memory (exit code 0)" },
		{ "kill1",    FALSE, FALSE, 137,  FALSE, FALSE, TRUE,  TRUE,
		  "exited with code 137" },
		{ "made1",    FALSE, FALSE,   0,  FALSE, FALSE, FALSE, TRUE,
		  "never ran (podman says created)" },
		{ "gone1",    TRUE,  FALSE,   0,  FALSE, FALSE, FALSE, TRUE,
		  "is gone from podman" },
		{ "",         FALSE, TRUE,    0,  FALSE, FALSE, FALSE, TRUE,
		  "was never made" }
	};
	g_autoptr(ClawtPodBridge) bridge = NULL;
	FakePodman *fake;
	guint i;

	fake = fake_podman_start();
	bridge = container_bridge(fake->uri);

	if (bridge == NULL) {
		fake_podman_stop(fake);
		return;
	}

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GError) error = NULL;
		g_autoptr(ClawtBrokerExit) stopped = NULL;
		g_autofree gchar *how = NULL;
		g_autofree gchar *never = NULL;
		g_autofree gchar *on_failure = NULL;
		g_autofree gchar *always = NULL;
		const gchar *ids[] = { "run1", NULL, NULL };
		const gchar *names[] = { "gateway", "database", NULL };

		ids[1] = cases[i].id;
		g_test_message("case %s", cases[i].id);

		/* What podman says, through the module. */
		g_assert_true(clawt_broker_find_stopped(bridge, fake->uri, ids,
		                                        names, &stopped, &error));
		g_assert_no_error(error);
		g_assert_nonnull(stopped);
		g_assert_cmpstr(stopped->name, ==, "database");
		g_assert_cmpint(stopped->gone, ==, cases[i].gone);
		g_assert_cmpint(stopped->never_made, ==, cases[i].never_made);

		if (!cases[i].gone && !cases[i].never_made) {
			g_assert_true(stopped->exit_known);
			g_assert_cmpint(stopped->exit_code, ==, cases[i].exit_code);
			g_assert_cmpint(stopped->oom_killed, ==, cases[i].oom_killed);
		}

		how = clawt_broker_exit_describe(stopped);
		g_assert_cmpstr(how, ==, cases[i].how);
		g_assert_cmpint(clawt_broker_exit_is_failure(stopped), ==,
		                cases[i].on_failure);

		/* What each policy does with it. */
		g_assert_cmpint(clawt_broker_should_restart(CLAWT_RESTART_NEVER,
		                                            stopped, &never),
		                ==, cases[i].never);
		g_assert_cmpint(clawt_broker_should_restart(
		                    CLAWT_RESTART_ON_FAILURE, stopped, &on_failure),
		                ==, cases[i].on_failure);
		g_assert_cmpint(clawt_broker_should_restart(CLAWT_RESTART_ALWAYS,
		                                            stopped, &always),
		                ==, cases[i].always);

		/* A restart needs no reason; a refusal always gives one. */
		g_assert_null(always);
		g_assert_nonnull(never);
		g_assert_nonnull(strstr(never, "database "));
		g_assert_nonnull(strstr(never, cases[i].how));
		g_assert_nonnull(strstr(never, "broker.restart is never"));

		if (cases[i].on_failure) {
			g_assert_null(on_failure);
		} else {
			g_assert_nonnull(on_failure);
			g_assert_nonnull(strstr(on_failure, cases[i].how));
			g_assert_nonnull(strstr(on_failure,
			                        "broker.restart is on-failure"));
			g_assert_nonnull(strstr(on_failure, "not restarted"));
		}
	}

	fake_podman_stop(fake);
}

/*
 * A container module older than podomation 1.0.3 says the container
 * stopped and not how.  on-failure cannot call that a failure, so it
 * does not restart, and says that the module is why.
 */
static void
test_restart_policy_without_exit(
	void
){
	ClawtBrokerExit ended = { 0 };
	g_autofree gchar *reason = NULL;
	g_autofree gchar *how = NULL;

	ended.name = (gchar *)"gateway";
	ended.status = (gchar *)"exited";

	how = clawt_broker_exit_describe(&ended);
	g_assert_nonnull(strstr(how, "podman says exited"));
	g_assert_false(clawt_broker_exit_is_failure(&ended));

	g_assert_true(clawt_broker_should_restart(CLAWT_RESTART_ALWAYS, &ended,
	                                          NULL));
	g_assert_false(clawt_broker_should_restart(CLAWT_RESTART_ON_FAILURE,
	                                           &ended, &reason));
	g_assert_nonnull(strstr(reason, "podomation 1.0.3"));
	g_clear_pointer(&reason, g_free);
	g_assert_false(clawt_broker_should_restart(CLAWT_RESTART_NEVER, &ended,
	                                           &reason));
	g_assert_nonnull(strstr(reason, "broker.restart is never"));
}

/* Iterates @fixture until the broker is failed, or twenty seconds pass. */
static void
wait_for_error(
	DaemonFixture *fixture
){
	gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

	while (clawt_broker_get_state(fixture->daemon->broker) !=
	       CLAWT_BROKER_STATE_ERROR &&
	       g_get_monotonic_time() < deadline)
		g_main_context_iteration(fixture->context, TRUE);

	g_assert_cmpint(clawt_broker_get_state(fixture->daemon->broker), ==,
	                CLAWT_BROKER_STATE_ERROR);
}

/*
 * What the watch does with a stopped container, and what broker.status
 * says about it, for each policy.  A restart begins a start (which fails
 * here: there is no podman); a container left down leaves the broker
 * failed with the reason in its detail and in last_exit.
 */
static void
check_policy_in_status(
	const gchar *policy,
	gboolean     restarts_clean,
	gboolean     restarts_failure
){
	DaemonFixture fixture = { 0 };
	g_autofree gchar *broker = NULL;
	ClawtBrokerExit clean = { 0 };
	ClawtBrokerExit failed = { 0 };
	ClawtBrokerExit *each[2];
	gboolean expect[2];
	guint restarts = 0;
	guint i;

	clean.name = (gchar *)"clawt-broker-anythingmcp";
	clean.status = (gchar *)"exited";
	clean.exit_known = TRUE;
	failed = clean;
	failed.exit_code = 2;
	each[0] = &clean;
	each[1] = &failed;
	expect[0] = restarts_clean;
	expect[1] = restarts_failure;

	broker = g_strdup_printf("  enabled: true\n  restart: %s\n", policy);
	daemon_setup(&fixture, broker);

	/* No podman: the first start fails, which is where each case begins. */
	wait_for_error(&fixture);

	for (i = 0; i < G_N_ELEMENTS(each); i++) {
		g_autoptr(JsonNode) status = NULL;
		JsonObject *root;
		JsonObject *last;
		const gchar *detail;

		g_test_message("%s, exit code %d", policy, each[i]->exit_code);
		clawt_broker_test_handle_exit(fixture.daemon->broker, each[i]);

		status = ask(&fixture, "broker.status", NULL);
		root = payload_of(status);
		g_assert_true(json_object_has_member(root, "last_exit"));
		last = json_object_get_object_member(root, "last_exit");
		g_assert_cmpstr(json_object_get_string_member(last, "container"),
		                ==, "clawt-broker-anythingmcp");
		g_assert_cmpint(json_object_get_int_member(last, "exit_code"), ==,
		                each[i]->exit_code);
		g_assert_false(json_object_get_boolean_member(last, "oom_killed"));
		g_assert_cmpint(json_object_get_boolean_member(last, "failure"), ==,
		                each[i]->exit_code != 0);
		g_assert_cmpint(json_object_get_boolean_member(last, "restarted"),
		                ==, expect[i]);

		if (expect[i]) {
			/* Started again: counted, and no longer failed. */
			restarts++;
			g_assert_cmpint(clawt_broker_get_state(fixture.daemon->broker),
			                ==, CLAWT_BROKER_STATE_STARTING);
			g_assert_cmpuint(clawt_broker_get_restarts(
			                     fixture.daemon->broker), ==, restarts);
			wait_for_error(&fixture);
			continue;
		}

		/* Left down: failed, and the detail says why. */
		g_assert_cmpint(clawt_broker_get_state(fixture.daemon->broker), ==,
		                CLAWT_BROKER_STATE_ERROR);
		g_assert_cmpuint(clawt_broker_get_restarts(fixture.daemon->broker),
		                 ==, restarts);
		detail = json_object_get_string_member(root, "detail");
		g_assert_nonnull(detail);
		g_assert_nonnull(strstr(detail, "clawt-broker-anythingmcp "));
		g_assert_nonnull(strstr(detail, "not restarted"));
		g_assert_nonnull(strstr(detail, policy));
		g_assert_nonnull(strstr(detail, each[i]->exit_code == 0
		                                ? "exited cleanly (exit code 0)"
		                                : "exited with code 2"));
	}

	daemon_teardown(&fixture);
}

static void
test_status_policy_never(
	void
){
	check_policy_in_status("never", FALSE, FALSE);
}

static void
test_status_policy_on_failure(
	void
){
	check_policy_in_status("on-failure", FALSE, TRUE);
}

static void
test_status_policy_always(
	void
){
	check_policy_in_status("always", TRUE, TRUE);
}

static void
test_container_states(
	void
){
	g_assert_true(clawt_broker_container_is_up("running"));
	g_assert_true(clawt_broker_container_is_up("paused"));
	g_assert_true(clawt_broker_container_is_up("stopping"));
	g_assert_false(clawt_broker_container_is_up("exited"));
	g_assert_false(clawt_broker_container_is_up("stopped"));
	g_assert_false(clawt_broker_container_is_up("created"));
	g_assert_false(clawt_broker_container_is_up("dead"));
	g_assert_false(clawt_broker_container_is_up(""));
	g_assert_false(clawt_broker_container_is_up(NULL));
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/broker/image-pinned", test_image_must_be_pinned);
	g_test_add_func("/broker/spec/anythingmcp", test_anythingmcp_spec);
	g_test_add_func("/broker/spec/jentic", test_jentic_spec);
	g_test_add_func("/broker/spec/bad-port", test_bad_port_refused);
	g_test_add_func("/broker/state/keeps-secrets",
	                test_prepare_keeps_secrets);
	g_test_add_func("/broker/assignments", test_assignments_and_connectors);
	g_test_add_func("/broker/assignments/off", test_off_makes_no_connector);
	g_test_add_func("/broker/assignments/server-id",
	                test_server_id_is_checked);
	g_test_add_func("/broker/jentic/confined-only",
	                test_jentic_needs_confinement);
	g_test_add_func("/broker/names", test_names_and_paths);
	g_test_add_func("/broker/relay/refusals", test_refusals_name_the_gateway);
	g_test_add_func("/broker/relay/annotate", test_errors_are_annotated);
	g_test_add_func("/broker/relay/reason", test_reason_from_body);
	g_test_add_func("/broker/relay/plan", test_relay_plan_names_the_gateway);
	g_test_add_func("/broker/status/no-secret", test_status_carries_no_secret);
	g_test_add_func("/broker/daemon/off", test_daemon_off);
	g_test_add_func("/broker/daemon/on-without-podman",
	                test_daemon_on_without_podman);
	g_test_add_func("/broker/logs/text", test_logs_print_the_text);
	g_test_add_func("/broker/watch/inspect", test_watch_reads_inspect);
	g_test_add_func("/broker/watch/states", test_container_states);
	g_test_add_func("/broker/watch/restart-per-exit",
	                test_restart_policy_per_exit);
	g_test_add_func("/broker/watch/restart-without-exit",
	                test_restart_policy_without_exit);
	g_test_add_func("/broker/watch/status/never", test_status_policy_never);
	g_test_add_func("/broker/watch/status/on-failure",
	                test_status_policy_on_failure);
	g_test_add_func("/broker/watch/status/always",
	                test_status_policy_always);

	return g_test_run();
}
