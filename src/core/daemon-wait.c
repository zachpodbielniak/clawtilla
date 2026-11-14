/*
 * daemon-wait.c - Looking at the conditions agents are waiting on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * #ClawtWaitManager decides *when* to look; this decides *how*, for each
 * kind, and says what it saw back through clawt_wait_manager_report().
 * The decisions about what an answer means are in clawt-wait.c, where
 * they are tested without any of what follows.
 *
 * Three properties hold for every probe here, and each is a rule this
 * tree learned somewhere else:
 *
 *   **Nothing waits on the loop.**  A file and a task are answered from
 *   memory and a stat; a URL is asynchronous end to end; a command goes
 *   through clawt_computer_exec_async(), the library's one way of
 *   leaving the thread.  The tick is on the daemon's main context, which
 *   every agent's messages share.
 *
 *   **Every asynchronous start names its context.**  A probe is started
 *   from a timer, and dispatching a timer pushes nothing -- so the
 *   daemon's context is pushed around each start here, where the
 *   operation attaches, rather than assumed.
 *
 *   **A URL is fetched as nobody, from nowhere private.**  No
 *   credentials (CLAWT_HTTP_NO_CREDENTIALS), no proxy, and the address
 *   actually connected to -- not merely the one the name resolved to
 *   when it was looked up -- has to pass clawt_net_address_is_public(),
 *   so a name that answers differently the second time it is asked
 *   reaches nothing it should not.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"
#include "clawt-http.h"
#include "clawt-net.h"

#include <string.h>

/* Where the waits live, beside the mailboxes and the handoff queue. */
#define WAIT_DIR_NAME "waits"

/* How long a URL may take, connecting and answering together. */
#define WAIT_URL_TIMEOUT_SECONDS (15)

/* How long a command or a reachability check may run. */
#define WAIT_EXEC_TIMEOUT_SECONDS (45)

/* Read in pieces of this size, so the cap is applied as it fills. */
#define WAIT_READ_CHUNK (64 * 1024)

/* ── Small helpers ───────────────────────────────────────────────── */

static void
report(
	ClawtDaemon      *self,
	ClawtWait        *wait,
	ClawtWaitVerdict  verdict,
	const gchar      *seen,
	const gchar      *quoted
){
	if (self->waits == NULL)
		return;

	clawt_wait_manager_report(self->waits, wait, verdict, seen, quoted,
	                          g_get_real_time());
}

/* ── Files and tasks: answered on the spot ───────────────────────── */

static void
probe_file(
	ClawtDaemon *self,
	ClawtAgent  *agent,
	ClawtWait   *wait
){
	g_autofree gchar *workspace = NULL;
	g_autoptr(GError) error = NULL;
	gboolean exists = FALSE;
	gint64 size = 0;
	gint64 mtime = 0;
	ClawtWaitVerdict verdict;

	workspace = clawt_agent_config_get_workspace(clawt_agent_get_config(agent));

	if (!clawt_wait_stat_in_workspace(workspace, wait->path, &exists, &size,
	                                  &mtime, &error)) {
		/*
		 * A link pointing out of the workspace is a condition that can
		 * never be watched, not a look that failed: ending it says so
		 * once, where backing off would say nothing for a week.
		 */
		report(self, wait,
		       g_error_matches(error, CLAWT_ERROR,
		                       CLAWT_ERROR_PERMISSION_DENIED)
		       ? CLAWT_WAIT_IMPOSSIBLE : CLAWT_WAIT_UNANSWERED,
		       error->message, NULL);
		return;
	}

	verdict = clawt_wait_decide_file(wait, exists, size, mtime);

	report(self, wait, verdict,
	       exists ? (verdict == CLAWT_WAIT_MET ? "the file is there"
	                                           : "the file has not changed")
	              : "the file is not there", NULL);
}

static void
probe_task(
	ClawtDaemon *self,
	ClawtWait   *wait
){
	ClawtTask *task = (self->tasks != NULL)
	                  ? clawt_task_manager_get(self->tasks, wait->task_id)
	                  : NULL;
	g_autofree gchar *seen = NULL;

	if (task == NULL) {
		report(self, wait, clawt_wait_decide_task(wait, FALSE, FALSE),
		       "clawtilla no longer knows this task -- it may have "
		       "settled before a restart; clawtilla_task_status can say",
		       NULL);
		return;
	}

	seen = g_strdup_printf("the task is %s",
	                       clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
	                                          (gint)clawt_task_get_state(task)));

	report(self, wait,
	       clawt_wait_decide_task(wait, TRUE, clawt_task_is_finished(task)),
	       seen, NULL);
}

/* ── Computers and commands: off the thread ──────────────────────── */

typedef struct {
	ClawtDaemon   *daemon;
	ClawtWait     *wait;
	ClawtComputer *computer;
} ExecProbe;

static void
exec_probe_free(
	ExecProbe *probe
){
	g_clear_object(&probe->computer);
	g_clear_pointer(&probe->wait, clawt_wait_unref);
	g_clear_object(&probe->daemon);
	g_free(probe);
}

static void
on_exec_probed(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	ExecProbe *probe = user_data;
	g_autoptr(ClawtExecResult) outcome = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;
	ClawtWaitVerdict verdict;

	outcome = clawt_computer_exec_finish(probe->computer, result,
	                                     &error);

	if (outcome == NULL) {
		verdict = clawt_wait_decide_exec(probe->wait, FALSE, -1);
		seen = g_strdup_printf("the computer did not run it: %s",
		                       error != NULL ? error->message : "no reason");
		report(probe->daemon, probe->wait, verdict, seen, NULL);
		exec_probe_free(probe);
		return;
	}

	verdict = clawt_wait_decide_exec(
		probe->wait, TRUE, clawt_exec_result_get_exit_status(outcome));
	seen = g_strdup_printf("exit status %d",
	                       clawt_exec_result_get_exit_status(outcome));

	/*
	 * The output is quoted only for a command, and only when it ends the
	 * wait: it is what the agent will want to read, and it is a
	 * program's words, which compose_notice escapes.
	 */
	report(probe->daemon, probe->wait, verdict, seen,
	       probe->wait->kind == CLAWT_WAIT_COMMAND
	       ? clawt_exec_result_get_stdout(outcome) : NULL);
	exec_probe_free(probe);
}

static void
probe_exec(
	ClawtDaemon *self,
	ClawtAgent  *agent,
	ClawtWait   *wait
){
	static const gchar *const reachable[] = { "true", NULL };
	ClawtComputer *computer = clawt_agent_get_computer(agent);
	g_auto(GStrv) argv = NULL;
	g_autoptr(GError) error = NULL;
	ExecProbe *probe;

	if (computer == NULL) {
		report(self, wait, clawt_wait_decide_exec(wait, FALSE, -1),
		       "the agent has no computer", NULL);
		return;
	}

	if (wait->kind == CLAWT_WAIT_COMMAND &&
	    !g_shell_parse_argv(wait->command, NULL, &argv, &error)) {
		report(self, wait, CLAWT_WAIT_IMPOSSIBLE, error->message, NULL);
		return;
	}

	probe = g_new0(ExecProbe, 1);
	probe->daemon = g_object_ref(self);
	probe->wait = clawt_wait_ref(wait);
	probe->computer = g_object_ref(computer);

	clawt_computer_exec_async(
		computer,
		(wait->kind == CLAWT_WAIT_COMMAND)
			? (const gchar *const *)argv : reachable,
		NULL, WAIT_EXEC_TIMEOUT_SECONDS, self->main_context, NULL,
		on_exec_probed, probe);
}

/* ── URLs: resolved, checked, fetched as nobody ──────────────────── */

typedef struct {
	ClawtDaemon  *daemon;
	ClawtWait    *wait;
	GCancellable *cancellable;
	SoupSession  *session;
	SoupMessage  *message;
	GInputStream *stream;
	GByteArray   *body;
	gchar        *host;

	/* Why the connection was cut, when the address check cut it. */
	gchar        *refused;
} UrlProbe;

static void
url_probe_free(
	UrlProbe *probe
){
	if (probe->message != NULL)
		g_signal_handlers_disconnect_by_data(probe->message, probe);

	g_clear_object(&probe->stream);
	g_clear_object(&probe->message);
	g_clear_object(&probe->session);
	g_clear_object(&probe->cancellable);
	g_clear_pointer(&probe->body, g_byte_array_unref);
	g_clear_pointer(&probe->wait, clawt_wait_unref);
	g_clear_object(&probe->daemon);
	g_free(probe->host);
	g_free(probe->refused);
	g_free(probe);
}

static void
url_probe_finish(
	UrlProbe         *probe,
	ClawtWaitVerdict  verdict,
	const gchar      *seen
){
	report(probe->daemon, probe->wait, verdict, seen, NULL);
	url_probe_free(probe);
}

/*
 * The address a connection actually reached, checked before a byte of
 * the request is written.
 *
 * libsoup re-emits the socket client's events on the message, and
 * CONNECTED carries the plain socket connection -- TLS, and with it the
 * request, comes after.  Checking here rather than only after the
 * lookup is what closes DNS rebinding: a name that resolved to a public
 * address for the check and to 169.254.169.254 for the connect is
 * caught at the connect.  A redirect to a private host opens a new
 * connection and is caught the same way.
 */
static void
on_network_event(
	SoupMessage        *message,
	GSocketClientEvent  event,
	GIOStream          *connection,
	gpointer            user_data
){
	UrlProbe *probe = user_data;
	g_autoptr(GSocketAddress) remote = NULL;
	GInetAddress *address;

	(void)message;

	if (event != G_SOCKET_CLIENT_CONNECTED ||
	    !G_IS_SOCKET_CONNECTION(connection))
		return;

	remote = g_socket_connection_get_remote_address(
		G_SOCKET_CONNECTION(connection), NULL);

	if (remote == NULL || !G_IS_INET_SOCKET_ADDRESS(remote)) {
		g_free(probe->refused);
		probe->refused = g_strdup("the connection reached something that "
		                          "is not an internet address");
		g_cancellable_cancel(probe->cancellable);
		return;
	}

	address = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote));

	if (!clawt_net_address_is_public(address)) {
		g_autofree gchar *text = g_inet_address_to_string(address);

		g_free(probe->refused);
		probe->refused = g_strdup_printf(
			"%s reached the private or reserved address %s, which "
			"clawtilla does not fetch from", probe->host, text);
		g_cancellable_cancel(probe->cancellable);
	}
}

static void
url_decide(
	UrlProbe *probe
){
	guint status = soup_message_get_status(probe->message);
	g_autofree gchar *body = NULL;
	g_autofree gchar *seen = NULL;
	ClawtWaitVerdict verdict;

	if (probe->body != NULL)
		body = g_strndup((const gchar *)probe->body->data, probe->body->len);

	verdict = clawt_wait_decide_url(probe->wait, status, body);

	if (probe->wait->contains != NULL && *probe->wait->contains != '\0')
		seen = g_strdup_printf("HTTP %u, %s", status,
		                       verdict == CLAWT_WAIT_MET
		                       ? "and the text is there"
		                       : "the text is not there (or the status is "
		                         "not the one wanted)");
	else
		seen = g_strdup_printf("HTTP %u", status);

	url_probe_finish(probe, verdict, seen);
}

static void url_read_more(UrlProbe *probe);

static void
on_url_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	UrlProbe *probe = user_data;
	g_autoptr(GBytes) chunk = NULL;
	g_autoptr(GError) error = NULL;

	chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
	                                         &error);

	if (chunk == NULL) {
		g_autofree gchar *seen = g_strdup_printf(
			"the page stopped arriving: %s", error->message);

		url_probe_finish(probe, CLAWT_WAIT_UNANSWERED, seen);
		return;
	}

	if (g_bytes_get_size(chunk) == 0) {
		url_decide(probe);
		return;
	}

	g_byte_array_append(probe->body, g_bytes_get_data(chunk, NULL),
	                    (guint)g_bytes_get_size(chunk));

	/*
	 * The cap is applied as the body fills, and what is past it is never
	 * read: a wait pointed at a large download decides on its first two
	 * megabytes, which is where a status page says what it says.
	 */
	if (probe->body->len >= CLAWT_WAIT_MAX_BODY_BYTES) {
		g_byte_array_set_size(probe->body, CLAWT_WAIT_MAX_BODY_BYTES);
		url_decide(probe);
		return;
	}

	url_read_more(probe);
}

static void
url_read_more(
	UrlProbe *probe
){
	if (probe->daemon->main_context != NULL)
		g_main_context_push_thread_default(probe->daemon->main_context);

	g_input_stream_read_bytes_async(probe->stream, WAIT_READ_CHUNK,
	                                G_PRIORITY_DEFAULT, probe->cancellable,
	                                on_url_read, probe);

	if (probe->daemon->main_context != NULL)
		g_main_context_pop_thread_default(probe->daemon->main_context);
}

static void
on_url_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	UrlProbe *probe = user_data;
	g_autoptr(GError) error = NULL;

	probe->stream = soup_session_send_finish(SOUP_SESSION(source), result,
	                                         &error);

	if (probe->stream == NULL) {
		if (probe->refused != NULL) {
			g_autofree gchar *seen = g_strdup(probe->refused);

			url_probe_finish(probe, CLAWT_WAIT_IMPOSSIBLE, seen);
			return;
		}

		{
			g_autofree gchar *seen = g_strdup_printf("no answer: %s",
			                                         error->message);

			url_probe_finish(probe, CLAWT_WAIT_UNANSWERED, seen);
		}
		return;
	}

	/* Without text to look for, the status is the whole answer. */
	if (probe->wait->contains == NULL || *probe->wait->contains == '\0') {
		url_decide(probe);
		return;
	}

	probe->body = g_byte_array_new();
	url_read_more(probe);
}

static void
url_send(
	UrlProbe *probe
){
	GMainContext *context = probe->daemon->main_context;

	/*
	 * A session per look, built with the daemon's context pushed: a
	 * SoupSession belongs to whichever context was thread-default when
	 * it was made, and one made under the wrong one sends its request
	 * and never reads the answer.  Per look rather than shared, so no
	 * pooled connection is reused past the check that admitted it, and
	 * no cookie from one wait's site reaches the next.
	 *
	 * The proxy resolver is cleared outright.  A configured proxy is a
	 * machine on the operator's network, which is exactly where this
	 * request must not go on an agent's behalf -- and the address check
	 * would refuse it anyway, with a message about the wrong thing.
	 */
	if (context != NULL)
		g_main_context_push_thread_default(context);

	probe->session = soup_session_new_with_options(
		"timeout", (guint)WAIT_URL_TIMEOUT_SECONDS,
		"idle-timeout", (guint)WAIT_URL_TIMEOUT_SECONDS,
		"user-agent", "clawtilla-wait",
		"proxy-resolver", NULL,
		NULL);
	probe->message = clawt_http_message_new("GET", probe->wait->url,
	                                        CLAWT_HTTP_NO_CREDENTIALS);

	if (probe->message == NULL) {
		if (context != NULL)
			g_main_context_pop_thread_default(context);

		url_probe_finish(probe, CLAWT_WAIT_IMPOSSIBLE,
		                 "the URL cannot be requested");
		return;
	}

	g_signal_connect(probe->message, "network-event",
	                 G_CALLBACK(on_network_event), probe);

	soup_session_send_async(probe->session, probe->message,
	                        G_PRIORITY_DEFAULT, probe->cancellable,
	                        on_url_sent, probe);

	if (context != NULL)
		g_main_context_pop_thread_default(context);
}

/*
 * Every address the name has, checked before anything is sent.
 *
 * Every one, not the first: a resolver hands them back in an order it
 * chooses, and a connection may try them all.  A name with one private
 * address among public ones is refused whole.  The connect-time check
 * in on_network_event() is what makes this safe against a name that
 * answers differently the second time; this one is what lets the
 * refusal name the address and come back at once.
 */
static void
on_url_resolved(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	UrlProbe *probe = user_data;
	g_autoptr(GError) error = NULL;
	GList *addresses;
	GList *l;

	addresses = g_resolver_lookup_by_name_finish(G_RESOLVER(source), result,
	                                             &error);

	if (addresses == NULL) {
		g_autofree gchar *seen = g_strdup_printf(
			"%s did not resolve: %s", probe->host,
			error != NULL ? error->message : "no addresses");

		url_probe_finish(probe, CLAWT_WAIT_UNANSWERED, seen);
		return;
	}

	for (l = addresses; l != NULL; l = l->next) {
		GInetAddress *address = l->data;

		if (!clawt_net_address_is_public(address)) {
			g_autofree gchar *text = g_inet_address_to_string(address);
			g_autofree gchar *seen = g_strdup_printf(
				"%s resolves to the private or reserved address %s, which "
				"clawtilla does not fetch from", probe->host, text);

			g_resolver_free_addresses(addresses);
			url_probe_finish(probe, CLAWT_WAIT_IMPOSSIBLE, seen);
			return;
		}
	}

	g_resolver_free_addresses(addresses);
	url_send(probe);
}

static void
probe_url(
	ClawtDaemon *self,
	ClawtWait   *wait
){
	g_autofree gchar *refusal = clawt_net_url_refusal(wait->url);
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GResolver) resolver = NULL;
	UrlProbe *probe;

	/*
	 * Checked again, though the tool checked it when the wait was set:
	 * a record on disk is only as trustworthy as whoever last wrote the
	 * directory, and the rule may have tightened since.
	 */
	if (refusal != NULL) {
		report(self, wait, CLAWT_WAIT_IMPOSSIBLE, refusal, NULL);
		return;
	}

	/*
	 * And the agent's network, which may have been gated since the wait
	 * was set: a gated or closed agent's URL is never fetched by the
	 * daemon, which would be a request on its behalf around the gate.
	 */
	{
		ClawtAgent *agent = (self->agents != NULL)
			? clawt_agent_manager_get(self->agents, wait->agent_id) : NULL;
		const gchar *gated = (agent != NULL)
			? clawt_egress_url_wait_refusal(clawt_enum_to_nick(
				  CLAWT_TYPE_RUNTIME_NETWORK,
				  clawt_agent_config_get_enum(
				      clawt_agent_get_config(agent), "runtime.network")))
			: NULL;

		if (gated != NULL) {
			report(self, wait, CLAWT_WAIT_IMPOSSIBLE, gated, NULL);
			return;
		}
	}

	uri = g_uri_parse(wait->url, G_URI_FLAGS_NONE, NULL);

	if (uri == NULL || g_uri_get_host(uri) == NULL) {
		report(self, wait, CLAWT_WAIT_IMPOSSIBLE, "the URL has no host",
		       NULL);
		return;
	}

	probe = g_new0(UrlProbe, 1);
	probe->daemon = g_object_ref(self);
	probe->wait = clawt_wait_ref(wait);
	probe->cancellable = g_cancellable_new();
	probe->host = g_strdup(g_uri_get_host(uri));

	if (g_hostname_is_ip_address(probe->host)) {
		url_send(probe);
		return;
	}

	resolver = g_resolver_get_default();

	if (self->main_context != NULL)
		g_main_context_push_thread_default(self->main_context);

	g_resolver_lookup_by_name_async(resolver, probe->host, probe->cancellable,
	                                on_url_resolved, probe);

	if (self->main_context != NULL)
		g_main_context_pop_thread_default(self->main_context);
}

/* ── The probe the manager calls ─────────────────────────────────── */

static void
on_probe(
	ClawtWaitManager *manager,
	ClawtWait        *wait,
	gpointer          user_data
){
	ClawtDaemon *self = user_data;
	ClawtAgent *agent = (self->agents != NULL)
	                    ? clawt_agent_manager_get(self->agents, wait->agent_id)
	                    : NULL;

	/*
	 * An agent removed from the fleet takes its waits with it, telling
	 * nobody -- there is nobody left to tell, and a notice queued into a
	 * mailbox that no longer exists is an error in the log every minute.
	 */
	if (agent == NULL) {
		g_message("waits: %s is no longer in the fleet; dropping its waits",
		          wait->agent_id);
		clawt_wait_manager_drop_agent(manager, wait->agent_id);
		return;
	}

	/*
	 * Every kind named, no default: a kind added without a probe here
	 * is a -Wswitch warning rather than a wait that is never looked at
	 * and sits "in flight" until it expires.
	 */
	switch (wait->kind) {
	case CLAWT_WAIT_URL:
		probe_url(self, wait);
		return;

	case CLAWT_WAIT_FILE:
		probe_file(self, agent, wait);
		return;

	case CLAWT_WAIT_TASK:
		probe_task(self, wait);
		return;

	case CLAWT_WAIT_COMPUTER:
	case CLAWT_WAIT_COMMAND:
		probe_exec(self, agent, wait);
		return;
	}

	report(self, wait, CLAWT_WAIT_IMPOSSIBLE, "an unknown kind of wait",
	       NULL);
}

/* ── Telling the agent ───────────────────────────────────────────── */

/*
 * Sends the notice into the room the wait was set from.
 *
 * As the daemon, with the exchange closed, addressed to the agent alone
 * and at depth zero -- the same shape as a settled task's notice, and
 * for the same reasons: nobody asked this turn into being, so
 * clawtilla_message_user is open to it, and a wait ending is not a hop
 * in anybody's chain.
 */
static void
on_wait_ended(
	ClawtWaitManager *manager,
	ClawtWait        *wait,
	gpointer          user_data
){
	ClawtDaemon *self = user_data;
	g_autoptr(ClawtMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL;
	ClawtRoom *room = NULL;

	(void)manager;

	if (self->router == NULL || self->rooms == NULL)
		return;

	if (clawt_agent_manager_get(self->agents, wait->agent_id) == NULL)
		return;

	room = clawt_room_manager_get(self->rooms, wait->room_id);

	/*
	 * A room deleted while the wait ran falls back to the operator's
	 * conversation with the agent: the notice has to land somewhere the
	 * agent will read it, and a wait ending unannounced is a wait the
	 * agent believes is still running.
	 */
	if (room == NULL)
		room = clawt_room_manager_get_direct(self->rooms, "user",
		                                     wait->agent_id);

	if (room == NULL)
		return;

	body = clawt_wait_compose_notice(wait, g_get_real_time());
	message = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER,
	                            body);
	clawt_message_set_depth(message, 0);
	clawt_message_set_invites_reply(message, FALSE);
	clawt_message_set_only_for(message, wait->agent_id);

	if (clawt_mailbox_router_send(self->router, message, &error) < 0)
		g_warning("waits: %s's wait %s ended %s and it could not be told: "
		          "%s", wait->agent_id, wait->id,
		          clawt_wait_state_nick(wait->state),
		          error != NULL ? error->message : "no reason");
}

/*
 * One event per change that a person could see, for the clients'
 * "Waiting for" lists.  The manager already declines to announce a look
 * that saw what the last one saw.
 */
static void
on_waits_changed(
	ClawtWaitManager *manager,
	const gchar      *agent_id,
	const gchar      *wait_id,
	gpointer          user_data
){
	ClawtDaemon *self = user_data;
	ClawtWait *wait = clawt_wait_manager_get(manager, wait_id);
	ClawtEvent *event;

	if (self->bus == NULL)
		return;

	event = clawt_event_new("wait.changed", agent_id);
	clawt_event_set_detail(event, "wait", wait_id);
	clawt_event_set_detail(event, "state",
	                       wait != NULL ? clawt_wait_state_nick(wait->state)
	                                    : "ended");
	clawt_event_bus_publish(self->bus, event);
	clawt_event_free(event);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

void
clawt_daemon_wait_setup(
	ClawtDaemon *self
){
	g_autofree gchar *dir = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->state_dir == NULL || self->waits != NULL)
		return;

	dir = g_build_filename(self->state_dir, WAIT_DIR_NAME, NULL);
	self->waits = clawt_wait_manager_new(dir, &error);

	if (self->waits == NULL) {
		/*
		 * A warning rather than a failed start: a fleet without waits
		 * is a fleet where three tools are not offered, which
		 * clawt_mcp_tools_is_permitted() already arranges when the
		 * manager is unset.
		 */
		g_warning("waits: could not open %s, so waits are not available: "
		          "%s", dir, error != NULL ? error->message : "unknown");
		return;
	}

	clawt_wait_manager_set_probe_func(self->waits, on_probe, self, NULL);
	clawt_wait_manager_set_end_func(self->waits, on_wait_ended, self, NULL);
	self->waits_changed_handler = g_signal_connect(
		self->waits, "changed", G_CALLBACK(on_waits_changed), self);

	if (self->mcp_tools != NULL)
		clawt_mcp_tools_set_waits(self->mcp_tools, self->waits);

	clawt_wait_manager_start(self->waits, self->main_context);
}

void
clawt_daemon_wait_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->waits == NULL)
		return;

	/*
	 * The tools lose their pointer first, or a call arriving during
	 * shutdown reads a freed one.  A probe still out holds its own
	 * reference to the daemon and to its wait, and reports into a
	 * manager it no longer reaches -- report() checks.
	 */
	if (self->mcp_tools != NULL)
		clawt_mcp_tools_set_waits(self->mcp_tools, NULL);

	clawt_wait_manager_stop(self->waits);
	clawt_wait_manager_set_probe_func(self->waits, NULL, NULL, NULL);
	clawt_wait_manager_set_end_func(self->waits, NULL, NULL, NULL);

	if (self->waits_changed_handler != 0)
		g_signal_handler_disconnect(self->waits, self->waits_changed_handler);
	self->waits_changed_handler = 0;

	g_clear_object(&self->waits);
}

/* ── The client surface ──────────────────────────────────────────── */

static void
add_wait_object(
	JsonBuilder *builder,
	ClawtWait   *wait
){
	g_autofree gchar *condition = clawt_wait_describe(wait);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, wait->id);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, wait->agent_id);
	json_builder_set_member_name(builder, "room");
	json_builder_add_string_value(builder, wait->room_id);
	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, wait->label);

	if (wait->note != NULL) {
		json_builder_set_member_name(builder, "note");
		json_builder_add_string_value(builder, wait->note);
	}

	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder, clawt_wait_kind_nick(wait->kind));
	json_builder_set_member_name(builder, "condition");
	json_builder_add_string_value(builder, condition);
	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder, clawt_wait_state_nick(wait->state));

	/* Microseconds, like every other stamp a client renders. */
	json_builder_set_member_name(builder, "created_at");
	json_builder_add_int_value(builder, wait->created_at);
	json_builder_set_member_name(builder, "last_check_at");
	json_builder_add_int_value(builder, wait->last_check_at);
	json_builder_set_member_name(builder, "next_check_at");
	json_builder_add_int_value(builder, wait->next_check_at);
	json_builder_set_member_name(builder, "expires_at");
	json_builder_add_int_value(builder, wait->expires_at);
	json_builder_set_member_name(builder, "checks");
	json_builder_add_int_value(builder, (gint64)wait->checks);
	json_builder_set_member_name(builder, "failures");
	json_builder_add_int_value(builder, (gint64)wait->failures);

	if (wait->last_result != NULL) {
		json_builder_set_member_name(builder, "last_result");
		json_builder_add_string_value(builder, wait->last_result);
	}

	json_builder_end_object(builder);
}

JsonNode *
clawt_daemon_handle_wait(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(JsonBuilder) builder = NULL;

	*handled = TRUE;

	if (g_strcmp0(kind, "wait.list") == 0) {
		const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
		g_autoptr(GPtrArray) waits = NULL;
		guint i;

		if (self->waits == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
			                           "this daemon keeps no waits");

		waits = clawt_wait_manager_list(self->waits, agent_id);
		builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "waits");
		json_builder_begin_array(builder);

		for (i = 0; i < waits->len; i++)
			add_wait_object(builder, g_ptr_array_index(waits, i));

		json_builder_end_array(builder);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "wait.cancel") == 0) {
		const gchar *id = clawt_ipc_payload_string(payload, "wait");
		g_autoptr(GError) error = NULL;

		if (self->waits == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
			                           "this daemon keeps no waits");

		if (id == NULL || *id == '\0')
			return clawt_ipc_error_new(request,
			                           CLAWT_ERROR_INVALID_ARGUMENT,
			                           "cancelling needs a wait");

		/*
		 * As the operator: any agent's wait, and the agent is told,
		 * because it would otherwise go on believing it was waiting.
		 */
		if (!clawt_wait_manager_cancel(self->waits, id, NULL,
		                               g_get_real_time(), &error))
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			                           error->message);

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "cancelled");
		json_builder_add_string_value(builder, id);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	*handled = FALSE;
	return NULL;
}
