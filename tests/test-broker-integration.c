/*
 * test-broker-integration.c - A real broker, in real podman
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Behind CLAWT_TEST_INTEGRATION=1, because it runs this user's rootless
 * podman and pulls images of several hundred megabytes if they are not
 * here yet.  Each broker is started through #ClawtBroker -- the same
 * containers the daemon would make, from a config pinned to a temporary
 * state directory -- and waited on until its gateway answers; then its
 * MCP endpoint is asked something with no key, which the gateway must
 * refuse; then it is stopped, started again on the same state (the
 * database survives the containers being replaced) and stopped for good.
 *
 * The state directory holds files the containers' own users wrote, which
 * are other uids in podman's user namespace, so it is removed with
 * `podman unshare` rather than by this user.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>

#include "clawt-http.h"
#include "clawt-test-util.h"

/* A first start may pull an image; give it time, but not for ever. */
#define BROKER_WAIT_SECONDS (15 * 60)

static gboolean
integration_enabled(
	void
){
	return g_getenv("CLAWT_TEST_INTEGRATION") != NULL;
}

static void
remove_state(
	const gchar *dir
){
	const gchar *argv[] = { "podman", "unshare", "rm", "-rf", dir, NULL };

	g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
	             NULL, NULL, NULL, NULL, NULL);

	if (g_file_test(dir, G_FILE_TEST_EXISTS))
		clawt_test_remove_tree(dir);
}

/* Iterates @context until @broker reaches @state, fails or time runs out. */
static ClawtBrokerState
wait_for(
	ClawtBroker      *broker,
	GMainContext     *context,
	ClawtBrokerState  wanted
){
	gint64 deadline = g_get_monotonic_time() +
	                  (gint64)BROKER_WAIT_SECONDS * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		ClawtBrokerState state = clawt_broker_get_state(broker);

		if (state == wanted || state == CLAWT_BROKER_STATE_ERROR)
			return state;

		g_main_context_iteration(context, TRUE);
	}

	return clawt_broker_get_state(broker);
}

/* The HTTP status the gateway gives an MCP request with no key. */
static guint
status_without_key(
	const gchar *url
){
	g_autoptr(SoupSession) session = soup_session_new_with_options(
		"timeout", 10, NULL);
	g_autoptr(SoupMessage) message = clawt_http_message_new(
		"POST", url, CLAWT_HTTP_NO_CREDENTIALS);
	g_autoptr(GBytes) request = NULL;
	g_autoptr(GBytes) body = NULL;
	static const gchar call[] =
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"whoami\",\"arguments\":{}}}";

	g_assert_nonnull(message);
	request = g_bytes_new_static(call, sizeof(call) - 1);
	soup_message_set_request_body_from_bytes(message, "application/json",
	                                         request);
	soup_message_headers_replace(soup_message_get_request_headers(message),
	                             "Accept",
	                             "application/json, text/event-stream");
	body = clawt_http_send_and_read(session, message, NULL, NULL);

	return soup_message_get_status(message);
}

static void
run_broker(
	const gchar *broker_yaml,
	const gchar *server
){
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtBroker) broker = NULL;
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *url = NULL;
	ClawtBrokerState state;
	guint refused;

	dir = g_dir_make_tmp("clawt-broker-real-XXXXXX", NULL);
	state_dir = g_build_filename(dir, "state", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  state_dir: \"%s\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"broker:\n  enabled: true\n%s",
		state_dir, dir, broker_yaml);
	config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);

	g_main_context_push_thread_default(context);

	bridge = clawt_pod_bridge_new(CLAWT_TEST_POD_MODULE_DIR);
	broker = clawt_broker_new(bridge, context);
	clawt_broker_set_config(broker, config, state_dir);

	/* Started in the background: the call itself waits on nothing. */
	clawt_broker_start(broker);
	g_assert_cmpint(clawt_broker_get_state(broker), ==,
	                CLAWT_BROKER_STATE_STARTING);

	state = wait_for(broker, context, CLAWT_BROKER_STATE_RUNNING);
	g_test_message("broker: %s (%s)",
	               clawt_broker_state_label(state),
	               clawt_broker_get_detail(broker) != NULL
	               ? clawt_broker_get_detail(broker) : "");
	g_assert_cmpint(state, ==, CLAWT_BROKER_STATE_RUNNING);

	/* It answers, and it refuses a call that brings no key. */
	url = clawt_broker_mcp_url(config, server);
	refused = status_without_key(url);
	g_test_message("%s without a key: HTTP %u", url, refused);
	g_assert_cmpuint(refused, ==, 401);

	/* Stopped, then started on the same state: the data outlives them. */
	clawt_broker_stop(broker);
	state = wait_for(broker, context, CLAWT_BROKER_STATE_STOPPED);
	g_assert_cmpint(state, ==, CLAWT_BROKER_STATE_STOPPED);

	clawt_broker_start(broker);
	state = wait_for(broker, context, CLAWT_BROKER_STATE_RUNNING);
	g_assert_cmpint(state, ==, CLAWT_BROKER_STATE_RUNNING);

	clawt_broker_stop_sync(broker);
	g_assert_cmpint(clawt_broker_get_state(broker), ==,
	                CLAWT_BROKER_STATE_STOPPED);

	g_main_context_pop_thread_default(context);

	/* The containers are removed too: a test leaves nothing in podman. */
	{
		const gchar *names[] = { "clawt-broker-anythingmcp",
		                         "clawt-broker-anythingmcp-db",
		                         "clawt-broker-jentic", NULL };
		guint i;

		for (i = 0; names[i] != NULL; i++) {
			const gchar *argv[] = { "podman", "rm", "-f", "--ignore",
			                        names[i], NULL };

			g_spawn_sync(NULL, (gchar **)argv, NULL,
			             G_SPAWN_SEARCH_PATH |
			             G_SPAWN_STDOUT_TO_DEV_NULL |
			             G_SPAWN_STDERR_TO_DEV_NULL,
			             NULL, NULL, NULL, NULL, NULL, NULL);
		}
	}

	remove_state(dir);
}

static void
test_anythingmcp(
	void
){
	if (!integration_enabled()) {
		g_test_skip("set CLAWT_TEST_INTEGRATION=1 to run AnythingMCP in "
		            "podman");
		return;
	}

	run_broker("  type: anythingmcp\n  port: 47491\n  dashboard_port: 47391\n",
	           "no-such-server");
}

static void
test_jentic(
	void
){
	if (!integration_enabled()) {
		g_test_skip("set CLAWT_TEST_INTEGRATION=1 to run Jentic One in "
		            "podman");
		return;
	}

	run_broker("  type: jentic\n  port: 47891\n", NULL);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/broker-integration/anythingmcp", test_anythingmcp);
	g_test_add_func("/broker-integration/jentic", test_jentic);

	return g_test_run();
}
