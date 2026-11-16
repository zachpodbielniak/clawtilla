/*
 * test-connector-confine-integration.c - A confined agent's connectors, for real
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Behind CLAWT_TEST_INTEGRATION=1, because it runs bubblewrap and opens
 * a listener on loopback.  The daemon starts an agent with
 * `runtime.confine: bwrap` on the open network; the agent is a stub
 * standing in for libreclaw, and inside its box it runs the connector
 * relay exactly as its AI CLI would -- the command and arguments the
 * daemon wrote into the workspace's .mcp.json -- against a fake MCP
 * server on this machine's loopback.
 *
 * What it proves, each against the box the daemon really built:
 *
 *   - the relay inside the box reads its connector's credential, and the
 *     server receives it as the bearer header (before the fix every
 *     credentialed connector failed here: "cannot read the credential");
 *   - a credential stored *after* the box was built -- a renewal, which
 *     the daemon writes by renaming a new file into place -- is the one
 *     the next relay sends, not the one the box started with;
 *   - another agent's connector credential, and the secrets directory
 *     itself, are not readable inside the box.
 *
 * And, on the same fake server, forward_one()'s empty-reply bug: a
 * reply the server sends must reach the client whole, so the relay's
 * output for each request carries the server's own text.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>

#include "clawt-test-util.h"

#define FIRST_TOKEN   ("granted-first-credential")
#define SECOND_TOKEN  ("granted-renewed-credential")
#define THEIR_TOKEN   ("theirs-credential-not-yours")

typedef struct {
	GPtrArray *authorizations;   /* every Authorization header seen */
} FakeServer;

static void
write_executable(
	const gchar *path,
	const gchar *text
){
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

/*
 * A streamable-HTTP MCP server that knows three requests: initialize,
 * tools/list and a tools/call of `ping`, which answers `pong`.  A
 * notification gets 202 and no body, as the protocol says.
 */
static void
on_mcp_request(
	SoupServer        *server,
	SoupServerMessage *message,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	FakeServer *fake = user_data;
	SoupMessageBody *body = soup_server_message_get_request_body(message);
	const gchar *authorization;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *reply = NULL;
	JsonObject *request;
	const gchar *method;
	gint64 id;

	authorization = soup_message_headers_get_one(
		soup_server_message_get_request_headers(message), "Authorization");
	g_ptr_array_add(fake->authorizations,
	                g_strdup(authorization != NULL ? authorization : "(none)"));

	if (body->data == NULL ||
	    !json_parser_load_from_data(parser, body->data, body->length, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		soup_server_message_set_status(message, 400, NULL);
		return;
	}

	request = json_node_get_object(json_parser_get_root(parser));
	method = json_object_get_string_member_with_default(request, "method",
	                                                    "");

	if (!json_object_has_member(request, "id")) {
		soup_server_message_set_status(message, 202, NULL);
		return;
	}

	id = json_object_get_int_member(request, "id");

	if (g_strcmp0(method, "initialize") == 0)
		reply = g_strdup_printf(
			"{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":"
			"{\"protocolVersion\":\"2025-06-18\",\"capabilities\":"
			"{\"tools\":{}},\"serverInfo\":{\"name\":\"fake\","
			"\"version\":\"1\"}}}", id);
	else if (g_strcmp0(method, "tools/list") == 0)
		reply = g_strdup_printf(
			"{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":"
			"{\"tools\":[{\"name\":\"ping\",\"description\":\"answers\","
			"\"inputSchema\":{\"type\":\"object\"}}]}}", id);
	else if (g_strcmp0(method, "tools/call") == 0)
		reply = g_strdup_printf(
			"{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":"
			"{\"content\":[{\"type\":\"text\",\"text\":\"pong-from-fake\"}]}}",
			id);
	else
		reply = g_strdup_printf(
			"{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"error\":"
			"{\"code\":-32601,\"message\":\"no such method\"}}", id);

	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "application/json",
	                                 SOUP_MEMORY_COPY, reply, strlen(reply));
}

/* Writes a token file the way the daemon does. */
static void
store_token(
	const gchar *path,
	const gchar *value
){
	ClawtOauthToken token = { 0 };
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;

	token.access_token = (gchar *)value;
	token.token_type = (gchar *)"Bearer";
	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	g_assert_true(clawt_oauth_token_save(&token, path, &error));
	g_assert_no_error(error);
}

/*
 * The relay's command line, from the .mcp.json the daemon wrote: the same
 * argv the AI CLI would start, quoted for the stub's shell.
 */
static gchar *
relay_command(
	const gchar *mcp_json,
	const gchar *server_name
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) out = g_string_new(NULL);
	g_autofree gchar *quoted = NULL;
	JsonObject *servers;
	JsonObject *server;
	JsonArray *args;
	guint i;

	g_assert_true(json_parser_load_from_file(parser, mcp_json, &error));
	g_assert_no_error(error);

	servers = json_object_get_object_member(
		json_node_get_object(json_parser_get_root(parser)), "mcpServers");
	g_assert_nonnull(servers);
	server = json_object_get_object_member(servers, server_name);
	g_assert_nonnull(server);

	quoted = g_shell_quote(json_object_get_string_member(server, "command"));
	g_string_append(out, quoted);
	args = json_object_get_array_member(server, "args");

	for (i = 0; args != NULL && i < json_array_get_length(args); i++) {
		g_autofree gchar *arg = g_shell_quote(
			json_array_get_string_element(args, i));

		g_string_append_printf(out, " %s", arg);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* Iterates @context until @path exists or @seconds pass. */
static gboolean
wait_for_file(
	GMainContext *context,
	const gchar  *path,
	guint         seconds
){
	gint64 deadline = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return TRUE;

		if (!g_main_context_iteration(context, FALSE))
			g_usleep(10 * 1000);
	}

	return g_file_test(path, G_FILE_TEST_EXISTS);
}

/*
 * One round: the stub runs run<round>.sh, which pipes three MCP messages
 * into the relay and keeps what comes back.
 */
static gchar *
run_round(
	GMainContext *context,
	const gchar  *workspace,
	const gchar  *command,
	guint         round
){
	g_autofree gchar *name = g_strdup_printf("run%u.sh", round);
	g_autofree gchar *script = g_build_filename(workspace, name, NULL);
	g_autofree gchar *out_name = g_strdup_printf("out%u", round);
	g_autofree gchar *out = g_build_filename(workspace, out_name, NULL);
	g_autofree gchar *body = NULL;
	g_autofree gchar *text = NULL;

	body = g_strdup_printf(
		"printf '%%s\\n' "
		"'{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
		"\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
		"\"clientInfo\":{\"name\":\"stub\",\"version\":\"1\"}}}' "
		"'{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}' "
		"'{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"ping\",\"arguments\":{}}}' "
		"| %s\n", command);
	g_assert_true(g_file_set_contents(script, body, -1, NULL));

	g_assert_true(wait_for_file(context, out, 60));
	g_assert_true(g_file_get_contents(out, &text, NULL, NULL));

	return g_steal_pointer(&text);
}

/*
 * One agent, one daemon.  @network is the agent's runtime.network; with
 * @allow the fake server's address is in egress.allow, so a gated relay
 * may reach it through the gate.
 */
static void
run_case(
	const gchar *network,
	gboolean     allow
){
	gboolean reaches = g_strcmp0(network, "open") == 0 || allow;
	g_autofree gchar *made = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *workspace = NULL;
	g_autofree gchar *other_workspace = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *secrets = NULL;
	g_autofree gchar *config_path = NULL;
	g_autofree gchar *stub = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *mcp_json = NULL;
	g_autofree gchar *command = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *probe = NULL;
	g_autofree gchar *probe_path = NULL;
	g_autofree gchar *granted_legacy = NULL;
	g_autofree gchar *granted_file = NULL;
	g_autofree gchar *their_file = NULL;
	g_autofree gchar *their_current = NULL;
	g_autofree gchar *path_env = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *allow_block = NULL;
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(ClawtDaemon) daemon = NULL;
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GSList) uris = NULL;
	FakeServer fake = { NULL };
	guint port;
	guint i;

	if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL) {
		g_test_skip("set CLAWT_TEST_INTEGRATION=1 to run a real bwrap box");
		return;
	}

	{
		g_autofree gchar *bwrap = g_find_program_in_path("bwrap");
		g_autofree gchar *cli = g_build_filename(BUILD_OUTDIR, "clawtilla",
		                                         NULL);
		g_autofree gchar *relay = g_build_filename(BUILD_OUTDIR,
		                                           "clawtilla-relay", NULL);

		if (bwrap == NULL || !g_file_test(cli, G_FILE_TEST_IS_EXECUTABLE) ||
		    !g_file_test(relay, G_FILE_TEST_IS_EXECUTABLE)) {
			g_test_skip("needs bubblewrap, and clawtilla and clawtilla-relay "
			            "built");
			return;
		}
	}

	/* Short, because the daemon's sockets live under it. */
	made = g_dir_make_tmp("ccc-XXXXXX", NULL);
	base = clawt_canonicalize_missing(made);
	workspace = g_build_filename(base, "ws", NULL);
	other_workspace = g_build_filename(base, "ows", NULL);
	state = g_build_filename(base, "st", NULL);
	secrets = g_build_filename(base, "secrets", NULL);
	config_path = g_build_filename(base, "clawtilla.yaml", NULL);
	stub = g_build_filename(base, "libreclaw-stub", NULL);
	probe_path = g_build_filename(workspace, "probe", NULL);
	g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(other_workspace, 0700), ==, 0);

	/*
	 * The granted connector's credential where every clawtilla before
	 * this fix kept it -- flat in secrets.dir -- so the test also covers
	 * an install that connected before upgrading.  The other agent's
	 * sits beside it.
	 */
	granted_legacy = g_build_filename(secrets, "connector-granted.json",
	                                  NULL);
	their_file = g_build_filename(secrets, "connector-theirs.json", NULL);
	store_token(granted_legacy, FIRST_TOKEN);
	store_token(their_file, THEIR_TOKEN);
	granted_file = clawt_connector_token_path(secrets, "granted");
	their_current = clawt_connector_token_path(secrets, "theirs");

	/* The daemon's own loop, which the fake server shares. */
	context = g_main_context_new();
	g_main_context_push_thread_default(context);

	fake.authorizations = g_ptr_array_new_with_free_func(g_free);
	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, "/mcp", on_mcp_request, &fake, NULL);
	g_assert_true(soup_server_listen_local(server, 0,
	                                       SOUP_SERVER_LISTEN_IPV4_ONLY,
	                                       &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	g_assert_nonnull(uris);
	port = (guint)g_uri_get_port(uris->data);
	g_slist_free_full(g_steal_pointer(&uris), (GDestroyNotify)g_uri_unref);

	/*
	 * The stub: runs each round's script as it appears, then tries what an
	 * agent that went looking would try.
	 */
	body = g_strdup_printf(
		"#!/bin/sh\n"
		"cd \"%s\"\n"
		"for r in 1 2; do\n"
		"  while [ ! -f run$r.sh ]; do sleep 0.1; done\n"
		"  sh run$r.sh > out$r.tmp 2> err$r\n"
		"  mv out$r.tmp out$r\n"
		"done\n"
		"{\n"
		"  if cat \"%s\" >/dev/null 2>&1; then echo theirs=read; "
		"else echo theirs=held; fi\n"
		"  if cat \"%s\" >/dev/null 2>&1; then echo theirs-moved=read; "
		"else echo theirs-moved=held; fi\n"
		"  if ls \"%s\" >/dev/null 2>&1; then echo listing=$(ls \"%s\" | "
		"tr '\\n' ' '); else echo listing=held; fi\n"
		"} > probe.tmp\n"
		"mv probe.tmp probe\n"
		"exec sleep 600\n",
		workspace, their_file, their_current, secrets, secrets);
	write_executable(stub, body);

	/* The CLI the .mcp.json names is found on PATH, beside the daemon's. */
	path_env = g_strdup_printf("%s:%s", BUILD_OUTDIR, g_getenv("PATH"));
	{
		g_autofree gchar *absolute = clawt_canonicalize_missing(BUILD_OUTDIR);

		g_free(path_env);
		path_env = g_strdup_printf("%s:%s", absolute, g_getenv("PATH"));
	}
	g_setenv("PATH", path_env, TRUE);

	allow_block = allow
		? g_strdup_printf("egress:\n  allow: [\"127.0.0.1:%u\"]\n", port)
		: g_strdup("");

	yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s\"\n"
		"  socket: \"%s/d.sock\"\n"
		"  tailscale: false\n"
		"defaults:\n"
		"  workspace_root: \"%s\"\n"
		"%s"
		"secrets:\n"
		"  dir: \"%s\"\n"
		"skills:\n"
		"  dir: \"%s/skills\"\n"
		"  extra_dirs: []\n"
		"integrations:\n"
		"  - name: granted\n"
		"    type: connector\n"
		"    provider: bearer\n"
		"    url: \"http://127.0.0.1:%u/mcp\"\n"
		"    token_file: \"%s\"\n"
		"    scope: selected\n"
		"    agents: [boxed]\n"
		"  - name: theirs\n"
		"    type: connector\n"
		"    provider: bearer\n"
		"    url: \"http://127.0.0.1:%u/mcp\"\n"
		"    token_file: \"%s\"\n"
		"    scope: selected\n"
		"    agents: [other]\n"
		"agents:\n"
		"  - id: boxed\n"
		"    workspace: \"%s\"\n"
		"    env:\n"
		"      PATH: \"%s\"\n"
		"    runtime:\n"
		"      confine: bwrap\n"
		"      network: %s\n"
		"      restart: never\n"
		"  - id: other\n"
		"    workspace: \"%s\"\n"
		"    runtime:\n"
		"      autostart: false\n",
		state, base, base, allow_block, secrets, base, port, granted_legacy,
		port, their_file, workspace, path_env, network, other_workspace);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, NULL));

	daemon = clawt_daemon_new(config_path, context);
	clawt_daemon_set_libreclaw_binary(daemon, stub);
	g_assert_true(clawt_daemon_start(daemon, &error));
	g_assert_no_error(error);
	g_assert_true(clawt_daemon_start_agent(daemon, "boxed", &error));
	g_assert_no_error(error);

	/*
	 * The daemon moved the flat file into the connector's own directory
	 * before the agent started, and said so in the config.
	 */
	g_assert_false(g_file_test(granted_legacy, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(granted_file, G_FILE_TEST_IS_REGULAR));
	{
		g_autofree gchar *saved = NULL;

		g_assert_true(g_file_get_contents(config_path, &saved, NULL, NULL));
		g_assert_nonnull(strstr(saved, granted_file));
	}

	mcp_json = g_build_filename(workspace, ".mcp.json", NULL);
	g_assert_true(wait_for_file(context, mcp_json, 30));
	command = relay_command(mcp_json, "clawtilla-granted");
	g_test_message("relay: %s", command);

	/* The first round: the credential the box started with. */
	first = run_round(context, workspace, command, 1);
	g_test_message("round 1:\n%s", first);

	{
		g_autofree gchar *err_path = g_build_filename(workspace, "err1",
		                                              NULL);
		g_autofree gchar *err = NULL;

		if (g_file_get_contents(err_path, &err, NULL, NULL))
			g_test_message("round 1 stderr:\n%s", err);
	}

	if (reaches) {
		g_assert_nonnull(strstr(first, "\"protocolVersion\""));
		g_assert_nonnull(strstr(first, "pong-from-fake"));
	} else {
		/*
		 * Refused by the gate, and said so in words naming the setting,
		 * on both requests -- neither left unanswered.
		 */
		g_assert_cmpuint(clawt_test_count_substrings(first, "egress.allow"),
		                 ==, 2);
		g_assert_nonnull(strstr(first, "\"isError\":true"));
		g_assert_null(strstr(first, "pong-from-fake"));
	}

	/*
	 * A renewal, as the daemon stores one: a new file renamed into the
	 * connector's own directory while the box is running.
	 */
	store_token(granted_file, SECOND_TOKEN);

	second = run_round(context, workspace, command, 2);
	g_test_message("round 2:\n%s", second);

	for (i = 0; i < fake.authorizations->len; i++)
		g_test_message("server saw: %s",
		               (const gchar *)g_ptr_array_index(fake.authorizations,
		                                                i));

	if (reaches) {
		/* What the server saw: the first credential, then the renewed one. */
		g_assert_nonnull(strstr(second, "pong-from-fake"));
		expected = g_strdup_printf("Bearer %s", FIRST_TOKEN);
		g_assert_cmpuint(fake.authorizations->len, >=, 2);
		g_assert_cmpstr(g_ptr_array_index(fake.authorizations, 0), ==,
		                expected);
		g_free(expected);
		expected = g_strdup_printf("Bearer %s", SECOND_TOKEN);
		g_assert_cmpstr(g_ptr_array_index(fake.authorizations,
		                                  fake.authorizations->len - 1),
		                ==, expected);
	} else {
		g_assert_cmpuint(fake.authorizations->len, ==, 0);
		g_assert_nonnull(strstr(second, "egress.allow"));
	}

	for (i = 0; i < fake.authorizations->len; i++)
		g_assert_null(strstr(g_ptr_array_index(fake.authorizations, i),
		                     THEIR_TOKEN));

	/* And what the agent could not reach. */
	g_assert_true(wait_for_file(context, probe_path, 30));
	g_assert_true(g_file_get_contents(probe_path, &probe, NULL, NULL));
	g_test_message("probe:\n%s", probe);
	g_assert_nonnull(strstr(probe, "theirs=held\n"));
	g_assert_nonnull(strstr(probe, "theirs-moved=held\n"));
	g_assert_true(g_file_test(their_current, G_FILE_TEST_IS_REGULAR));
	g_assert_nonnull(strstr(probe, "listing=connectors\n"));
	g_assert_null(strstr(probe, "connector-theirs"));

	clawt_daemon_stop(daemon);
	soup_server_disconnect(server);

	while (g_main_context_iteration(context, FALSE))
		;

	g_main_context_pop_thread_default(context);
	g_clear_object(&daemon);
	g_ptr_array_unref(fake.authorizations);
	clawt_test_remove_tree(base);
}

static void
test_open_agent_uses_its_connector(void)
{
	run_case("open", FALSE);
}

static void
test_gated_agent_reaches_an_allowed_connector(void)
{
	run_case("gated", TRUE);
}

static void
test_gated_agent_is_told_why_a_connector_is_refused(void)
{
	run_case("gated", FALSE);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/connector-confine-integration/open",
	                test_open_agent_uses_its_connector);
	g_test_add_func("/connector-confine-integration/gated-allowed",
	                test_gated_agent_reaches_an_allowed_connector);
	g_test_add_func("/connector-confine-integration/gated-refused",
	                test_gated_agent_is_told_why_a_connector_is_refused);

	return g_test_run();
}
