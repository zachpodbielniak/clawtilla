/*
 * test-connector-accounts.c - Several accounts on one service
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every test here holds at least two accounts on one service, because
 * that is the case the feature exists for and "a test with one key
 * cannot see a shared-scalar bug".  The relay's rewriting is tested as
 * data; the daemon's refusal of an unlabelled second account and its
 * record of a 401 are tested through clawt_daemon_handle_request(), with
 * no socket.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ── Labels ──────────────────────────────────────────────────────── */

static void
test_slugs_fold(
	void
){
	g_autofree gchar *work = clawt_connector_account_slug("gh", "Work");
	g_autofree gchar *spaced = clawt_connector_account_slug("gh",
	                                                         " My  Work! ");
	g_autofree gchar *unlabelled = clawt_connector_account_slug("gh-home",
	                                                             NULL);
	g_autofree gchar *empty = clawt_connector_account_slug("gh", "");
	g_autofree gchar *nothing = clawt_connector_account_slug("!!", "--");

	g_assert_cmpstr(work, ==, "work");
	g_assert_cmpstr(spaced, ==, "my_work");
	g_assert_cmpstr(unlabelled, ==, "gh_home");
	g_assert_cmpstr(empty, ==, "gh");
	g_assert_cmpstr(nothing, ==, "account");
}

/* ── Bindings for one agent ──────────────────────────────────────── */

typedef struct {
	gchar       *dir;
	ClawtConfig *config;
	GPtrArray   *bindings;
} Agent;

static void
agent_load(
	Agent       *agent,
	const gchar *integrations
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	ClawtAgentConfig *scout;

	agent->dir = g_dir_make_tmp("clawt-accounts-XXXXXX", NULL);
	yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s/state\"\n"
		"defaults:\n"
		"  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: scout\n"
		"integrations:\n"
		"%s",
		agent->dir, agent->dir, integrations);

	agent->config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);

	scout = clawt_config_get_agent(agent->config, "scout");
	g_assert_nonnull(scout);

	agent->bindings = clawt_integration_resolve_for_agent(agent->config,
	                                                      scout);
}

static void
agent_free(
	Agent *agent
){
	g_clear_pointer(&agent->bindings, g_ptr_array_unref);
	g_clear_object(&agent->config);
	clawt_test_remove_tree(agent->dir);
	g_free(agent->dir);
}

static ClawtIntegrationBinding *
find(
	Agent       *agent,
	const gchar *name
){
	guint i;

	for (i = 0; i < agent->bindings->len; i++) {
		ClawtIntegrationBinding *binding = g_ptr_array_index(agent->bindings,
		                                                     i);

		if (g_strcmp0(clawt_integration_binding_get_name(binding), name) == 0)
			return binding;
	}

	g_assert_not_reached();
	return NULL;
}

#define TWO_GITHUBS \
	"  - name: gh-work\n" \
	"    type: connector\n" \
	"    provider: github\n" \
	"    account: work\n" \
	"    scope: all\n" \
	"  - name: gh-home\n" \
	"    type: connector\n" \
	"    provider: github\n" \
	"    account: Personal\n" \
	"    scope: all\n" \
	"  - name: notes\n" \
	"    type: connector\n" \
	"    provider: notion\n" \
	"    scope: all\n"

/*
 * What happens today, stated as a test: each account is its own relay,
 * so the CLI keeps the servers apart -- but both advertise the same tool
 * names.  Each account's prefix is its own, both relays reach the same
 * answer about the other, and a service with one account is untouched.
 */
static void
test_two_accounts_on_one_service_are_prefixed(
	void
){
	Agent agent = { 0 };
	g_autofree gchar *work = NULL;
	g_autofree gchar *home = NULL;
	g_autofree gchar *notes = NULL;
	g_auto(GStrv) work_siblings = NULL;
	g_auto(GStrv) home_siblings = NULL;
	g_autoptr(GError) error = NULL;

	agent_load(&agent, TWO_GITHUBS);

	work = clawt_connector_account_prefix(agent.bindings,
	                                      find(&agent, "gh-work"),
	                                      &work_siblings, &error);
	g_assert_no_error(error);
	home = clawt_connector_account_prefix(agent.bindings,
	                                      find(&agent, "gh-home"),
	                                      &home_siblings, &error);
	g_assert_no_error(error);
	notes = clawt_connector_account_prefix(agent.bindings,
	                                       find(&agent, "notes"), NULL,
	                                       &error);
	g_assert_no_error(error);

	g_assert_cmpstr(work, ==, "work");
	g_assert_cmpstr(home, ==, "personal");
	g_assert_null(notes);

	g_assert_cmpuint(g_strv_length(work_siblings), ==, 2);
	g_assert_true(g_strv_contains((const gchar * const *)work_siblings,
	                              "personal"));
	g_assert_true(g_strv_contains((const gchar * const *)home_siblings,
	                              "work"));

	agent_free(&agent);
}

/*
 * Two labels that fold to one prefix are refused on both sides -- never
 * routed to whichever account registered first.
 */
static void
test_colliding_labels_are_refused(
	void
){
	Agent agent = { 0 };
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autoptr(GError) first_error = NULL;
	g_autoptr(GError) second_error = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *forwarded = NULL;

	agent_load(&agent,
	           "  - name: gh-a\n"
	           "    type: connector\n"
	           "    provider: github\n"
	           "    account: Work\n"
	           "    scope: all\n"
	           "  - name: gh-b\n"
	           "    type: connector\n"
	           "    provider: github\n"
	           "    account: work!\n"
	           "    scope: all\n");

	first = clawt_connector_account_prefix(agent.bindings,
	                                       find(&agent, "gh-a"), NULL,
	                                       &first_error);
	second = clawt_connector_account_prefix(agent.bindings,
	                                        find(&agent, "gh-b"), NULL,
	                                        &second_error);

	g_assert_null(first);
	g_assert_null(second);
	g_assert_error(first_error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	g_assert_error(second_error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	g_assert_nonnull(strstr(first_error->message, "gh-b"));

	/* And the relay refuses every call on either, naming the reason. */
	forwarded = clawt_connector_accounts_outbound(
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"work_create_issue\"}}",
		NULL, NULL, first_error->message, &refusal);

	g_assert_null(forwarded);
	g_assert_nonnull(refusal);
	g_assert_nonnull(strstr(refusal, "Nothing ran"));
	g_assert_nonnull(strstr(refusal, "gh-b"));

	agent_free(&agent);
}

/* ── The relay's rewriting ───────────────────────────────────────── */

static void
test_tool_lists_are_renamed_per_account(
	void
){
	const gchar *listed =
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":["
		"{\"name\":\"create_issue\",\"description\":\"Open an issue\"},"
		"{\"name\":\"list_repos\"}]}}";
	g_autofree gchar *work = clawt_connector_accounts_inbound(listed, "work");
	g_autofree gchar *home = clawt_connector_accounts_inbound(listed,
	                                                           "personal");
	g_autofree gchar *other = clawt_connector_accounts_inbound(
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"content\":[]}}", "work");

	g_assert_nonnull(strstr(work, "\"work_create_issue\""));
	g_assert_nonnull(strstr(work, "\"work_list_repos\""));
	g_assert_nonnull(strstr(work, "[work account] Open an issue"));
	g_assert_null(strstr(work, "\"create_issue\""));

	g_assert_nonnull(strstr(home, "\"personal_create_issue\""));

	/* Two accounts, and no tool name the two lists share. */
	g_assert_null(strstr(home, "work_"));
	g_assert_null(strstr(work, "personal_"));

	/* Anything that is not a tool list passes untouched. */
	g_assert_null(strstr(other, "work"));
}

static void
test_a_prefixed_call_reaches_its_own_account(
	void
){
	gchar *siblings[] = { (gchar *)"work", (gchar *)"personal", NULL };
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *stripped = NULL;
	g_autofree gchar *untouched = NULL;

	stripped = clawt_connector_accounts_outbound(
		"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"work_create_issue\","
		"\"arguments\":{\"title\":\"x\"}}}",
		"work", siblings, NULL, &refusal);

	g_assert_null(refusal);
	g_assert_nonnull(stripped);
	g_assert_nonnull(strstr(stripped, "\"name\":\"create_issue\""));
	g_assert_nonnull(strstr(stripped, "\"title\":\"x\""));

	/* Everything that is not a tool call goes through as it came. */
	untouched = clawt_connector_accounts_outbound(
		"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}", "work",
		siblings, NULL, &refusal);
	g_assert_cmpstr(untouched, ==,
	                "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}");
}

/*
 * The bare name is the guess this exists to take away: run as this
 * account, it might have been meant for the other one.
 */
static void
test_an_unprefixed_call_is_refused(
	void
){
	gchar *siblings[] = { (gchar *)"work", (gchar *)"personal", NULL };
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *bare = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *quiet_refusal = NULL;
	g_autofree gchar *quiet = NULL;

	bare = clawt_connector_accounts_outbound(
		"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"create_issue\"}}",
		"work", siblings, NULL, &refusal);

	g_assert_null(bare);
	g_assert_nonnull(refusal);
	g_assert_nonnull(strstr(refusal, "\"id\":6"));
	g_assert_nonnull(strstr(refusal, "work, personal"));
	g_assert_nonnull(strstr(refusal, "work_create_issue"));
	g_clear_pointer(&refusal, g_free);

	/* The other account's prefix is not this account's either. */
	other = clawt_connector_accounts_outbound(
		"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"personal_create_issue\"}}",
		"work", siblings, NULL, &refusal);
	g_assert_null(other);
	g_assert_nonnull(refusal);

	/* A notification is dropped with nothing to answer into. */
	quiet = clawt_connector_accounts_outbound(
		"{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\","
		"\"params\":{\"name\":\"create_issue\"}}",
		"work", siblings, NULL, &quiet_refusal);
	g_assert_null(quiet);
	g_assert_null(quiet_refusal);
}

/* ── A remote plan with no credential ────────────────────────────── */

static void
test_a_remote_plan_needs_no_credential_yet(
	void
){
	Agent agent = { 0 };
	g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(NULL, NULL);
	g_autoptr(ClawtConnectorPlan) remote = NULL;
	g_autoptr(ClawtConnectorPlan) stdio = NULL;
	g_autoptr(GError) error = NULL;

	agent_load(&agent,
	           "  - name: notion\n"
	           "    type: connector\n"
	           "    provider: remote\n"
	           "    url: https://mcp.notion.example/mcp\n"
	           "    scope: all\n"
	           "  - name: gh\n"
	           "    type: connector\n"
	           "    provider: github\n"
	           "    scope: all\n");

	remote = clawt_connector_plan_new(
		clawt_connector_catalog_find(catalog, "remote"),
		find(&agent, "notion"), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(remote->url, ==, "https://mcp.notion.example/mcp");
	g_assert_cmpstr(remote->header_name, ==, "Authorization");
	g_assert_null(remote->header_value);
	g_assert_cmpstr(remote->name, ==, "notion");

	/* A stdio server would fail inside itself, naming neither. */
	stdio = clawt_connector_plan_new(
		clawt_connector_catalog_find(catalog, "github"),
		find(&agent, "gh"), NULL, &error);
	g_assert_null(stdio);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
	g_assert_nonnull(strstr(error->message, "clawtilla connector connect gh"));

	agent_free(&agent);
}

/* ── The daemon ──────────────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(
	DaemonFixture *fixture
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-accounts-daemon-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n  extra_dirs: []\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"connectors:\n  dir: \"%s/connectors.d\"\n"
		"agents:\n"
		"  - id: kudu\n    runtime:\n      autostart: false\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir, fixture->dir);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
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

/* The connector called @name in a connector.list reply. */
static JsonObject *
listed(
	JsonNode    *reply,
	const gchar *name
){
	JsonArray *connectors = json_object_get_array_member(payload_of(reply),
	                                                     "connectors");
	guint i;

	for (i = 0; i < json_array_get_length(connectors); i++) {
		JsonObject *entry = json_array_get_object_element(connectors, i);

		if (g_strcmp0(json_object_get_string_member(entry, "name"),
		              name) == 0)
			return entry;
	}

	return NULL;
}

static void
test_a_second_account_needs_its_own_label(
	void
){
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) unlabelled = NULL;
	g_autoptr(JsonNode) duplicate = NULL;
	g_autoptr(JsonNode) second = NULL;
	g_autoptr(JsonNode) other_service = NULL;
	g_autoptr(JsonNode) list = NULL;

	daemon_setup(&fixture);

	/* The first account may go without; nothing to tell it from yet. */
	first = ask(&fixture, "integration.add",
	            "{\"name\":\"gh\",\"type\":\"connector\","
	            "\"provider\":\"github\",\"scope\":\"all\"}");
	g_assert_true(reply_ok(first));

	unlabelled = ask(&fixture, "integration.add",
	                 "{\"name\":\"gh2\",\"type\":\"connector\","
	                 "\"provider\":\"github\",\"scope\":\"all\"}");
	g_assert_false(reply_ok(unlabelled));
	g_assert_nonnull(strstr(error_message(unlabelled), "account label"));
	g_assert_nonnull(strstr(error_message(unlabelled), "'gh'"));

	/* An unlabelled first account's label is its name. */
	duplicate = ask(&fixture, "integration.add",
	                "{\"name\":\"gh2\",\"type\":\"connector\","
	                "\"provider\":\"github\",\"account\":\"GH\","
	                "\"scope\":\"all\"}");
	g_assert_false(reply_ok(duplicate));
	g_assert_nonnull(strstr(error_message(duplicate), "already labelled"));

	second = ask(&fixture, "integration.add",
	             "{\"name\":\"gh2\",\"type\":\"connector\","
	             "\"provider\":\"github\",\"account\":\"personal\","
	             "\"scope\":\"all\"}");
	g_assert_true(reply_ok(second));

	/* Another service is not a second account of this one. */
	other_service = ask(&fixture, "integration.add",
	                    "{\"name\":\"gl\",\"type\":\"connector\","
	                    "\"provider\":\"gitlab\",\"scope\":\"all\"}");
	g_assert_true(reply_ok(other_service));

	/* Nothing refused was left behind in the configuration. */
	list = ask(&fixture, "connector.list", NULL);
	g_assert_true(reply_ok(list));
	g_assert_nonnull(listed(list, "gh"));
	g_assert_cmpstr(json_object_get_string_member(listed(list, "gh2"),
	                                              "account"), ==, "personal");
	g_assert_nonnull(listed(list, "gl"));

	daemon_teardown(&fixture);
}

/*
 * A URL is all a remote connector needs, and a 401 the relay reports
 * turns into a sign-in every client offers -- without the daemon going
 * to the network to record it.
 */
static void
test_a_401_offers_a_sign_in(
	void
){
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) added = NULL;
	g_autoptr(JsonNode) before = NULL;
	g_autoptr(JsonNode) told = NULL;
	g_autoptr(JsonNode) after = NULL;
	g_autoptr(JsonNode) unknown = NULL;
	g_autoptr(JsonNode) keyed = NULL;
	g_autoptr(JsonNode) discovered = NULL;
	JsonObject *entry;

	daemon_setup(&fixture);

	added = ask(&fixture, "integration.add",
	            "{\"name\":\"notion\",\"type\":\"connector\","
	            "\"url\":\"https://mcp.notion.example/mcp\","
	            "\"scope\":\"all\"}");
	g_assert_true(reply_ok(added));

	before = ask(&fixture, "connector.list", NULL);
	entry = listed(before, "notion");
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "provider"), ==,
	                CLAWT_CONNECTOR_PROVIDER_REMOTE);
	g_assert_true(json_object_get_boolean_member(entry, "remote"));
	g_assert_false(json_object_get_boolean_member(entry, "sign_in"));
	g_assert_false(json_object_get_boolean_member(entry, "needs_sign_in"));

	told = ask(&fixture, "connector.unauthorized",
	           "{\"name\":\"notion\",\"challenge\":\"Bearer "
	           "resource_metadata=\\\"https://mcp.notion.example/.well-known/"
	           "oauth-protected-resource\\\"\"}");
	g_assert_true(reply_ok(told));

	after = ask(&fixture, "connector.list", NULL);
	entry = listed(after, "notion");
	g_assert_true(json_object_get_boolean_member(entry, "sign_in"));
	g_assert_true(json_object_get_boolean_member(entry, "needs_sign_in"));

	unknown = ask(&fixture, "connector.unauthorized",
	              "{\"name\":\"nobody\"}");
	g_assert_false(reply_ok(unknown));

	/*
	 * A connector that takes a key has nothing to discover, and is told
	 * so as an answer rather than an error -- with no request made.
	 */
	keyed = ask(&fixture, "integration.add",
	            "{\"name\":\"books\",\"type\":\"connector\","
	            "\"provider\":\"venture\",\"scope\":\"all\"}");
	g_assert_true(reply_ok(keyed));

	discovered = ask(&fixture, "connector.discover", "{\"name\":\"books\"}");
	g_assert_true(reply_ok(discovered));
	g_assert_false(json_object_get_boolean_member(payload_of(discovered),
	                                              "sign_in"));

	daemon_teardown(&fixture);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/connector-accounts/slugs", test_slugs_fold);
	g_test_add_func("/connector-accounts/two-accounts-prefixed",
	                test_two_accounts_on_one_service_are_prefixed);
	g_test_add_func("/connector-accounts/colliding-labels-refused",
	                test_colliding_labels_are_refused);
	g_test_add_func("/connector-accounts/tool-lists-renamed",
	                test_tool_lists_are_renamed_per_account);
	g_test_add_func("/connector-accounts/prefixed-call-stripped",
	                test_a_prefixed_call_reaches_its_own_account);
	g_test_add_func("/connector-accounts/unprefixed-call-refused",
	                test_an_unprefixed_call_is_refused);
	g_test_add_func("/connector-accounts/remote-plan-without-credential",
	                test_a_remote_plan_needs_no_credential_yet);
	g_test_add_func("/connector-accounts/daemon/second-account-label",
	                test_a_second_account_needs_its_own_label);
	g_test_add_func("/connector-accounts/daemon/401-offers-sign-in",
	                test_a_401_offers_a_sign_in);

	return g_test_run();
}
