/*
 * test-mcp-app-call.c - An MCP app calling its own server's tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An app's view may call a tool on the server that shipped it, and only
 * one the server lists as visible to apps.  These hold down each wall:
 * the relay turned around for an app's caller refuses the model's own
 * tools; the daemon's client refuses them before anything is sent, and
 * says why; the daemon's frame refuses an app whose connector stopped
 * showing apps and a confined agent, and records every call in the
 * conversation as the app's; and policy.check asked for an app obeys
 * the agent's action rules without counting against a turn.  The relay
 * is a fake (tests/fixtures/fake-app-relay), so nothing leaves the
 * machine.
 */

#include <clawtilla.h>

#include <string.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

static JsonObject *
parse(
	const gchar *text
){
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_nonnull(text);
	g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));

	return json_object_ref(json_node_get_object(
		json_parser_get_root(parser)));
}

static gboolean
on_watchdog(
	gpointer data
){
	gboolean *fired = data;

	*fired = TRUE;
	return G_SOURCE_REMOVE;
}

/* ── Visibility ──────────────────────────────────────────────────── */

static void
test_app_visibility(
	void
){
	g_autoptr(JsonObject) both = parse("{\"name\":\"draw\"}");
	g_autoptr(JsonObject) app = parse("{\"name\":\"save\",\"_meta\":{\"ui\":"
		"{\"visibility\":[\"app\"]}}}");
	g_autoptr(JsonObject) model = parse("{\"name\":\"secret\",\"_meta\":{"
		"\"ui\":{\"visibility\":[\"model\"]}}}");
	g_autoptr(JsonObject) odd = parse("{\"name\":\"odd\",\"_meta\":{\"ui\":"
		"{\"visibility\":[7]}}}");

	g_assert_true(clawt_mcp_app_tool_app_visible(both));
	g_assert_true(clawt_mcp_app_tool_app_visible(app));
	g_assert_false(clawt_mcp_app_tool_app_visible(model));
	g_assert_false(clawt_mcp_app_tool_app_visible(odd));
	g_assert_true(clawt_mcp_app_tool_model_visible(model));
	g_assert_false(clawt_mcp_app_tool_model_visible(app));
}

/* The relay turned around: the list whole, and the model's tools refused. */
static void
test_the_relay_for_an_app(
	void
){
	g_autoptr(ClawtMcpApps) apps = clawt_mcp_apps_new("draw", FALSE);
	g_autofree gchar *list = NULL;
	g_autofree gchar *to_server = NULL;
	g_autofree gchar *to_client = NULL;
	const gchar *calls[] = { "draw", "save_view", "secret", "unlisted",
	                         NULL };
	const gboolean allowed[] = { TRUE, TRUE, FALSE, FALSE };
	guint i;

	clawt_mcp_apps_set_app_caller(apps, TRUE);

	list = clawt_mcp_apps_inbound(apps,
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
		"{\"name\":\"draw\"},"
		"{\"name\":\"save_view\",\"_meta\":{\"ui\":{\"visibility\":"
		"[\"app\"]}}},"
		"{\"name\":\"secret\",\"_meta\":{\"ui\":{\"visibility\":"
		"[\"model\"]}}}]}}", &to_server);

	/* Passed whole: the app-only tool is not taken out for this caller. */
	g_assert_nonnull(strstr(list, "save_view"));
	g_assert_null(to_server);

	for (i = 0; calls[i] != NULL; i++) {
		g_autofree gchar *line = g_strdup_printf(
			"{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"tools/call\","
			"\"params\":{\"name\":\"%s\",\"arguments\":{}}}", 10 + i,
			calls[i]);

		g_clear_pointer(&to_server, g_free);
		g_clear_pointer(&to_client, g_free);
		clawt_mcp_apps_outbound(apps, line, &to_server, &to_client);

		if (allowed[i]) {
			g_assert_cmpstr(to_server, ==, line);
			g_assert_null(to_client);
		} else {
			g_assert_null(to_server);
			g_assert_nonnull(to_client);
			g_assert_nonnull(strstr(to_client, "apps call"));
		}
	}

	/* And the agent's relay still refuses the app's own tool to the model. */
	{
		g_autoptr(ClawtMcpApps) agent = clawt_mcp_apps_new("draw", FALSE);
		g_autofree gchar *seen = NULL;

		seen = clawt_mcp_apps_inbound(agent,
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
			"{\"name\":\"save_view\",\"_meta\":{\"ui\":{\"visibility\":"
			"[\"app\"]}}}]}}", &to_server);
		g_assert_null(strstr(seen, "save_view"));

		g_clear_pointer(&to_server, g_free);
		g_clear_pointer(&to_client, g_free);
		clawt_mcp_apps_outbound(agent, "{\"jsonrpc\":\"2.0\",\"id\":3,"
			"\"method\":\"tools/call\",\"params\":{\"name\":\"save_view\"}}",
			&to_server, &to_client);
		g_assert_null(to_server);
		g_assert_nonnull(strstr(to_client, "belongs to an app"));
	}
}

/* ── The daemon's client, against the fake relay ─────────────────── */

typedef struct {
	gboolean    done;
	JsonObject *result;
	GError     *error;
} Outcome;

static void
on_called(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Outcome *outcome = data;

	(void)source;
	outcome->result = clawt_mcp_app_call_finish(result, &outcome->error);
	outcome->done = TRUE;
}

static gchar *
fixture_relay(
	void
){
	return g_build_filename(CLAWT_TEST_FIXTURES, "fake-app-relay", NULL);
}

/* One call through the fake relay; @log gets every method it was sent. */
static void
call(
	const gchar *log,
	const gchar *mode,
	const gchar *tool,
	guint        timeout_seconds,
	Outcome     *outcome
){
	g_autofree gchar *relay = fixture_relay();
	const gchar *argv[] = { "bash", relay, log, mode, NULL };
	g_autoptr(JsonObject) arguments = parse("{\"shape\":\"box\"}");
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	gboolean fired = FALSE;
	guint watchdog;

	json_node_set_object(node, arguments);
	outcome->done = FALSE;
	watchdog = g_timeout_add_seconds(20, on_watchdog, &fired);

	clawt_mcp_app_call_async(argv, tool, node, timeout_seconds, NULL,
		on_called, outcome);

	/* Never synchronous: the caller's own call returns first. */
	g_assert_false(outcome->done);

	while (!outcome->done && !fired)
		g_main_context_iteration(NULL, TRUE);

	g_assert_false(fired);
	g_source_remove(watchdog);
}

static gboolean
log_has(
	const gchar *log,
	const gchar *line
){
	g_autofree gchar *contents = NULL;
	g_autofree gchar *needle = g_strconcat(line, "\n", NULL);

	if (!g_file_get_contents(log, &contents, NULL, NULL))
		return FALSE;

	return strstr(contents, needle) != NULL;
}

/* Lets the reaped relays go, so a later check is not racing them. */
static void
drain(
	void
){
	gboolean fired = FALSE;

	g_timeout_add(200, on_watchdog, &fired);
	while (!fired)
		g_main_context_iteration(NULL, TRUE);
}

static void
test_the_client(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-appcall-XXXXXX", NULL);
	g_autofree gchar *log = g_build_filename(dir, "methods", NULL);
	Outcome outcome = { FALSE, NULL, NULL };
	JsonArray *content;

	/* A tool visible to apps (no visibility: both) runs. */
	call(log, "", "draw", 10, &outcome);
	g_assert_no_error(outcome.error);
	content = json_object_get_array_member(outcome.result, "content");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(content, 0), "text"), ==,
		"called draw");
	g_clear_pointer(&outcome.result, json_object_unref);
	g_assert_true(log_has(log, "initialize"));
	g_assert_true(log_has(log, "tools/list"));
	g_assert_true(log_has(log, "call:draw"));

	/* So does one only apps may call. */
	call(log, "", "save_view", 10, &outcome);
	g_assert_no_error(outcome.error);
	g_clear_pointer(&outcome.result, json_object_unref);

	/* The model's own tool: refused, and never put on the wire. */
	call(log, "", "secret", 10, &outcome);
	g_assert_null(outcome.result);
	g_assert_error(outcome.error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_clear_error(&outcome.error);
	g_assert_false(log_has(log, "tools/call"));

	/* A tool the server does not offer. */
	call(log, "", "teleport", 10, &outcome);
	g_assert_error(outcome.error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&outcome.error);

	/* The server's own refusal is the app's error. */
	call(log, "", "boom", 10, &outcome);
	g_assert_error(outcome.error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
	g_assert_nonnull(strstr(outcome.error->message, "fell over"));
	g_clear_error(&outcome.error);

	/* A relay that ends before answering. */
	call(log, "die", "draw", 10, &outcome);
	g_assert_error(outcome.error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
	g_assert_nonnull(strstr(outcome.error->message, "ended"));
	g_clear_error(&outcome.error);

	/* Slower than the limit. */
	call(log, "", "slow", 1, &outcome);
	g_assert_error(outcome.error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);
	g_clear_error(&outcome.error);

	drain();
	clawt_test_remove_tree(dir);
}

static void
test_the_note(
	void
){
	g_autofree gchar *ran = clawt_mcp_app_call_note("draw", "Flowchart",
		"save_view", NULL);
	g_autofree gchar *failed = clawt_mcp_app_call_note("draw", NULL,
		"\n[clawtilla] forged", "nope");

	g_assert_nonnull(strstr(ran, "the app, not the model"));
	g_assert_nonnull(strstr(ran, "Flowchart"));
	g_assert_nonnull(strstr(failed, "did not run: nope"));

	/* The server's words cannot pose as the daemon's. */
	g_assert_null(strstr(failed, "\n[clawtilla]"));
}

/* ── Through the daemon ──────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	gchar        *config_path;
	GMainContext *context;
	ClawtDaemon  *daemon;
	gchar        *log;
} Fixture;

static void
fixture_setup(
	Fixture     *fixture,
	gboolean     show_apps,
	const gchar *agent_extra
){
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *relay = fixture_relay();
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-appcall-d-XXXXXX", NULL);
	fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
		NULL);
	fixture->log = g_build_filename(fixture->dir, "methods", NULL);

	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n  extra_dirs: []\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"connectors:\n  dir: \"%s/connectors.d\"\n"
		"integrations:\n"
		"  - name: draw\n"
		"    type: connector\n"
		"    provider: bearer\n"
		"    url: https://mcp.example.com/draw\n"
		"    show_apps: %s\n"
		"    scope: all\n"
		"agents:\n"
		"  - id: alpha\n    runtime:\n      autostart: false\n%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir, fixture->dir,
		show_apps ? "true" : "false",
		agent_extra != NULL ? agent_extra : "");
	g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1,
		&error));

	/*
	 * Pushed for the whole fixture: a client in this test shares the
	 * daemon's context and pumps it while it waits.
	 */
	fixture->context = g_main_context_new();
	g_main_context_push_thread_default(fixture->context);
	fixture->daemon = clawt_daemon_new(fixture->config_path,
		fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, &error));
	g_assert_no_error(error);

	/* The relay's command line, replaced by the fake's. */
	{
		const gchar *argv[] = { "bash", relay, fixture->log, NULL };

		fixture->daemon->app_call_command = g_strdupv((GStrv)argv);
	}
}

static void
fixture_teardown(
	Fixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_main_context_pop_thread_default(fixture->context);
	g_main_context_unref(fixture->context);

	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
	g_free(fixture->config_path);
	g_free(fixture->log);
}

/* An app the relay reported earlier, kept as the daemon keeps one. */
static void
keep_app(
	Fixture     *fixture,
	const gchar *app_id
){
	g_autofree gchar *dir = clawt_mcp_app_store_dir(
		clawt_daemon_get_config(fixture->daemon), "alpha");
	g_autoptr(JsonObject) record = parse("{\"connector\":\"draw\","
		"\"tool\":\"create_view\",\"title\":\"Flowchart\","
		"\"html\":\"<p>hi</p>\"}");
	g_autoptr(GError) error = NULL;

	g_assert_true(clawt_mcp_app_save(dir, app_id, record, &error));
	g_assert_no_error(error);
}

static JsonNode *
request(
	Fixture     *fixture,
	const gchar *kind,
	const gchar *payload_json
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "1");
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, payload_json, -1,
		NULL));
	clawt_ipc_frame_set_payload(frame,
		json_node_copy(json_parser_get_root(parser)));

	return clawt_daemon_handle_request(fixture->daemon, frame);
}

/* The last message in the operator's room with alpha. */
static gchar *
last_said(
	Fixture *fixture
){
	ClawtRoom *room = clawt_room_manager_get_direct(
		clawt_daemon_get_rooms(fixture->daemon), "user", "alpha");
	g_autoptr(GPtrArray) history = NULL;

	if (room == NULL)
		return NULL;

	history = clawt_room_get_history(room, 1);

	if (history == NULL || history->len == 0)
		return NULL;

	return g_strdup(clawt_message_get_body(g_ptr_array_index(history, 0)));
}

static void
wait_for_note(
	Fixture     *fixture,
	const gchar *needle
){
	gboolean fired = FALSE;
	guint watchdog = g_timeout_add_seconds(20, on_watchdog, &fired);

	while (!fired) {
		g_autofree gchar *said = last_said(fixture);

		if (said != NULL && strstr(said, needle) != NULL)
			break;

		g_main_context_iteration(fixture->context, FALSE);
		g_main_context_iteration(NULL, FALSE);
		g_usleep(2000);
	}

	g_assert_false(fired);
	g_source_remove(watchdog);
}

/* A payload a client sends. */
static JsonNode *
payload(
	const gchar *json
){
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));

	return json_node_copy(json_parser_get_root(parser));
}

/*
 * The frame runs the call through the relay, answers the client that
 * asked -- deferred, never by waiting in the handler -- and puts the
 * call in the conversation as the app's.
 */
static void
test_the_frame_runs_and_records(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(ClawtClient) client = NULL;
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *socket_path = NULL;
	JsonArray *content;

	fixture_setup(&fixture, TRUE, NULL);
	keep_app(&fixture, "a0123456789abcdef");

	socket_path = g_build_filename(fixture.dir, "d.sock", NULL);
	client = clawt_client_new(socket_path);
	g_assert_true(clawt_client_connect(client, &error));
	g_assert_no_error(error);

	reply = clawt_client_request_full(client, "connector.app_call",
		payload("{\"agent\":\"alpha\",\"app\":\"a0123456789abcdef\","
			"\"tool\":\"save_view\",\"arguments\":{\"shape\":\"box\"}}"),
		30, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reply);
	content = json_object_get_array_member(json_object_get_object_member(
		json_node_get_object(reply), "result"), "content");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(content, 0), "text"), ==,
		"called save_view");
	g_assert_true(log_has(fixture.log, "call:save_view"));

	wait_for_note(&fixture, "the app, not the model");
	{
		g_autofree gchar *said = last_said(&fixture);

		g_assert_nonnull(strstr(said, "Flowchart"));
		g_assert_nonnull(strstr(said, "save_view"));
	}

	/* The model's own tool: refused, said in the transcript, not called. */
	g_clear_pointer(&reply, json_node_unref);
	reply = clawt_client_request_full(client, "connector.app_call",
		payload("{\"agent\":\"alpha\",\"app\":\"a0123456789abcdef\","
			"\"tool\":\"secret\"}"), 30, &error);
	g_assert_null(reply);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "apps call"));
	g_clear_error(&error);
	wait_for_note(&fixture, "did not run");
	g_assert_false(log_has(fixture.log, "call:secret"));

	g_clear_object(&client);
	drain();
	fixture_teardown(&fixture);
}

/* Every refusal the frame makes before starting anything. */
static void
test_the_frame_refuses(
	void
){
	Fixture fixture = { 0 };
	Fixture off = { 0 };
	Fixture confined = { 0 };
	const gchar *bad[] = {
		"{\"agent\":\"nobody\",\"app\":\"a0123456789abcdef\","
		"\"tool\":\"draw\"}",
		"{\"agent\":\"alpha\",\"app\":\"../escape\",\"tool\":\"draw\"}",
		"{\"agent\":\"alpha\",\"app\":\"a0123456789abcdef\"}",
		"{\"agent\":\"alpha\",\"app\":\"affffffffffffffff\","
		"\"tool\":\"draw\"}",
		"{\"agent\":\"alpha\",\"app\":\"a0123456789abcdef\","
		"\"tool\":\"draw\",\"arguments\":[1,2]}",
		NULL
	};
	guint i;

	fixture_setup(&fixture, TRUE, NULL);
	keep_app(&fixture, "a0123456789abcdef");

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(JsonNode) reply = request(&fixture, "connector.app_call",
			bad[i]);

		g_assert_nonnull(reply);
		g_assert_true(clawt_ipc_frame_is_error(reply));
	}

	g_assert_false(g_file_test(fixture.log, G_FILE_TEST_EXISTS));
	fixture_teardown(&fixture);

	/* A connector whose apps were switched off since. */
	fixture_setup(&off, FALSE, NULL);
	keep_app(&off, "a0123456789abcdef");
	{
		g_autoptr(JsonNode) reply = request(&off, "connector.app_call",
			"{\"agent\":\"alpha\",\"app\":\"a0123456789abcdef\","
			"\"tool\":\"draw\"}");

		g_assert_true(clawt_ipc_frame_is_error(reply));
	}
	fixture_teardown(&off);

	/* A confined agent: the connector would run outside the box. */
	fixture_setup(&confined, TRUE, "    runtime:\n      confine: bwrap\n");
	keep_app(&confined, "a0123456789abcdef");
	{
		g_autoptr(JsonNode) reply = request(&confined, "connector.app_call",
			"{\"agent\":\"alpha\",\"app\":\"a0123456789abcdef\","
			"\"tool\":\"draw\"}");

		g_assert_true(clawt_ipc_frame_is_error(reply));
		g_assert_nonnull(strstr(json_object_get_string_member(
			json_node_get_object(reply), "error"), "confined"));
	}
	fixture_teardown(&confined);
}

/*
 * The gate an app's call goes through is the agent's: an action rule
 * that denies the tool denies it to the app, and the refusal names the
 * tool.
 */
static void
test_the_gate_is_the_agents(
	void
){
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) denied = NULL;
	g_autoptr(JsonNode) allowed = NULL;
	JsonObject *answer;

	fixture_setup(&fixture, TRUE,
		"    policy:\n      rules: [\"save_view=deny\"]\n");

	denied = request(&fixture, "policy.check", "{\"agent\":\"alpha\","
		"\"tool\":\"save_view\",\"class\":\"external\",\"origin\":\"app\","
		"\"summary\":\"save_view, asked by an app in chat (not the "
		"model)\"}");
	answer = clawt_ipc_frame_get_payload(denied);
	g_assert_false(json_object_get_boolean_member(answer, "allowed"));
	g_assert_nonnull(strstr(json_object_get_string_member(answer, "reason"),
		"save_view"));

	allowed = request(&fixture, "policy.check", "{\"agent\":\"alpha\","
		"\"tool\":\"draw\",\"class\":\"external\",\"origin\":\"app\"}");
	answer = clawt_ipc_frame_get_payload(allowed);
	g_assert_true(json_object_get_boolean_member(answer, "allowed"));

	fixture_teardown(&fixture);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/mcp-app-call/visibility", test_app_visibility);
	g_test_add_func("/mcp-app-call/relay", test_the_relay_for_an_app);
	g_test_add_func("/mcp-app-call/client", test_the_client);
	g_test_add_func("/mcp-app-call/note", test_the_note);
	g_test_add_func("/mcp-app-call/frame", test_the_frame_runs_and_records);
	g_test_add_func("/mcp-app-call/frame-refuses", test_the_frame_refuses);
	g_test_add_func("/mcp-app-call/gate", test_the_gate_is_the_agents);

	return g_test_run();
}
