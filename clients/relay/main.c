/*
 * main.c - clawtilla-relay: the in-box end of a gated agent's network
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A gated agent's sandbox is a network namespace with nothing in it but
 * loopback.  The daemon's egress gate and provider-key broker are unix
 * sockets bound into the box; this program listens on loopback ports
 * inside the namespace and bridges each connection to one of them, then
 * runs the agent's own process and exits with its status.
 *
 * It is the payload bwrap starts, and libreclaw is its child, so it
 * forwards the signals a stop sends (bwrap forwards none), and the
 * daemon looks past it, by its process name, for libreclaw itself.
 *
 * It links GIO and nothing of clawtilla's: it runs inside every gated
 * box, and the less it brings the less the box has to contain.
 */

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define RELAY_LICENSE \
	"clawtilla-relay - part of clawtilla\n" \
	"Copyright (C) 2026\n" \
	"\n" \
	"This program is free software: you can redistribute it and/or modify\n" \
	"it under the terms of the GNU Affero General Public License as\n" \
	"published by the Free Software Foundation, either version 3 of the\n" \
	"License, or (at your option) any later version.\n" \
	"\n" \
	"This program is distributed in the hope that it will be useful,\n" \
	"but WITHOUT ANY WARRANTY; without even the implied warranty of\n" \
	"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n" \
	"GNU Affero General Public License for more details.\n" \
	"\n" \
	"You should have received a copy of the GNU Affero General Public\n" \
	"License along with this program.  If not, see\n" \
	"<https://www.gnu.org/licenses/>.\n"

/* One loopback port and the socket it is bridged to. */
typedef struct {
	gchar          *socket_path;
	GSocketService *service;
} Forward;

/* One bridged connection. */
typedef struct {
	GSocketConnection *inside;
	GSocketConnection *outside;
} Bridge;

static GMainLoop   *loop;
static GSubprocess *child;
static gint         exit_status = 1;

static void
bridge_free(
	Bridge *bridge
){
	g_clear_object(&bridge->inside);
	g_clear_object(&bridge->outside);
	g_free(bridge);
}

static void
on_spliced(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Bridge *bridge = user_data;

	g_io_stream_splice_finish(result, NULL);
	bridge_free(bridge);
}

static void
on_connected(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Bridge *bridge = user_data;
	g_autoptr(GError) error = NULL;

	bridge->outside = g_socket_client_connect_finish(G_SOCKET_CLIENT(source),
	                                                 result, &error);

	/*
	 * The daemon's end is gone: the connection inside is closed, which
	 * the client sees as the proxy refusing -- the truth, and the safe
	 * direction.  Said on stderr, which is the agent's log.
	 */
	if (bridge->outside == NULL) {
		g_printerr("clawtilla-relay: %s\n", error->message);
		bridge_free(bridge);
		return;
	}

	g_io_stream_splice_async(G_IO_STREAM(bridge->inside),
	                         G_IO_STREAM(bridge->outside),
	                         G_IO_STREAM_SPLICE_CLOSE_STREAM1 |
	                         G_IO_STREAM_SPLICE_CLOSE_STREAM2 |
	                         G_IO_STREAM_SPLICE_WAIT_FOR_BOTH,
	                         G_PRIORITY_DEFAULT, NULL, on_spliced, bridge);
}

static gboolean
on_incoming(
	GSocketService    *service,
	GSocketConnection *connection,
	GObject           *source_object,
	gpointer           user_data
){
	Forward *forward = user_data;
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketAddress) address =
		g_unix_socket_address_new(forward->socket_path);
	Bridge *bridge = g_new0(Bridge, 1);

	bridge->inside = g_object_ref(connection);

	/* A unix socket needs no proxy, and must never be given one. */
	g_socket_client_set_enable_proxy(client, FALSE);
	g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(address),
	                              NULL, on_connected, bridge);

	return TRUE;
}

/*
 * "127.0.0.1:3128=/path/to.sock".  Loopback only: the namespace has
 * nothing else, and a relay that could be told to listen anywhere would
 * be a relay somebody could point at the host's own interfaces if it
 * were ever run outside a box.
 */
static Forward *
forward_new(
	const gchar  *spec,
	GError      **error
){
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GInetAddress) loopback = NULL;
	g_autofree gchar *host = NULL;
	const gchar *equals = strchr(spec, '=');
	const gchar *colon;
	guint64 port = 0;
	Forward *forward;

	if (equals == NULL || equals[1] != '/') {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "--forward takes ADDRESS:PORT=/path/to/socket, not '%s'",
		            spec);
		return NULL;
	}

	colon = memchr(spec, ':', (gsize)(equals - spec));

	if (colon == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "--forward '%s' has no port", spec);
		return NULL;
	}

	host = g_strndup(spec, (gsize)(colon - spec));

	{
		g_autofree gchar *port_text =
			g_strndup(colon + 1, (gsize)(equals - colon - 1));

		if (!g_ascii_string_to_unsigned(port_text, 10, 1, 65535, &port,
		                                error))
			return NULL;
	}

	loopback = g_inet_address_new_from_string(host);

	if (loopback == NULL || !g_inet_address_get_is_loopback(loopback)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "--forward listens on loopback only, not '%s'", host);
		return NULL;
	}

	forward = g_new0(Forward, 1);
	forward->socket_path = g_strdup(equals + 1);
	forward->service = g_socket_service_new();
	address = g_inet_socket_address_new(loopback, (guint16)port);

	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(forward->service),
	                                   address, G_SOCKET_TYPE_STREAM,
	                                   G_SOCKET_PROTOCOL_TCP, NULL, NULL,
	                                   error)) {
		g_object_unref(forward->service);
		g_free(forward->socket_path);
		g_free(forward);
		return NULL;
	}

	g_signal_connect(forward->service, "incoming", G_CALLBACK(on_incoming),
	                 forward);

	return forward;
}

static void
die_with_parent(
	gpointer user_data
){
#if defined(__linux__)
	prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
}

static gboolean
on_signal(
	gpointer user_data
){
	gint signum = GPOINTER_TO_INT(user_data);

	if (child != NULL)
		g_subprocess_send_signal(child, signum);

	return G_SOURCE_CONTINUE;
}

static void
on_child_exited(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GSubprocess *process = G_SUBPROCESS(source);

	g_subprocess_wait_finish(process, result, NULL);

	if (g_subprocess_get_if_exited(process))
		exit_status = g_subprocess_get_exit_status(process);
	else if (g_subprocess_get_if_signaled(process))
		exit_status = 128 + g_subprocess_get_term_sig(process);

	g_main_loop_quit(loop);
}

int
main(
	int    argc,
	char **argv
){
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) forwards = NULL;
	g_auto(GStrv) command = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	gboolean license = FALSE;
	GPtrArray *services;
	guint i;
	GOptionEntry entries[] = {
		{ "forward", 'f', 0, G_OPTION_ARG_STRING_ARRAY, NULL,
		  "Listen on ADDRESS:PORT (loopback) and bridge each connection "
		  "to the unix socket SOCKET", "ADDRESS:PORT=SOCKET" },
		{ "license", 0, 0, G_OPTION_ARG_NONE, NULL,
		  "Print the license and exit", NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, NULL,
		  NULL, "-- PROGRAM [ARGUMENT...]" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

	entries[0].arg_data = &forwards;
	entries[1].arg_data = &license;
	entries[2].arg_data = &command;

	context = g_option_context_new(NULL);
	g_option_context_set_summary(context,
		"Bridge loopback ports inside a gated agent's network namespace to "
		"clawtilla's egress gate and provider-key broker, then run the "
		"agent.\n"
		"\n"
		"clawtillad starts this inside the bubblewrap box of an agent with "
		"runtime.network: gated or none; it is not meant to be run by "
		"hand.");
	g_option_context_set_description(context,
		"Examples:\n"
		"  clawtilla-relay --forward 127.0.0.1:3128=/run/gate.sock \\\n"
		"      --forward 127.0.0.1:3129=/run/broker.sock \\\n"
		"      -- libreclaw -c config.yaml\n"
		"  clawtilla-relay --license\n");
	g_option_context_add_main_entries(context, entries, NULL);

	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("clawtilla-relay: %s\n", error->message);
		return 2;
	}

	if (license) {
		fputs(RELAY_LICENSE, stdout);
		return 0;
	}

	if (command == NULL || command[0] == NULL) {
		g_printerr("clawtilla-relay: nothing to run; give the program "
		           "after --\n");
		return 2;
	}

	/*
	 * Every port is bound before the agent starts.  A relay that ran the
	 * agent with a port missing would hand it proxy variables naming
	 * nothing -- an agent that cannot reach its provider and no line
	 * anywhere saying why.
	 */
	services = g_ptr_array_new();

	for (i = 0; forwards != NULL && forwards[i] != NULL; i++) {
		Forward *forward = forward_new(forwards[i], &error);

		if (forward == NULL) {
			g_printerr("clawtilla-relay: %s\n", error->message);
			return 1;
		}

		g_ptr_array_add(services, forward);
	}

	loop = g_main_loop_new(NULL, FALSE);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
	g_subprocess_launcher_set_child_setup(launcher, die_with_parent, NULL,
	                                      NULL);
	child = g_subprocess_launcher_spawnv(launcher,
	                                     (const gchar * const *)command,
	                                     &error);

	if (child == NULL) {
		g_printerr("clawtilla-relay: starting %s: %s\n", command[0],
		           error->message);
		return 1;
	}

	g_unix_signal_add(SIGTERM, on_signal, GINT_TO_POINTER(SIGTERM));
	g_unix_signal_add(SIGINT, on_signal, GINT_TO_POINTER(SIGINT));
	g_unix_signal_add(SIGHUP, on_signal, GINT_TO_POINTER(SIGHUP));

	g_subprocess_wait_async(child, NULL, on_child_exited, NULL);
	g_main_loop_run(loop);

	g_clear_object(&child);
	g_main_loop_unref(loop);

	/* The process is ending; the services go with it. */
	g_ptr_array_unref(services);

	return exit_status;
}
