/*
 * test-egress-integration.c - A real gated box, a real gate, a real broker
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Behind CLAWT_TEST_INTEGRATION=1, because it runs bubblewrap and reaches
 * the internet.  A confined agent with `runtime.network: gated` is started
 * for real -- a shell script standing in for libreclaw -- and asked to
 * reach a public site through the proxy (which works), the same site
 * around it (which has no route), a loopback listener on this machine and
 * a LAN address (both refused by the gate, by address), and its provider
 * through the broker with the token it was handed (which arrives at the
 * provider carrying the real key -- an invalid one, so the provider's own
 * 401 is the proof).
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

static void
write_executable(
	const gchar *path,
	const gchar *text
){
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

/* A loopback listener the box must not reach: accepts and says "inside". */
static gboolean
on_loopback(
	GSocketService    *service,
	GSocketConnection *connection,
	GObject           *source,
	gpointer           user_data
){
	static const gchar reply[] =
		"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\ninside";
	GOutputStream *out =
		g_io_stream_get_output_stream(G_IO_STREAM(connection));

	g_output_stream_write_all(out, reply, sizeof(reply) - 1, NULL, NULL,
	                          NULL);
	g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);

	return TRUE;
}

/* Iterates the default context until @path exists or @seconds pass. */
static gboolean
wait_for_file(
	const gchar *path,
	guint        seconds
){
	gint64 deadline = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return TRUE;

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(10 * 1000);
	}

	return g_file_test(path, G_FILE_TEST_EXISTS);
}

static void
test_gated_box(void)
{
	g_autofree gchar *made = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *workspace = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *egress = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *report = NULL;
	g_autofree gchar *done = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *gate_socket = NULL;
	g_autofree gchar *broker_socket = NULL;
	g_autofree gchar *relay = NULL;
	g_autofree gchar *curl = NULL;
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(ClawtProcessRuntime) runtime = NULL;
	g_autoptr(ClawtSandbox) sandbox = NULL;
	g_autoptr(ClawtEgressGate) gate = NULL;
	g_autoptr(ClawtEgressBroker) broker = NULL;
	g_autoptr(GSocketService) loopback = NULL;
	g_autoptr(GHashTable) env = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) refusals = NULL;
	ClawtAgentConfig *agent;
	guint16 port;
	guint i;

	if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL) {
		g_test_skip("set CLAWT_TEST_INTEGRATION=1 to run a real gated box");
		return;
	}

	{
		g_autofree gchar *bwrap = g_find_program_in_path("bwrap");

		curl = g_find_program_in_path("curl");

		if (bwrap == NULL || curl == NULL) {
			g_test_skip("needs bubblewrap and curl");
			return;
		}
	}

	relay = g_build_filename(BUILD_OUTDIR, "clawtilla-relay", NULL);

	if (!g_file_test(relay, G_FILE_TEST_IS_EXECUTABLE)) {
		g_test_skip("build clawtilla-relay first (make relay)");
		return;
	}

	made = g_dir_make_tmp("clawt-egress-it-XXXXXX", NULL);
	base = clawt_canonicalize_missing(made);
	workspace = g_build_filename(base, "workspace", NULL);
	state = g_build_filename(base, "state", NULL);
	egress = g_build_filename(base, "egress", NULL);
	g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(state, 0700), ==, 0);

	/* The listener on this machine's loopback, on a port of its own. */
	loopback = g_socket_service_new();
	port = g_socket_listener_add_any_inet_port(
		G_SOCKET_LISTENER(loopback), NULL, &error);
	g_assert_no_error(error);
	g_signal_connect(loopback, "incoming", G_CALLBACK(on_loopback), NULL);

	/* The daemon's half: the gate and the broker, on the default loop. */
	{
		g_autofree gchar *gate_dir = g_build_filename(egress, "gate", NULL);

		gate = clawt_egress_gate_new(NULL, gate_dir);
		gate_socket = clawt_egress_gate_open(gate, "boxed", &error);
		g_assert_no_error(error);
	}

	broker = clawt_egress_broker_new(NULL);
	broker_socket = g_build_filename(egress, "broker.sock", NULL);
	g_assert_true(clawt_egress_broker_listen(broker, broker_socket,
	                                         &error));
	g_assert_no_error(error);

	report = g_build_filename(workspace, "report", NULL);
	done = g_build_filename(workspace, "done", NULL);
	script = g_build_filename(workspace, "probe.sh", NULL);

	/*
	 * Each probe prints one line.  --max-time bounds them all, so a
	 * probe that has no route fails rather than hangs the test.
	 */
	body = g_strdup_printf(
		"#!/bin/sh\n"
		"C='%s --silent --show-error --max-time 20 -o /dev/null "
		"-w %%{http_code}'\n"
		"{\n"
		"  echo \"key=$ANTHROPIC_API_KEY\"\n"
		"  echo \"proxy=$HTTPS_PROXY\"\n"
		"  echo \"public=$($C https://example.com/ 2>/dev/null)\"\n"
		"  echo \"around=$($C --noproxy '*' https://example.com/ "
		"2>/dev/null)\"\n"
		/* --noproxy '' overrides NO_PROXY, which exempts 127.0.0.1. */
		"  echo \"loopback=$($C --noproxy '' http://127.0.0.1:%u/ "
		"2>/dev/null)\"\n"
		/* A name that resolves to this machine: refused by address. */
		"  echo \"by-name=$($C --noproxy '' http://localhost:%u/ "
		"2>/dev/null)\"\n"
		"  echo \"loopback-direct=$($C --noproxy '*' "
		"http://127.0.0.1:%u/ 2>/dev/null)\"\n"
		"  echo \"lan=$($C http://192.168.1.1/ 2>/dev/null)\"\n"
		"  echo \"metadata=$($C http://169.254.169.254/ 2>/dev/null)\"\n"
		"  echo \"broker=$($C -H \"x-api-key: $ANTHROPIC_API_KEY\" "
		"$ANTHROPIC_BASE_URL/v1/models 2>/dev/null)\"\n"
		"  echo \"stolen=$($C -H \"x-api-key: sk-ant-not-yours\" "
		"$ANTHROPIC_BASE_URL/v1/models 2>/dev/null)\"\n"
		/* The bodies, to tell the provider's 401 from the broker's. */
		"  echo \"broker-body=$(%s -s --max-time 20 -H \"x-api-key: "
		"$ANTHROPIC_API_KEY\" $ANTHROPIC_BASE_URL/v1/models)\"\n"
		"  echo \"stolen-body=$(%s -s --max-time 20 -H \"x-api-key: "
		"sk-ant-not-yours\" $ANTHROPIC_BASE_URL/v1/models)\"\n"
		"} > \"%s\"\n"
		"touch \"%s\"\n",
		curl, (guint)port, (guint)port, (guint)port, curl, curl, report,
		done);
	write_executable(script, body);

	yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s\"\n"
		"agents:\n"
		"  - id: boxed\n"
		"    workspace: \"%s\"\n"
		"    runtime:\n"
		"      confine: bwrap\n"
		"      network: gated\n",
		state, workspace);
	config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);
	agent = clawt_config_get_agent(config, "boxed");
	g_assert_false(clawt_agent_config_is_shadow(agent));

	/* /bin/sh -c probe.sh, in place of libreclaw -c config.yaml. */
	runtime = clawt_process_runtime_new(agent, script);
	clawt_process_runtime_set_binary(runtime, "/bin/sh");
	clawt_process_runtime_set_relay_binary(runtime, relay);

	env = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(env, g_strdup("PATH"),
	                    g_strdup(g_getenv("PATH")));
	g_hash_table_insert(env, g_strdup("ANTHROPIC_API_KEY"),
	                    g_strdup("sk-ant-clawtilla-integration-test"));
	clawt_process_runtime_set_environment(runtime, env);

	sandbox = clawt_agent_confine_build(agent, state, NULL, NULL, NULL,
	                                    &error);
	g_assert_no_error(error);
	clawt_process_runtime_set_sandbox(runtime, sandbox);
	clawt_process_runtime_set_egress(runtime, CLAWT_RUNTIME_NETWORK_GATED,
	                                 gate_socket, broker);

	/* The command, in the log, for whoever has to debug a failure. */
	{
		g_auto(GStrv) argv = NULL;
		g_auto(GStrv) child_env = NULL;
		g_autofree gchar *joined = NULL;

		g_assert_true(clawt_process_runtime_prepare(runtime, &argv,
		                                            &child_env, &error));
		joined = g_strjoinv(" ", argv);
		g_test_message("%s", joined);
	}

	g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
	                                        &error));
	g_assert_no_error(error);

	g_assert_true(wait_for_file(done, 90));
	g_assert_true(g_file_get_contents(report, &text, NULL, NULL));
	g_test_message("%s", text);

	/* The real key never entered the box; a token did. */
	g_assert_null(strstr(text, "sk-ant-clawtilla-integration-test"));
	g_assert_nonnull(strstr(text, "key=clawt-broker-"));
	g_assert_nonnull(strstr(text, "proxy=http://127.0.0.1:3128\n"));

	/* Public through the gate; nothing around it. */
	g_assert_nonnull(strstr(text, "public=200\n"));
	g_assert_null(strstr(text, "around=200"));

	/* This machine, the LAN and metadata refused by the gate (403). */
	g_assert_nonnull(strstr(text, "loopback=403\n"));
	g_assert_nonnull(strstr(text, "by-name=403\n"));
	g_assert_null(strstr(text, "loopback-direct=200"));
	g_assert_nonnull(strstr(text, "lan=403\n"));
	g_assert_nonnull(strstr(text, "metadata=403\n"));

	/*
	 * The broker reached the provider with the real (invalid) key: the
	 * provider's own 401.  A token the broker never issued is refused by
	 * the broker itself, also 401, before anything leaves the machine.
	 */
	g_assert_nonnull(strstr(text, "broker=401\n"));
	g_assert_nonnull(strstr(text, "stolen=401\n"));
	g_assert_nonnull(strstr(text, "authentication_error"));
	g_assert_nonnull(strstr(text, "stolen-body={\"type\":\"error\","
	                              "\"error\":{\"type\":\"clawtilla_broker\""));
	g_assert_null(strstr(text, "broker-body={\"type\":\"error\","
	                           "\"error\":{\"type\":\"clawtilla_broker\""));

	refusals = clawt_egress_gate_recent_refusals(gate, "boxed");
	g_assert_cmpuint(refusals->len, >=, 3);
	g_assert_cmpuint(clawt_egress_gate_refused_today(gate, "boxed"), >=, 3);

	for (i = 0; i < refusals->len; i++) {
		ClawtEgressRefusal *refusal = g_ptr_array_index(refusals, i);

		g_test_message("refused %s:%u (%s): %s", refusal->host,
		               refusal->port,
		               refusal->address != NULL ? refusal->address : "-",
		               refusal->reason);
	}

	clawt_agent_runtime_stop(CLAWT_AGENT_RUNTIME(runtime));

	clawt_egress_gate_stop(gate);
	clawt_egress_broker_stop(broker);
	g_socket_service_stop(loopback);
	g_socket_listener_close(G_SOCKET_LISTENER(loopback));

	while (g_main_context_iteration(NULL, FALSE))
		;

	clawt_test_remove_tree(base);
}

/*
 * The same through the daemon, which is the path that matters: it makes
 * the gate and the broker on the first gated start, binds the agent's
 * socket and hands the runtime its network.  libreclaw is a stub script
 * that reports what it was given and what it could reach.
 */
static void
test_daemon_starts_a_gated_agent(void)
{
	g_autofree gchar *made = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *workspace = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *config_path = NULL;
	g_autofree gchar *stub = NULL;
	g_autofree gchar *report = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *curl = NULL;
	g_autofree gchar *gate_socket = NULL;
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(ClawtDaemon) daemon = NULL;
	g_autoptr(GError) error = NULL;
	gint64 deadline;

	if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL) {
		g_test_skip("set CLAWT_TEST_INTEGRATION=1 to run a real gated box");
		return;
	}

	{
		g_autofree gchar *bwrap = g_find_program_in_path("bwrap");
		g_autofree gchar *relay =
			g_build_filename(BUILD_OUTDIR, "clawtilla-relay", NULL);

		curl = g_find_program_in_path("curl");

		if (bwrap == NULL || curl == NULL ||
		    !g_file_test(relay, G_FILE_TEST_IS_EXECUTABLE)) {
			g_test_skip("needs bubblewrap, curl and clawtilla-relay");
			return;
		}
	}

	/* Short, because the daemon's sockets live under it. */
	made = g_dir_make_tmp("cegd-XXXXXX", NULL);
	base = clawt_canonicalize_missing(made);
	workspace = g_build_filename(base, "ws", NULL);
	state = g_build_filename(base, "st", NULL);
	config_path = g_build_filename(base, "clawtilla.yaml", NULL);
	stub = g_build_filename(base, "libreclaw-stub", NULL);
	report = g_build_filename(workspace, "report", NULL);
	g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);

	body = g_strdup_printf(
		"#!/bin/sh\n"
		"{\n"
		"  echo \"key=$ANTHROPIC_API_KEY\"\n"
		"  echo \"proxy=$HTTPS_PROXY\"\n"
		"  echo \"public=$(%s -s -o /dev/null -w %%{http_code} --max-time "
		"20 https://example.com/)\"\n"
		"} > \"%s.tmp\"\n"
		"mv \"%s.tmp\" \"%s\"\n"
		"exec sleep 600\n",
		curl, report, report, report);
	write_executable(stub, body);

	yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s\"\n"
		"  socket: \"%s/d.sock\"\n"
		"  tailscale: false\n"
		"defaults:\n"
		"  workspace_root: \"%s\"\n"
		"agents:\n"
		"  - id: boxed\n"
		"    workspace: \"%s\"\n"
		"    env:\n"
		"      PATH: \"%s\"\n"
		"      ANTHROPIC_API_KEY: sk-ant-daemon-integration\n"
		"    runtime:\n"
		"      confine: bwrap\n"
		"      network: gated\n"
		"      restart: never\n",
		state, base, base, workspace, g_getenv("PATH"));
	g_assert_true(g_file_set_contents(config_path, yaml, -1, NULL));

	context = g_main_context_new();
	daemon = clawt_daemon_new(config_path, context);
	clawt_daemon_set_libreclaw_binary(daemon, stub);

	g_main_context_push_thread_default(context);
	g_assert_true(clawt_daemon_start(daemon, &error));
	g_assert_no_error(error);
	g_assert_true(clawt_daemon_start_agent(daemon, "boxed", &error));
	g_assert_no_error(error);

	deadline = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;

	while (!g_file_test(report, G_FILE_TEST_EXISTS) &&
	       g_get_monotonic_time() < deadline)
		if (!g_main_context_iteration(context, FALSE))
			g_usleep(10 * 1000);

	g_assert_true(g_file_get_contents(report, &text, NULL, NULL));
	g_test_message("%s", text);

	g_assert_null(strstr(text, "sk-ant-daemon-integration"));
	g_assert_nonnull(strstr(text, "key=clawt-broker-"));
	g_assert_nonnull(strstr(text, "proxy=http://127.0.0.1:3128\n"));
	g_assert_nonnull(strstr(text, "public=200\n"));

	gate_socket = g_build_filename(state, "egress", "gate", "boxed.sock",
	                               NULL);
	g_assert_true(g_file_test(gate_socket, G_FILE_TEST_EXISTS));

	clawt_daemon_stop(daemon);

	while (g_main_context_iteration(context, FALSE))
		;

	g_main_context_pop_thread_default(context);
	g_clear_object(&daemon);
	clawt_test_remove_tree(base);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/egress-integration/gated-box", test_gated_box);
	g_test_add_func("/egress-integration/daemon-gated-agent",
	                test_daemon_starts_a_gated_agent);

	return g_test_run();
}
