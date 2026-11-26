/*
 * test-container-keep.c - A kept container survives a daemon restart
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * VENTURE #21: every computer object starts ABSENT, so the first start
 * after a daemon restart provisioned, collided with the container that
 * was there all along, and removed it -- every kept container destroyed,
 * with everything installed in it.  These drive a fresh
 * ClawtContainerComputer against a fake podman: the real podomation
 * container module on one side of a unix socket, a scripted libpod API
 * on the other, and a fake `podman` on PATH for the inspect the
 * adoption compares with.  No podman is needed, and none is touched.
 *
 * The comparison itself is also tested as data, through
 * clawt_container_computer_compare(), since a false "differs" is the
 * failure that destroys containers.
 */

#include <clawtilla.h>

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

/* ── A container as `podman container inspect` prints it ──────────── */

/*
 * @image, @label, the one bind mount (@source at /data/source, rw), and
 * the default command.  The shape is podman's, trimmed to the members
 * the comparison reads.
 */
static gchar *
inspect_json(
	const gchar *image,
	const gchar *label,
	const gchar *source,
	const gchar *destination,
	gboolean     rw
){
	return g_strdup_printf(
		"[{\"Id\":\"abc123\",\"Name\":\"clawt-keeper\","
		"\"Image\":\"0123456789abcdef\",\"ImageName\":\"%s\","
		"\"ProcessLabel\":\"%s\","
		"\"Mounts\":[{\"Type\":\"bind\",\"Source\":\"%s\","
		"\"Destination\":\"%s\",\"Driver\":\"\",\"Mode\":\"\","
		"\"Options\":[\"rbind\"],\"RW\":%s,\"Propagation\":\"rprivate\"}],"
		"\"Config\":{\"Cmd\":[\"sleep\",\"infinity\"]},"
		"\"State\":{\"Status\":\"exited\"}}]",
		image, label, source, destination, rw ? "true" : "false");
}

/* A read-write bind of @source at @target. */
static ClawtMount *
rw_mount(
	const gchar *source,
	const gchar *target
){
	ClawtMount *mount = clawt_mount_new(source, target);

	clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);

	return mount;
}

/* A container computer with one rw bind of @source at /data/source. */
static ClawtComputer *
keeper_new(
	ClawtPodBridge *bridge,
	const gchar    *source
){
	ClawtComputer *computer;
	g_autoptr(ClawtMount) mount = NULL;

	computer = clawt_container_computer_new(
		"keeper", bridge, "registry.fedoraproject.org/fedora:44");
	clawt_container_computer_set_keep(CLAWT_CONTAINER_COMPUTER(computer),
	                                  TRUE);

	if (source != NULL) {
		mount = rw_mount(source, "/data/source");
		clawt_computer_add_mount(computer, mount);
	}

	return computer;
}

/* ── The comparison, as data ───────────────────────────────────────── */

/* What compare() says for @inspected, asserting it could tell. */
static gchar *
reason_for(
	ClawtComputer *computer,
	const gchar   *inspected
){
	g_autoptr(GError) error = NULL;
	gchar *reason = NULL;

	g_assert_true(clawt_container_computer_compare(
		CLAWT_CONTAINER_COMPUTER(computer), inspected, &reason, &error));
	g_assert_no_error(error);

	return reason;
}

/*
 * The same container is kept: an image named as podman resolved it, a
 * mount whose source is spelled differently but is the same directory,
 * and a level that matches.
 */
static void
test_compare_matching_is_kept(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *spelled = NULL;
	g_autofree gchar *inspected = NULL;
	g_autofree gchar *reason = NULL;

	computer = keeper_new(bridge, dir);
	clawt_container_computer_set_selinux_level(
		CLAWT_CONTAINER_COMPUTER(computer), "s0:c1,c2");

	/* Exactly as configured. */
	inspected = inspect_json("registry.fedoraproject.org/fedora:44",
	                         "system_u:system_r:container_t:s0:c1,c2", dir,
	                         "/data/source", TRUE);
	reason = reason_for(computer, inspected);
	g_assert_null(reason);
	g_clear_pointer(&inspected, g_free);

	/* A trailing slash and a "." are the same directory. */
	spelled = g_strdup_printf("%s/./", dir);
	inspected = inspect_json("registry.fedoraproject.org/fedora:44",
	                         "system_u:system_r:container_t:s0:c1,c2",
	                         spelled, "/data/source/", TRUE);
	reason = reason_for(computer, inspected);
	g_assert_null(reason);
	g_clear_pointer(&inspected, g_free);

	clawt_test_remove_tree(dir);
}

/*
 * A short image name is the same image as the one podman resolved it
 * to, and a missing tag is ":latest".  A false "differs" here would
 * destroy the container at every start.
 */
static void
test_compare_image_names(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	struct {
		const gchar *configured;
		const gchar *podman;
		gboolean     same;
	} cases[] = {
		{ "fedora:44", "registry.fedoraproject.org/fedora:44", TRUE },
		{ "fedora", "docker.io/library/fedora:latest", TRUE },
		{ "registry.fedoraproject.org/fedora:44",
		  "registry.fedoraproject.org/fedora:44", TRUE },
		{ "localhost:5000/tools", "localhost:5000/tools:latest", TRUE },
		{ "fedora:44", "registry.fedoraproject.org/fedora:45", FALSE },
		{ "fedora:44", "docker.io/library/debian:12", FALSE },
		/* Not on a '/' boundary: a different image. */
		{ "fedora:44", "registry.example/myfedora:44", FALSE },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(ClawtComputer) computer = clawt_container_computer_new(
			"keeper", bridge, cases[i].configured);
		g_autofree gchar *inspected = g_strdup_printf(
			"[{\"ImageName\":\"%s\",\"Mounts\":[]}]", cases[i].podman);
		g_autofree gchar *reason = reason_for(computer, inspected);

		g_test_message("%s vs %s", cases[i].configured, cases[i].podman);

		if (cases[i].same) {
			g_assert_null(reason);
		} else {
			g_assert_nonnull(reason);
			g_assert_nonnull(strstr(reason, "image changed"));
		}
	}
}

/* Each mount change is named: added, removed, and changed in place. */
static void
test_compare_mounts(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(ClawtMount) extra = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *other = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *inspected = NULL;
	g_autofree gchar *reason = NULL;
	const gchar *image = "registry.fedoraproject.org/fedora:44";

	computer = keeper_new(bridge, dir);
	extra = rw_mount(other, "/data/foo");
	clawt_computer_add_mount(computer, extra);

	/* The container has /data/source only: /data/foo was added. */
	inspected = inspect_json(image, "", dir, "/data/source", TRUE);
	reason = reason_for(computer, inspected);
	g_assert_cmpstr(reason, ==, "mounts changed (+/data/foo)");
	g_clear_pointer(&inspected, g_free);
	g_clear_pointer(&reason, g_free);

	/* A different source at the same place, and an extra one podman has. */
	inspected = g_strdup_printf(
		"[{\"ImageName\":\"%s\",\"Mounts\":["
		"{\"Type\":\"bind\",\"Source\":\"%s\",\"Destination\":\"/data/source\","
		"\"RW\":true},"
		"{\"Type\":\"bind\",\"Source\":\"%s\",\"Destination\":\"/data/foo\","
		"\"RW\":true},"
		"{\"Type\":\"bind\",\"Source\":\"/srv/old\",\"Destination\":\"/old\","
		"\"RW\":true},"
		"{\"Type\":\"volume\",\"Name\":\"cache\",\"Source\":\"/var/lib/x\","
		"\"Destination\":\"/cache\",\"RW\":true}]}]",
		image, other, other);
	reason = reason_for(computer, inspected);
	g_assert_cmpstr(reason, ==, "mounts changed (~/data/source, -/old)");
	g_clear_pointer(&inspected, g_free);
	g_clear_pointer(&reason, g_free);

	/* Read-only in the container, read-write in the config. */
	inspected = g_strdup_printf(
		"[{\"ImageName\":\"%s\",\"Mounts\":["
		"{\"Type\":\"bind\",\"Source\":\"%s\",\"Destination\":\"/data/source\","
		"\"RW\":false},"
		"{\"Type\":\"bind\",\"Source\":\"%s\",\"Destination\":\"/data/foo\","
		"\"RW\":true}]}]",
		image, dir, other);
	reason = reason_for(computer, inspected);
	g_assert_cmpstr(reason, ==, "mounts changed (~/data/source)");
	g_clear_pointer(&inspected, g_free);
	g_clear_pointer(&reason, g_free);

	/* No Mounts member at all is "cannot tell", never "all removed". */
	inspected = g_strdup_printf("[{\"ImageName\":\"%s\"}]", image);
	reason = reason_for(computer, inspected);
	g_assert_null(reason);

	clawt_test_remove_tree(dir);
	clawt_test_remove_tree(other);
}

/*
 * The SELinux level: a differing one is a reason, podman's random pair
 * (no level wanted) accepts any, and a host without SELinux cannot be
 * compared.  The command, which podman also fixes at create, too.
 */
static void
test_compare_level_and_command(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *inspected = NULL;
	g_autofree gchar *reason = NULL;
	const gchar *image = "registry.fedoraproject.org/fedora:44";

	computer = keeper_new(bridge, dir);
	inspected = inspect_json(image, "system_u:system_r:container_t:s0:c7,c9",
	                         dir, "/data/source", TRUE);

	/* Random pair wanted: whatever the container has is fine. */
	reason = reason_for(computer, inspected);
	g_assert_null(reason);

	clawt_container_computer_set_selinux_level(
		CLAWT_CONTAINER_COMPUTER(computer), "s0:c1,c2");
	reason = reason_for(computer, inspected);
	g_assert_cmpstr(reason, ==, "SELinux level changed (s0:c7,c9, the config "
	                            "wants s0:c1,c2)");
	g_clear_pointer(&reason, g_free);
	g_clear_pointer(&inspected, g_free);

	/* No SELinux on the host: an empty label cannot disagree. */
	inspected = inspect_json(image, "", dir, "/data/source", TRUE);
	reason = reason_for(computer, inspected);
	g_assert_null(reason);
	g_clear_pointer(&inspected, g_free);

	/* A different command, and several reasons joined. */
	clawt_container_computer_set_command(CLAWT_CONTAINER_COMPUTER(computer),
	                                     "/sbin/init");
	inspected = inspect_json("docker.io/library/debian:12",
	                         "system_u:system_r:container_t:s0:c1,c2", dir,
	                         "/data/source", TRUE);
	reason = reason_for(computer, inspected);
	g_assert_cmpstr(reason, ==,
	                "image changed (docker.io/library/debian:12, the config "
	                "wants registry.fedoraproject.org/fedora:44); command "
	                "changed (the config wants [\"/sbin/init\"])");

	clawt_test_remove_tree(dir);
}

/* Something that is not podman's inspect is "could not tell". */
static void
test_compare_unreadable(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = keeper_new(bridge, NULL);
	g_autoptr(GError) error = NULL;
	gchar *reason = NULL;

	g_assert_false(clawt_container_computer_compare(
		CLAWT_CONTAINER_COMPUTER(computer), "not json", &reason, &error));
	g_assert_nonnull(error);
	g_assert_null(reason);
	g_clear_error(&error);

	g_assert_false(clawt_container_computer_compare(
		CLAWT_CONTAINER_COMPUTER(computer), "[]", &reason, &error));
	g_assert_nonnull(error);
	g_assert_null(reason);
}

/* ── A fake podman, both halves ────────────────────────────────────── */

/*
 * Where the fake `podman` lives, put first on PATH once, in main(),
 * before any thread exists: setenv() is not thread-safe, and the
 * container module's event source is a thread.
 */
static gchar *cli_dir = NULL;

/*
 * The libpod API on a unix socket, answering one container's worth of
 * requests from a few flags, and counting what it was asked to do: the
 * assertions are that an adoption creates and removes nothing.
 *
 * @inspect_fails makes the inspect the reconcile asks answer 500 -- a
 * podman that cannot be asked -- so the create's "already in use" path
 * is reachable.
 */
typedef struct {
	gchar           *dir;
	gchar           *socket;
	gchar           *uri;
	gchar           *cli_json;
	gchar           *cli_fail;
	GSocketListener *listener;
	GCancellable    *cancel;
	GThread         *thread;

	GMutex           lock;
	gboolean         exists;
	gboolean         inspect_fails;
	guint            creates;
	guint            removes;
	guint            starts;
	guint            inspects;
} Podman;

/* One HTTP reply with a JSON body, or none. */
static gchar *
http_reply(
	gint         status,
	const gchar *json
){
	return g_strdup_printf("HTTP/1.1 %d X\r\n"
	                       "Content-Type: application/json\r\n"
	                       "Content-Length: %" G_GSIZE_FORMAT "\r\n"
	                       "Connection: close\r\n\r\n%s",
	                       status, json != NULL ? strlen(json) : 0,
	                       json != NULL ? json : "");
}

static gchar *
podman_answer(
	Podman      *podman,
	const gchar *method,
	const gchar *path
){
	static const gchar *missing =
		"{\"cause\":\"no such container\","
		"\"message\":\"no container with name or ID found\","
		"\"response\":404}";
	gchar *reply;

	g_mutex_lock(&podman->lock);

	if (g_strcmp0(method, "GET") == 0 &&
	    g_str_has_suffix(path, "/json") &&
	    strstr(path, "/libpod/containers/") != NULL &&
	    !g_str_has_suffix(path, "/containers/json")) {
		podman->inspects++;

		if (podman->inspect_fails)
			reply = http_reply(500, "{\"cause\":\"busy\","
			                        "\"message\":\"podman is busy\"}");
		else if (podman->exists)
			reply = http_reply(200,
				"{\"Id\":\"abc123\",\"Name\":\"clawt-keeper\","
				"\"Image\":\"0123456789abcdef\","
				"\"State\":{\"Status\":\"exited\",\"Pid\":0}}");
		else
			reply = http_reply(404, missing);
	} else if (g_strcmp0(method, "POST") == 0 &&
	           g_str_has_suffix(path, "/libpod/containers/create")) {
		if (podman->exists) {
			reply = http_reply(500,
				"{\"cause\":\"that name is already in use\","
				"\"message\":\"creating container storage: the container "
				"name \\\"clawt-keeper\\\" is already in use by abc123. You "
				"have to remove that container to be able to reuse that "
				"name: that name is already in use\"}");
		} else {
			podman->exists = TRUE;
			podman->creates++;
			reply = http_reply(201, "{\"Id\":\"fresh0001\",\"Warnings\":[]}");
		}
	} else if (g_strcmp0(method, "DELETE") == 0 &&
	           strstr(path, "/libpod/containers/") != NULL) {
		if (podman->exists) {
			podman->exists = FALSE;
			podman->removes++;
			reply = http_reply(200, "[{\"Id\":\"abc123\"}]");
		} else {
			reply = http_reply(404, missing);
		}
	} else if (g_strcmp0(method, "POST") == 0 &&
	           g_str_has_suffix(path, "/start")) {
		podman->starts++;
		reply = podman->exists ? http_reply(204, NULL)
		                       : http_reply(404, missing);
	} else if (g_strcmp0(method, "POST") == 0 &&
	           g_str_has_suffix(path, "/stop")) {
		reply = podman->exists ? http_reply(204, NULL)
		                       : http_reply(404, missing);
	} else {
		/* The module's event stream, and anything else: nothing here. */
		reply = http_reply(404, missing);
	}

	g_mutex_unlock(&podman->lock);

	return reply;
}

static gpointer
podman_serve(
	gpointer data
){
	Podman *podman = data;

	for (;;) {
		g_autoptr(GSocketConnection) conn = NULL;
		g_autoptr(GDataInputStream) in = NULL;
		g_autofree gchar *request = NULL;
		g_autofree gchar *reply = NULL;
		g_auto(GStrv) words = NULL;
		gsize length = 0;
		gchar *line;

		conn = g_socket_listener_accept(podman->listener, NULL,
		                                podman->cancel, NULL);
		if (conn == NULL)
			break;

		in = g_data_input_stream_new(
			g_io_stream_get_input_stream(G_IO_STREAM(conn)));
		g_data_input_stream_set_newline_type(in,
		                                     G_DATA_STREAM_NEWLINE_TYPE_ANY);
		g_filter_input_stream_set_close_base_stream(
			G_FILTER_INPUT_STREAM(in), FALSE);

		/* The request line and headers, to the blank line. */
		while ((line = g_data_input_stream_read_line(in, NULL, NULL,
		                                             NULL)) != NULL) {
			gboolean blank = (*line == '\0');

			if (request == NULL)
				request = g_strdup(line);
			else if (g_ascii_strncasecmp(line, "Content-Length:", 15) == 0)
				length = (gsize)g_ascii_strtoull(line + 15, NULL, 10);

			g_free(line);

			if (blank)
				break;
		}

		/*
		 * And the body, read and dropped: a create carries one, and
		 * closing with it unread resets the connection under the reply.
		 */
		if (length > 0) {
			g_autofree gchar *body = g_malloc(length);
			gsize got = 0;

			g_input_stream_read_all(G_INPUT_STREAM(in), body, length, &got,
			                        NULL, NULL);
		}

		words = g_strsplit(request != NULL ? request : "", " ", 3);
		reply = podman_answer(podman, words[0],
		                      (words[0] != NULL && words[1] != NULL)
		                      ? words[1] : "");

		g_output_stream_write_all(
			g_io_stream_get_output_stream(G_IO_STREAM(conn)), reply,
			strlen(reply), NULL, NULL, NULL);
		g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
	}

	return NULL;
}

/*
 * The `podman` the adoption runs for `container inspect`: a script in
 * @cli_dir that prints this fake's cli.json, or fails when its cli.fail
 * exists.  Absolute
 * paths throughout, since the daemon runs it with an allowlisted
 * environment.
 */
static void
podman_cli_write(
	Podman *podman
){
	g_autofree gchar *script = NULL;
	g_autofree gchar *path = g_build_filename(cli_dir, "podman", NULL);
	g_autoptr(GError) error = NULL;

	script = g_strdup_printf(
		"#!/bin/sh\n"
		"if [ -e '%s' ]; then echo 'Error: inspect failed' >&2; exit 125; fi\n"
		"exec /usr/bin/cat '%s'\n",
		podman->cli_fail, podman->cli_json);

	g_assert_true(g_file_set_contents(path, script, -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

static void
podman_cli_says(
	Podman      *podman,
	const gchar *json
){
	g_autoptr(GError) error = NULL;

	g_assert_true(g_file_set_contents(podman->cli_json, json, -1, &error));
	g_assert_no_error(error);
}

static Podman *
podman_start(
	void
){
	Podman *podman = g_new0(Podman, 1);
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GError) error = NULL;

	g_mutex_init(&podman->lock);
	podman->dir = g_dir_make_tmp("clawt-keep-podman-XXXXXX", NULL);
	g_assert_nonnull(podman->dir);
	podman->socket = g_build_filename(podman->dir, "podman.sock", NULL);
	podman->uri = g_strdup_printf("unix://%s", podman->socket);
	podman->cli_json = g_build_filename(podman->dir, "cli.json", NULL);
	podman->cli_fail = g_build_filename(podman->dir, "cli.fail", NULL);
	podman_cli_write(podman);
	podman_cli_says(podman, "[]");

	podman->cancel = g_cancellable_new();
	podman->listener = g_socket_listener_new();
	address = g_unix_socket_address_new(podman->socket);
	g_socket_listener_add_address(podman->listener, address,
	                              G_SOCKET_TYPE_STREAM,
	                              G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
	                              &error);
	g_assert_no_error(error);
	podman->thread = g_thread_new("fake-podman", podman_serve, podman);

	return podman;
}

static void
podman_stop(
	Podman *podman
){
	g_cancellable_cancel(podman->cancel);
	g_thread_join(podman->thread);
	g_socket_listener_close(podman->listener);
	g_object_unref(podman->listener);
	g_object_unref(podman->cancel);

	clawt_test_remove_tree(podman->dir);
	g_mutex_clear(&podman->lock);
	g_free(podman->cli_json);
	g_free(podman->cli_fail);
	g_free(podman->dir);
	g_free(podman->socket);
	g_free(podman->uri);
	g_free(podman);
}

/*
 * A fresh computer object -- what a daemon restart builds -- bound to
 * the fake podman, or NULL with the test skipped when podomation's
 * container module has not been built.
 */
static ClawtComputer *
fresh_keeper(
	Podman          *podman,
	ClawtPodBridge **bridge,
	const gchar     *source
){
	ClawtComputer *computer;

	*bridge = clawt_pod_bridge_new(CLAWT_TEST_POD_MODULE_DIR);

	/*
	 * Loaded for the fake's socket and never the default connection,
	 * which would start the module's event source against whatever
	 * podman this machine has.
	 */
	if (!clawt_pod_bridge_load_module_for(*bridge, "container", podman->uri,
	                                      NULL)) {
		g_test_skip("podomation's container module has not been built");
		return NULL;
	}

	computer = keeper_new(*bridge, source);
	clawt_container_computer_set_connection(
		CLAWT_CONTAINER_COMPUTER(computer), podman->uri);

	return computer;
}

/* Every message the library logs, for asserting the one that matters. */
static void
capture_messages(
	const gchar    *log_domain,
	GLogLevelFlags  log_level,
	const gchar    *message,
	gpointer        user_data
){
	GPtrArray *seen = user_data;

	(void)log_domain;
	(void)log_level;

	g_ptr_array_add(seen, g_strdup(message));
}

/* How many of @seen match @pattern. */
static guint
messages_matching(
	GPtrArray   *seen,
	const gchar *pattern
){
	guint count = 0;
	guint i;

	for (i = 0; i < seen->len; i++) {
		if (g_pattern_match_simple(pattern, g_ptr_array_index(seen, i)))
			count++;
	}

	return count;
}

/* ── Adoption against the fake ─────────────────────────────────────── */

/*
 * The ticket's case.  A kept container is there, matching its config;
 * a fresh object starts it.  It is adopted: no create, no remove, the
 * adopted id is the one podman named, and nothing is reported as
 * recreated.
 */
static void
test_adopt_matching_container(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *inspected = NULL;
	Podman *podman = podman_start();
	guint handler;

	podman->exists = TRUE;
	inspected = inspect_json("registry.fedoraproject.org/fedora:44",
	                         "system_u:system_r:container_t:s0:c1,c2", dir,
	                         "/data/source", TRUE);
	podman_cli_says(podman, inspected);

	computer = fresh_keeper(podman, &bridge, dir);
	if (computer == NULL)
		goto out;

	clawt_container_computer_set_selinux_level(
		CLAWT_CONTAINER_COMPUTER(computer), "s0:c1,c2");
	g_assert_cmpint(clawt_computer_get_state(computer), ==,
	                CLAWT_COMPUTER_STATE_ABSENT);

	handler = g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
	                            capture_messages, seen);
	g_assert_true(clawt_computer_start(computer, &error));
	g_log_remove_handler(G_LOG_DOMAIN, handler);
	g_assert_no_error(error);

	g_assert_cmpuint(podman->creates, ==, 0);
	g_assert_cmpuint(podman->removes, ==, 0);
	g_assert_cmpuint(podman->starts, ==, 1);
	g_assert_true(podman->exists);
	g_assert_cmpint(clawt_computer_get_state(computer), ==,
	                CLAWT_COMPUTER_STATE_RUNNING);
	g_assert_cmpstr(clawt_container_computer_get_container_id(
	                    CLAWT_CONTAINER_COMPUTER(computer)), ==, "abc123");
	g_assert_null(clawt_container_computer_get_last_recreate(
	    CLAWT_CONTAINER_COMPUTER(computer)));
	g_assert_cmpuint(messages_matching(seen, "*adopted the existing*"), ==,
	                 1);
	g_assert_cmpuint(messages_matching(seen, "recreating *"), ==, 0);

out:
	podman_stop(podman);
	clawt_test_remove_tree(dir);
}

/*
 * The kept container no longer matches: a mount was added to the
 * config.  Exactly one remove and one create, one g_message naming the
 * container and the reason, and the same reason kept for the Fleet page.
 */
static void
test_recreate_when_mounts_changed(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(ClawtMount) extra = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *other = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *inspected = NULL;
	const gchar *note;
	Podman *podman = podman_start();
	guint handler;

	podman->exists = TRUE;
	inspected = inspect_json("registry.fedoraproject.org/fedora:44", "", dir,
	                         "/data/source", TRUE);
	podman_cli_says(podman, inspected);

	computer = fresh_keeper(podman, &bridge, dir);
	if (computer == NULL)
		goto out;

	extra = rw_mount(other, "/data/foo");
	clawt_computer_add_mount(computer, extra);

	handler = g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
	                            capture_messages, seen);
	g_assert_true(clawt_computer_start(computer, &error));
	g_log_remove_handler(G_LOG_DOMAIN, handler);
	g_assert_no_error(error);

	g_assert_cmpuint(podman->removes, ==, 1);
	g_assert_cmpuint(podman->creates, ==, 1);
	g_assert_cmpint(clawt_computer_get_state(computer), ==,
	                CLAWT_COMPUTER_STATE_RUNNING);
	g_assert_cmpstr(clawt_container_computer_get_container_id(
	                    CLAWT_CONTAINER_COMPUTER(computer)), ==, "fresh0001");
	g_assert_cmpuint(messages_matching(
	                     seen, "recreating clawt-keeper: mounts changed "
	                           "(+/data/foo)"), ==, 1);
	g_assert_cmpuint(messages_matching(seen, "recreating *"), ==, 1);

	note = clawt_container_computer_get_last_recreate(
		CLAWT_CONTAINER_COMPUTER(computer));
	g_assert_nonnull(note);
	g_assert_true(g_pattern_match_simple(
		"recreated clawt-keeper at *: mounts changed (+/data/foo)", note));

	/* A second start of the same object has nothing more to do. */
	g_assert_true(clawt_computer_start(computer, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(podman->removes, ==, 1);
	g_assert_cmpuint(podman->creates, ==, 1);

out:
	podman_stop(podman);
	clawt_test_remove_tree(dir);
	clawt_test_remove_tree(other);
}

/* A different image is a recreate too, and says which. */
static void
test_recreate_when_image_changed(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *inspected = NULL;
	Podman *podman = podman_start();
	guint handler;

	podman->exists = TRUE;
	inspected = inspect_json("docker.io/library/debian:12", "", dir,
	                         "/data/source", TRUE);
	podman_cli_says(podman, inspected);

	computer = fresh_keeper(podman, &bridge, dir);
	if (computer == NULL)
		goto out;

	handler = g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
	                            capture_messages, seen);
	g_assert_true(clawt_computer_start(computer, &error));
	g_log_remove_handler(G_LOG_DOMAIN, handler);
	g_assert_no_error(error);

	g_assert_cmpuint(podman->removes, ==, 1);
	g_assert_cmpuint(podman->creates, ==, 1);
	g_assert_cmpuint(messages_matching(
	                     seen, "recreating clawt-keeper: image changed "
	                           "(docker.io/library/debian:12, *"), ==, 1);

out:
	podman_stop(podman);
	clawt_test_remove_tree(dir);
}

/*
 * keep: false is what it was: a container an earlier run left is
 * replaced -- said, but not put on the Fleet page, since replacing it
 * is what not keeping it means -- and stopping removes it.
 */
static void
test_keep_false_unchanged(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *inspected = NULL;
	Podman *podman = podman_start();
	guint handler;

	podman->exists = TRUE;
	inspected = inspect_json("registry.fedoraproject.org/fedora:44", "", dir,
	                         "/data/source", TRUE);
	podman_cli_says(podman, inspected);

	computer = fresh_keeper(podman, &bridge, dir);
	if (computer == NULL)
		goto out;

	clawt_container_computer_set_keep(CLAWT_CONTAINER_COMPUTER(computer),
	                                  FALSE);

	handler = g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
	                            capture_messages, seen);
	g_assert_true(clawt_computer_start(computer, &error));
	g_log_remove_handler(G_LOG_DOMAIN, handler);
	g_assert_no_error(error);

	g_assert_cmpuint(podman->removes, ==, 1);
	g_assert_cmpuint(podman->creates, ==, 1);
	g_assert_cmpuint(messages_matching(
	                     seen, "recreating clawt-keeper: keep is false*"),
	                 ==, 1);
	g_assert_null(clawt_container_computer_get_last_recreate(
	    CLAWT_CONTAINER_COMPUTER(computer)));

	/* Torn down on stop, as before. */
	g_assert_true(clawt_computer_stop(computer, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(podman->removes, ==, 2);
	g_assert_false(podman->exists);
	g_assert_cmpint(clawt_computer_get_state(computer), ==,
	                CLAWT_COMPUTER_STATE_ABSENT);

out:
	podman_stop(podman);
	clawt_test_remove_tree(dir);
}

/*
 * The collision path no longer destroys a kept container.  podman could
 * not be asked about the name (the inspect fails), then the create finds
 * the name taken: the start fails saying so, and nothing is removed.
 * Once podman answers, the next start adopts the container.
 */
static void
test_name_taken_is_not_destroyed(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *inspected = NULL;
	Podman *podman = podman_start();

	podman->exists = TRUE;
	podman->inspect_fails = TRUE;
	inspected = inspect_json("registry.fedoraproject.org/fedora:44", "", dir,
	                         "/data/source", TRUE);
	podman_cli_says(podman, inspected);

	computer = fresh_keeper(podman, &bridge, dir);
	if (computer == NULL)
		goto out;

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
	                      "container clawt-keeper: a container named "
	                      "clawt-keeper already exists and could not be "
	                      "adopted, so nothing was removed*");
	g_assert_false(clawt_computer_start(computer, &error));
	g_test_assert_expected_messages();
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION);
	g_assert_nonnull(strstr(error->message, "nothing was removed"));
	g_clear_error(&error);

	g_assert_cmpuint(podman->removes, ==, 0);
	g_assert_cmpuint(podman->creates, ==, 0);
	g_assert_true(podman->exists);

	/* podman answers again: the next start asks, and adopts. */
	podman->inspect_fails = FALSE;
	g_assert_true(clawt_computer_start(computer, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(podman->removes, ==, 0);
	g_assert_cmpuint(podman->creates, ==, 0);
	g_assert_cmpint(clawt_computer_get_state(computer), ==,
	                CLAWT_COMPUTER_STATE_RUNNING);

out:
	podman_stop(podman);
	clawt_test_remove_tree(dir);
}

/*
 * A comparison that cannot be made keeps the container: the cost is a
 * container on an old setting until somebody rebuilds it, and the
 * opposite is the bug.  Said as a warning.
 */
static void
test_unreadable_inspect_keeps(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *fail = NULL;
	Podman *podman = podman_start();

	podman->exists = TRUE;
	fail = g_strdup(podman->cli_fail);
	g_assert_true(g_file_set_contents(fail, "", -1, NULL));

	computer = fresh_keeper(podman, &bridge, dir);
	if (computer == NULL)
		goto out;

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
	                      "container clawt-keeper: adopted without comparing "
	                      "it with the config*inspect failed*");
	g_assert_true(clawt_computer_start(computer, &error));
	g_test_assert_expected_messages();
	g_assert_no_error(error);

	g_assert_cmpuint(podman->removes, ==, 0);
	g_assert_cmpuint(podman->creates, ==, 0);

out:
	podman_stop(podman);
	clawt_test_remove_tree(dir);
}

/* No container under the name: one 404, then the ordinary create. */
static void
test_nothing_there_is_created(
	void
){
	g_autoptr(ClawtPodBridge) bridge = NULL;
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	Podman *podman = podman_start();

	computer = fresh_keeper(podman, &bridge, dir);
	if (computer == NULL)
		goto out;

	g_assert_true(clawt_computer_start(computer, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(podman->creates, ==, 1);
	g_assert_cmpuint(podman->removes, ==, 0);
	g_assert_cmpuint(podman->inspects, ==, 1);
	g_assert_null(clawt_container_computer_get_last_recreate(
	    CLAWT_CONTAINER_COMPUTER(computer)));

out:
	podman_stop(podman);
	clawt_test_remove_tree(dir);
}

/* ── podman's init as PID 1 (VENTURE #20) ──────────────────────────── */

/*
 * @json, a container as inspect_json() prints it, with a HostConfig.
 *
 * @init is HostConfig.Init's JSON: "true" for a container made with
 * podman's init, or anything else written there.  "" is the HostConfig
 * podman really prints for a container made *without* one: no Init member
 * at all, since libpod declares it `json:"Init,omitempty"` and omits it
 * when false.  NULL leaves HostConfig out altogether.
 */
static gchar *
with_init(
	const gchar *json,
	const gchar *init
){
	g_auto(GStrv) halves = NULL;
	g_autofree gchar *member = NULL;

	if (init == NULL)
		return g_strdup(json);

	halves = g_strsplit(json, "\"State\":", 2);
	g_assert_cmpuint(g_strv_length(halves), ==, 2);
	member = (*init == '\0')
	         ? g_strdup("\"HostConfig\":{\"NetworkMode\":\"none\"},\"State\":")
	         : g_strdup_printf("\"HostConfig\":{\"NetworkMode\":\"none\","
	                           "\"Init\":%s},\"State\":", init);

	return g_strconcat(halves[0], member, halves[1], NULL);
}

/* A container agent's computer, built by the factory from @yaml's alpha. */
static ClawtComputer *
factory_container(
	ClawtPodBridge *bridge,
	const gchar    *yaml
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml, &error);
	ClawtComputer *computer;

	g_assert_no_error(error);
	computer = clawt_computer_factory_create(clawt_config_get_agent(config, "alpha"),
	                                         NULL, bridge, &error);
	g_assert_no_error(error);
	g_assert_nonnull(computer);
	g_assert_true(CLAWT_IS_CONTAINER_COMPUTER(computer));

	return computer;
}

/*
 * The create request says init, both ways: true with the key unset --
 * the default -- and false when the config turns it off.  Asserted on
 * the parameters podomation is handed, since what a module older than
 * podomation v1.0.5 does with them (it ignores the key) is not ours.
 */
static void
test_init_in_create_params(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) by_default = NULL;
	g_autoptr(ClawtComputer) turned_off = NULL;
	g_autoptr(ClawtComputer) bare = NULL;
	g_autoptr(GHashTable) params = NULL;

	by_default = factory_container(bridge,
		"agents:\n"
		"  - id: alpha\n"
		"    computer:\n"
		"      type: container\n");
	g_assert_true(clawt_container_computer_get_init(
		CLAWT_CONTAINER_COMPUTER(by_default)));
	params = clawt_container_computer_build_create_params(
		CLAWT_CONTAINER_COMPUTER(by_default));
	g_assert_cmpstr(g_hash_table_lookup(params, "init"), ==, "true");
	g_clear_pointer(&params, g_hash_table_unref);

	turned_off = factory_container(bridge,
		"agents:\n"
		"  - id: alpha\n"
		"    computer:\n"
		"      type: container\n"
		"      container:\n"
		"        init: false\n");
	g_assert_false(clawt_container_computer_get_init(
		CLAWT_CONTAINER_COMPUTER(turned_off)));
	params = clawt_container_computer_build_create_params(
		CLAWT_CONTAINER_COMPUTER(turned_off));
	g_assert_cmpstr(g_hash_table_lookup(params, "init"), ==, "false");
	g_clear_pointer(&params, g_hash_table_unref);

	/*
	 * An object nobody configured -- the broker's -- is off: its images
	 * run their own entrypoints, and the key is an agent's.
	 */
	bare = clawt_container_computer_new("keeper", bridge, "example:1");
	params = clawt_container_computer_build_create_params(
		CLAWT_CONTAINER_COMPUTER(bare));
	g_assert_cmpstr(g_hash_table_lookup(params, "init"), ==, "false");
}

/*
 * HostConfig.Init against the wanted setting: a difference either way
 * is a recreate, saying which way; the same is not.  A HostConfig with no
 * Init is podman's spelling of false (the upgrade case: every container
 * made before the setting); no HostConfig, or an Init that is not a
 * boolean, cannot be compared and is not a reason.
 */
static void
test_compare_init(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *reason = NULL;
	struct {
		gboolean     wanted;
		const gchar *have;
		const gchar *reason;
	} cases[] = {
		{ TRUE,  "",      "init changed (now on)" },   /* podman's real "off" */
		{ TRUE,  "true",  NULL },
		{ FALSE, "",      NULL },
		{ FALSE, "true",  "init changed (now off)" },
		{ TRUE,  "false", "init changed (now on)" },   /* said outright */
		{ FALSE, "false", NULL },
		{ TRUE,  NULL,    NULL },      /* no HostConfig: cannot tell */
		{ FALSE, NULL,    NULL },
		{ TRUE,  "null",  NULL },      /* not a boolean: cannot tell */
	};
	gsize i;

	computer = keeper_new(bridge, dir);
	base = inspect_json("registry.fedoraproject.org/fedora:44", "", dir,
	                    "/data/source", TRUE);

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *inspected = with_init(base, cases[i].have);

		clawt_container_computer_set_init(CLAWT_CONTAINER_COMPUTER(computer),
		                                  cases[i].wanted);
		reason = reason_for(computer, inspected);
		g_assert_cmpstr(reason, ==, cases[i].reason);
		g_clear_pointer(&reason, g_free);
	}

	clawt_test_remove_tree(dir);
}

/*
 * The upgrade: a kept container made without the init -- inspected as
 * podman prints it, a HostConfig with no Init -- adopted by a daemon that
 * wants it.  Exactly one remove and one create, the reason logged and kept
 * for the Fleet page -- and a container that already has it is kept.
 */
static void
test_recreate_when_init_changed(
	void
){
	const gchar *have[] = { "", "true" };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(have); i++) {
		g_autoptr(ClawtPodBridge) bridge = NULL;
		g_autoptr(ClawtComputer) computer = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
		g_autofree gchar *dir = g_dir_make_tmp("clawt-keep-XXXXXX", NULL);
		g_autofree gchar *base = NULL;
		g_autofree gchar *inspected = NULL;
		gboolean upgrade = (*have[i] == '\0');
		Podman *podman = podman_start();
		guint handler;

		podman->exists = TRUE;
		base = inspect_json("registry.fedoraproject.org/fedora:44", "", dir,
		                    "/data/source", TRUE);
		inspected = with_init(base, have[i]);
		podman_cli_says(podman, inspected);

		computer = fresh_keeper(podman, &bridge, dir);
		if (computer == NULL) {
			podman_stop(podman);
			clawt_test_remove_tree(dir);
			return;
		}
		clawt_container_computer_set_init(CLAWT_CONTAINER_COMPUTER(computer), TRUE);

		handler = g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
		                            capture_messages, seen);
		g_assert_true(clawt_computer_start(computer, &error));
		g_log_remove_handler(G_LOG_DOMAIN, handler);
		g_assert_no_error(error);

		g_assert_cmpuint(podman->removes, ==, upgrade ? 1 : 0);
		g_assert_cmpuint(podman->creates, ==, upgrade ? 1 : 0);
		g_assert_cmpint(clawt_computer_get_state(computer), ==,
		                CLAWT_COMPUTER_STATE_RUNNING);

		if (upgrade) {
			g_assert_cmpuint(messages_matching(
			                     seen, "recreating clawt-keeper: init changed "
			                           "(now on)"), ==, 1);
			g_assert_true(g_pattern_match_simple(
				"recreated clawt-keeper at *: init changed (now on)",
				clawt_container_computer_get_last_recreate(
					CLAWT_CONTAINER_COMPUTER(computer))));
		} else {
			g_assert_cmpuint(messages_matching(seen, "recreating *"), ==, 0);
			g_assert_cmpuint(messages_matching(seen, "*adopted the existing*"),
			                 ==, 1);
			g_assert_null(clawt_container_computer_get_last_recreate(
				CLAWT_CONTAINER_COMPUTER(computer)));
		}

		podman_stop(podman);
		clawt_test_remove_tree(dir);
	}
}

/* What a label says its level is. */
static void
test_level_of_label(
	void
){
	gchar *level;

	level = clawt_selinux_level_of_label(
		"system_u:system_r:container_t:s0:c100,c200");
	g_assert_cmpstr(level, ==, "s0:c100,c200");
	g_free(level);

	level = clawt_selinux_level_of_label("system_u:system_r:container_t:s0:c5");
	g_assert_cmpstr(level, ==, "s0:c5");
	g_free(level);

	g_assert_null(clawt_selinux_level_of_label(NULL));
	g_assert_null(clawt_selinux_level_of_label(""));
	g_assert_null(clawt_selinux_level_of_label(
		"system_u:system_r:spc_t:s0-s0:c0.c1023"));
	g_assert_null(clawt_selinux_level_of_label("unconfined_t"));
}

gint
main(
	gint    argc,
	gchar **argv
){
	g_autofree gchar *path = NULL;
	gint status;

	g_test_init(&argc, &argv, NULL);

	/* Ours first, so the adoption finds the fake and not a real one. */
	cli_dir = g_dir_make_tmp("clawt-keep-cli-XXXXXX", NULL);
	g_assert_nonnull(cli_dir);
	path = g_strdup_printf("%s:%s", cli_dir,
	                       g_getenv("PATH") != NULL ? g_getenv("PATH")
	                                                : "/usr/bin:/bin");
	g_setenv("PATH", path, TRUE);

	g_test_add_func("/container-keep/level-of-label", test_level_of_label);
	g_test_add_func("/container-keep/compare/matching-is-kept",
	                test_compare_matching_is_kept);
	g_test_add_func("/container-keep/compare/image-names",
	                test_compare_image_names);
	g_test_add_func("/container-keep/compare/mounts", test_compare_mounts);
	g_test_add_func("/container-keep/compare/level-and-command",
	                test_compare_level_and_command);
	g_test_add_func("/container-keep/compare/unreadable",
	                test_compare_unreadable);
	g_test_add_func("/container-keep/adopt-matching-container",
	                test_adopt_matching_container);
	g_test_add_func("/container-keep/recreate-when-mounts-changed",
	                test_recreate_when_mounts_changed);
	g_test_add_func("/container-keep/recreate-when-image-changed",
	                test_recreate_when_image_changed);
	g_test_add_func("/container-keep/keep-false-unchanged",
	                test_keep_false_unchanged);
	g_test_add_func("/container-keep/name-taken-is-not-destroyed",
	                test_name_taken_is_not_destroyed);
	g_test_add_func("/container-keep/unreadable-inspect-keeps",
	                test_unreadable_inspect_keeps);
	g_test_add_func("/container-keep/init/create-params",
	                test_init_in_create_params);
	g_test_add_func("/container-keep/init/compare", test_compare_init);
	g_test_add_func("/container-keep/init/recreate-once",
	                test_recreate_when_init_changed);
	g_test_add_func("/container-keep/nothing-there-is-created",
	                test_nothing_there_is_created);

	status = g_test_run();

	clawt_test_remove_tree(cli_dir);
	g_free(cli_dir);

	return status;
}
